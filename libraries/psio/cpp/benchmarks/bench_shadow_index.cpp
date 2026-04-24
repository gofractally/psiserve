// Shadow-index over pack_bin vs fracpack view vs full unpack.
//
// Two phases benchmarked separately:
//   Phase 1 (decode): validate_frac  |  build_shadow_index  |  from_bin
//   Phase 2 (read):   frac_view      |  shadow_view         |  native struct
//
// The decode cost for the view paths happens once per buffer; the reads can
// then be repeated many times. Phase 1 and Phase 2 together == end-to-end.
//
// Matrix across a variety of shapes (same as bench_fracpack.cpp).
//
// Build:
//   cmake -B build/Release -G Ninja -DCMAKE_BUILD_TYPE=Release \
//         -DPSIO_ENABLE_BENCHMARKS=ON
//   cmake --build build/Release --target psio_bench_shadow
// Run:
//   ./build/Release/bin/psio_bench_shadow

#include <psio/frac_ref.hpp>
#include <psio/fracpack.hpp>
#include <psio/from_bin.hpp>
#include <psio/to_bin.hpp>

#include <cassert>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// ── Anti-DCE helpers ──────────────────────────────────────────────────────

template <typename T>
inline void do_not_optimize(T const& val)
{
   asm volatile("" : : "r,m"(val) : "memory");
}
inline void clobber_memory()
{
   asm volatile("" ::: "memory");
}

// ══════════════════════════════════════════════════════════════════════════
// Schemas (identical to bench_fracpack.cpp)
// ══════════════════════════════════════════════════════════════════════════

struct BPoint
{
   double x;
   double y;
};
PSIO_REFLECT(BPoint, definitionWillNotChange(), x, y)

struct Token
{
   std::uint16_t kind;
   std::uint32_t offset;
   std::uint32_t length;
   std::string   text;
};
PSIO_REFLECT(Token, definitionWillNotChange(), kind, offset, length, text)

struct UserProfile
{
   std::uint64_t              id;
   std::string                name;
   std::string                email;
   std::optional<std::string> bio;
   std::uint32_t              age;
   double                     score;
   std::vector<std::string>   tags;
   bool                       verified;
};
PSIO_REFLECT(UserProfile,
             definitionWillNotChange(),
             id, name, email, bio, age, score, tags, verified)

struct LineItem
{
   std::string   product;
   std::uint32_t qty;
   double        unit_price;
};
PSIO_REFLECT(LineItem, definitionWillNotChange(), product, qty, unit_price)

struct Order
{
   std::uint64_t              id;
   UserProfile                customer;
   std::vector<LineItem>      items;
   double                     total;
   std::optional<std::string> note;
};
PSIO_REFLECT(Order, definitionWillNotChange(), id, customer, items, total, note)

struct SensorReading
{
   std::uint64_t                timestamp;
   std::string                  device_id;
   double                       temp, humidity, pressure;
   double                       accel_x, accel_y, accel_z;
   double                       gyro_x, gyro_y, gyro_z;
   double                       mag_x, mag_y, mag_z;
   float                        battery;
   std::int16_t                 signal_dbm;
   std::optional<std::uint32_t> error_code;
   std::string                  firmware;
};
PSIO_REFLECT(SensorReading,
             definitionWillNotChange(),
             timestamp,
             device_id,
             temp,
             humidity,
             pressure,
             accel_x,
             accel_y,
             accel_z,
             gyro_x,
             gyro_y,
             gyro_z,
             mag_x,
             mag_y,
             mag_z,
             battery,
             signal_dbm,
             error_code,
             firmware)

// ── Factories ────────────────────────────────────────────────────────────

static BPoint  make_point() { return {3.14159265358979, 2.71828182845905}; }
static Token   make_token() { return {42, 1024, 15, "identifier_name"}; }
static UserProfile make_user()
{
   return {
       123456789ULL,
       "Alice Johnson",
       "alice@example.com",
       std::string("Software engineer interested in distributed systems and WebAssembly"),
       32,
       98.5,
       {"developer", "wasm", "c++", "open-source"},
       true,
   };
}
static LineItem make_line_item(int i)
{
   return {
       "Product-" + std::to_string(i),
       static_cast<std::uint32_t>(i + 1),
       19.99 + i * 5.0,
   };
}
static Order make_order()
{
   std::vector<LineItem> items;
   for (int i = 0; i < 5; ++i)
      items.push_back(make_line_item(i));
   return {
       987654321ULL,
       make_user(),
       std::move(items),
       199.95,
       std::string("Please ship before Friday"),
   };
}
static SensorReading make_sensor()
{
   return {
       1700000000000ULL, "sensor-alpha-42", 23.5,  65.2,  1013.25,  0.01,  -0.02, 9.81,
       0.001,            -0.003,            0.002, 25.1,  -12.3,    42.7,  3.7f,  -65,
       std::nullopt,     "v2.3.1-rc4",
   };
}

// ══════════════════════════════════════════════════════════════════════════
// Shadow index implementations — one per schema
// ══════════════════════════════════════════════════════════════════════════

namespace sidx
{
   inline std::uint32_t read_varuint32(const char* src, std::uint32_t& pos)
   {
      std::uint32_t r = 0;
      int           s = 0;
      while (true)
      {
         std::uint8_t b = static_cast<std::uint8_t>(src[pos++]);
         r |= std::uint32_t(b & 0x7f) << s;
         if (!(b & 0x80))
            return r;
         s += 7;
      }
   }

   // ── Bounds-checked primitives (used by build_checked variants) ─────────

   // Returns false on overrun or varuint encoding longer than 5 bytes.
   inline bool safe_read_varuint32(const char* src, std::uint32_t& pos, std::size_t end,
                                    std::uint32_t& out)
   {
      std::uint32_t r = 0;
      int           s = 0;
      for (int i = 0; i < 5; ++i)
      {
         if (pos >= end)
            return false;
         std::uint8_t b = static_cast<std::uint8_t>(src[pos++]);
         r |= std::uint32_t(b & 0x7f) << s;
         if (!(b & 0x80))
         {
            out = r;
            return true;
         }
         s += 7;
      }
      return false;  // varuint32 max is 5 bytes
   }

   // Check that `pos + n` is within `end`, using safe arithmetic.
   inline bool room_for(std::uint32_t pos, std::size_t n, std::size_t end)
   {
      return pos <= end && n <= end - pos;
   }

   // BPoint — no slots needed
   struct BPointIndex { const char* base = nullptr; };
   inline BPointIndex build(const BPoint*, const char* src, std::size_t) { return {src}; }
   inline bool build_checked(const BPoint*, const char* src, std::size_t size, BPointIndex& idx)
   {
      if (size != 16) return false;
      idx.base = src;
      return true;
   }
   struct BPointView
   {
      const BPointIndex* idx;
      double x() const { double v; std::memcpy(&v, idx->base + 0, 8); return v; }
      double y() const { double v; std::memcpy(&v, idx->base + 8, 8); return v; }
   };

   // Token
   struct TokenIndex { const char* base = nullptr; std::uint32_t text_pos; };
   inline TokenIndex build(const Token*, const char* src, std::size_t)
   {
      TokenIndex idx{src};
      idx.text_pos = 2 + 4 + 4;
      return idx;
   }
   inline bool build_checked(const Token*, const char* src, std::size_t size, TokenIndex& idx)
   {
      idx.base = src;
      constexpr std::uint32_t header = 2 + 4 + 4;
      if (size < header) return false;
      std::uint32_t pos = header;
      idx.text_pos = pos;
      std::uint32_t len;
      if (!safe_read_varuint32(src, pos, size, len)) return false;
      if (!room_for(pos, len, size)) return false;
      if (pos + len != size) return false;  // no trailing garbage
      return true;
   }
   struct TokenView
   {
      const TokenIndex* idx;
      std::uint16_t kind()   const { std::uint16_t v; std::memcpy(&v, idx->base + 0, 2); return v; }
      std::uint32_t offset() const { std::uint32_t v; std::memcpy(&v, idx->base + 2, 4); return v; }
      std::uint32_t length() const { std::uint32_t v; std::memcpy(&v, idx->base + 6, 4); return v; }
      std::string_view text() const
      {
         std::uint32_t p = idx->text_pos;
         std::uint32_t len = read_varuint32(idx->base, p);
         return {idx->base + p, len};
      }
   };

   // LineItem
   struct LineItemIndex { const char* base = nullptr; std::uint32_t qty_pos; };
   inline LineItemIndex build(const LineItem*, const char* src, std::size_t)
   {
      LineItemIndex idx{src};
      std::uint32_t pos = 0;
      std::uint32_t len = read_varuint32(src, pos);
      pos += len;
      idx.qty_pos = pos;
      return idx;
   }
   inline bool build_checked(const LineItem*, const char* src, std::size_t size,
                              LineItemIndex& idx)
   {
      idx.base = src;
      std::uint32_t pos = 0;
      std::uint32_t len;
      if (!safe_read_varuint32(src, pos, size, len)) return false;
      if (!room_for(pos, len, size)) return false;
      pos += len;
      idx.qty_pos = pos;
      if (!room_for(pos, 4 + 8, size)) return false;
      if (pos + 4 + 8 != size) return false;
      return true;
   }
   struct LineItemView
   {
      const LineItemIndex* idx;
      std::string_view product() const
      {
         std::uint32_t p = 0;
         std::uint32_t len = read_varuint32(idx->base, p);
         return {idx->base + p, len};
      }
      std::uint32_t qty() const { std::uint32_t v; std::memcpy(&v, idx->base + idx->qty_pos, 4); return v; }
      double unit_price() const { double v; std::memcpy(&v, idx->base + idx->qty_pos + 4, 8); return v; }
   };

   // UserProfile
   struct UserProfileIndex
   {
      const char*                base = nullptr;
      std::uint32_t              name_pos, email_pos, bio_pos, age_pos, score_pos;
      std::uint32_t              tags_count;
      std::uint32_t              verified_pos;
      std::vector<std::uint32_t> tag_offsets;
   };
   inline std::uint32_t build_user_profile_at(const char* src, std::uint32_t pos,
                                               UserProfileIndex& idx)
   {
      idx.base = src;
      pos += 8;
      idx.name_pos = pos;
      { std::uint32_t len = read_varuint32(src, pos); pos += len; }
      idx.email_pos = pos;
      { std::uint32_t len = read_varuint32(src, pos); pos += len; }
      idx.bio_pos = pos;
      std::uint8_t has_bio = static_cast<std::uint8_t>(src[pos++]);
      if (has_bio) { std::uint32_t len = read_varuint32(src, pos); pos += len; }
      idx.age_pos = pos;   pos += 4;
      idx.score_pos = pos; pos += 8;
      std::uint32_t tc = read_varuint32(src, pos);
      idx.tags_count = tc;
      idx.tag_offsets.reserve(tc);
      for (std::uint32_t i = 0; i < tc; ++i)
      {
         idx.tag_offsets.push_back(pos);
         std::uint32_t len = read_varuint32(src, pos);
         pos += len;
      }
      idx.verified_pos = pos;
      pos += 1;
      return pos;
   }
   inline UserProfileIndex build(const UserProfile*, const char* src, std::size_t size)
   {
      UserProfileIndex idx;
      auto p = build_user_profile_at(src, 0, idx);
      assert(p == size); (void)p; (void)size;
      return idx;
   }
   // Bounds-checked variant for UserProfile. Used standalone and nested (by Order).
   inline bool build_user_profile_checked_at(const char* src, std::uint32_t& pos,
                                              std::size_t size, UserProfileIndex& idx)
   {
      idx.base = src;
      if (!room_for(pos, 8, size)) return false;
      pos += 8;  // id
      idx.name_pos = pos;
      std::uint32_t len;
      if (!safe_read_varuint32(src, pos, size, len) || !room_for(pos, len, size)) return false;
      pos += len;
      idx.email_pos = pos;
      if (!safe_read_varuint32(src, pos, size, len) || !room_for(pos, len, size)) return false;
      pos += len;
      idx.bio_pos = pos;
      if (!room_for(pos, 1, size)) return false;
      std::uint8_t has_bio = static_cast<std::uint8_t>(src[pos++]);
      if (has_bio > 1) return false;
      if (has_bio)
      {
         if (!safe_read_varuint32(src, pos, size, len) || !room_for(pos, len, size)) return false;
         pos += len;
      }
      idx.age_pos = pos;
      if (!room_for(pos, 4, size)) return false;
      pos += 4;
      idx.score_pos = pos;
      if (!room_for(pos, 8, size)) return false;
      pos += 8;
      std::uint32_t tc;
      if (!safe_read_varuint32(src, pos, size, tc)) return false;
      idx.tags_count = tc;
      idx.tag_offsets.reserve(tc);
      for (std::uint32_t i = 0; i < tc; ++i)
      {
         idx.tag_offsets.push_back(pos);
         if (!safe_read_varuint32(src, pos, size, len) || !room_for(pos, len, size)) return false;
         pos += len;
      }
      idx.verified_pos = pos;
      if (!room_for(pos, 1, size)) return false;
      std::uint8_t v = static_cast<std::uint8_t>(src[pos++]);
      if (v > 1) return false;
      return true;
   }
   inline bool build_checked(const UserProfile*, const char* src, std::size_t size,
                              UserProfileIndex& idx)
   {
      std::uint32_t pos = 0;
      if (!build_user_profile_checked_at(src, pos, size, idx)) return false;
      return pos == size;
   }
   struct UserProfileView
   {
      const UserProfileIndex* idx;
      std::uint64_t id() const { std::uint64_t v; std::memcpy(&v, idx->base + 0, 8); return v; }
      std::string_view read_str(std::uint32_t p) const
      {
         std::uint32_t q = p; std::uint32_t len = read_varuint32(idx->base, q);
         return {idx->base + q, len};
      }
      std::string_view name()  const { return read_str(idx->name_pos); }
      std::string_view email() const { return read_str(idx->email_pos); }
      std::optional<std::string_view> bio() const
      {
         if (!idx->base[idx->bio_pos]) return std::nullopt;
         return read_str(idx->bio_pos + 1);
      }
      std::uint32_t age()   const { std::uint32_t v; std::memcpy(&v, idx->base + idx->age_pos, 4); return v; }
      double        score() const { double        v; std::memcpy(&v, idx->base + idx->score_pos, 8); return v; }
      std::size_t      tags_size() const { return idx->tags_count; }
      std::string_view tag(std::size_t i) const { return read_str(idx->tag_offsets[i]); }
      bool verified() const { return idx->base[idx->verified_pos] != 0; }
   };

   // Order — nested UserProfile + vector<LineItem>
   struct LineItemInOrder
   {
      const char*   base;
      std::uint32_t start;        // product length prefix
      std::uint32_t qty_pos_abs;
      std::string_view product() const
      {
         std::uint32_t p = start; std::uint32_t len = read_varuint32(base, p);
         return {base + p, len};
      }
      std::uint32_t qty()        const { std::uint32_t v; std::memcpy(&v, base + qty_pos_abs, 4);   return v; }
      double        unit_price() const { double        v; std::memcpy(&v, base + qty_pos_abs + 4, 8); return v; }
   };
   struct OrderIndex
   {
      const char*                  base = nullptr;
      UserProfileIndex             customer;
      std::uint32_t                items_count;
      std::vector<LineItemInOrder> items;
      std::uint32_t                total_pos;
      std::uint32_t                note_pos;
   };
   inline OrderIndex build(const Order*, const char* src, std::size_t size)
   {
      OrderIndex    idx;
      idx.base = src;
      std::uint32_t pos = 0;
      pos += 8;
      pos = build_user_profile_at(src, pos, idx.customer);
      std::uint32_t ic = read_varuint32(src, pos);
      idx.items_count = ic;
      idx.items.resize(ic);
      for (std::uint32_t i = 0; i < ic; ++i)
      {
         idx.items[i].base = src;
         idx.items[i].start = pos;
         std::uint32_t len = read_varuint32(src, pos);
         pos += len;
         idx.items[i].qty_pos_abs = pos;
         pos += 4 + 8;
      }
      idx.total_pos = pos; pos += 8;
      idx.note_pos = pos;
      std::uint8_t has_note = static_cast<std::uint8_t>(src[pos++]);
      if (has_note) { std::uint32_t len = read_varuint32(src, pos); pos += len; }
      assert(pos == size); (void)size;
      return idx;
   }
   inline bool build_checked(const Order*, const char* src, std::size_t size, OrderIndex& idx)
   {
      idx.base = src;
      std::uint32_t pos = 0;
      if (!room_for(pos, 8, size)) return false;
      pos += 8;  // id
      if (!build_user_profile_checked_at(src, pos, size, idx.customer)) return false;
      std::uint32_t ic;
      if (!safe_read_varuint32(src, pos, size, ic)) return false;
      idx.items_count = ic;
      idx.items.resize(ic);
      for (std::uint32_t i = 0; i < ic; ++i)
      {
         idx.items[i].base  = src;
         idx.items[i].start = pos;
         std::uint32_t len;
         if (!safe_read_varuint32(src, pos, size, len) || !room_for(pos, len, size)) return false;
         pos += len;
         idx.items[i].qty_pos_abs = pos;
         if (!room_for(pos, 4 + 8, size)) return false;
         pos += 4 + 8;
      }
      idx.total_pos = pos;
      if (!room_for(pos, 8, size)) return false;
      pos += 8;
      idx.note_pos = pos;
      if (!room_for(pos, 1, size)) return false;
      std::uint8_t has_note = static_cast<std::uint8_t>(src[pos++]);
      if (has_note > 1) return false;
      if (has_note)
      {
         std::uint32_t len;
         if (!safe_read_varuint32(src, pos, size, len) || !room_for(pos, len, size)) return false;
         pos += len;
      }
      return pos == size;
   }
   struct OrderView
   {
      const OrderIndex* idx;
      std::uint64_t   id() const { std::uint64_t v; std::memcpy(&v, idx->base + 0, 8); return v; }
      UserProfileView customer() const { return {&idx->customer}; }
      std::size_t     items_size() const { return idx->items_count; }
      LineItemInOrder item(std::size_t i) const { return idx->items[i]; }
      double          total() const { double v; std::memcpy(&v, idx->base + idx->total_pos, 8); return v; }
      std::optional<std::string_view> note() const
      {
         if (!idx->base[idx->note_pos]) return std::nullopt;
         std::uint32_t p = idx->note_pos + 1;
         std::uint32_t len = read_varuint32(idx->base, p);
         return std::string_view{idx->base + p, len};
      }
   };

   // SensorReading
   struct SensorReadingIndex
   {
      const char*   base = nullptr;
      std::uint32_t device_id_pos, after_device_id_pos;
      std::uint32_t error_code_pos, firmware_pos;
   };
   inline SensorReadingIndex build(const SensorReading*, const char* src, std::size_t size)
   {
      SensorReadingIndex idx;
      idx.base = src;
      std::uint32_t pos = 8;
      idx.device_id_pos = pos;
      { std::uint32_t len = read_varuint32(src, pos); pos += len; }
      idx.after_device_id_pos = pos;
      pos += 12 * 8 + 4 + 2;
      idx.error_code_pos = pos;
      std::uint8_t has_err = static_cast<std::uint8_t>(src[pos++]);
      if (has_err) pos += 4;
      idx.firmware_pos = pos;
      { std::uint32_t len = read_varuint32(src, pos); pos += len; }
      assert(pos == size); (void)size;
      return idx;
   }
   inline bool build_checked(const SensorReading*, const char* src, std::size_t size,
                              SensorReadingIndex& idx)
   {
      idx.base = src;
      if (!room_for(0, 8, size)) return false;
      std::uint32_t pos = 8;  // timestamp
      idx.device_id_pos = pos;
      std::uint32_t len;
      if (!safe_read_varuint32(src, pos, size, len) || !room_for(pos, len, size)) return false;
      pos += len;
      idx.after_device_id_pos = pos;
      constexpr std::uint32_t fixed_block = 12 * 8 + 4 + 2;
      if (!room_for(pos, fixed_block, size)) return false;
      pos += fixed_block;
      idx.error_code_pos = pos;
      if (!room_for(pos, 1, size)) return false;
      std::uint8_t has_err = static_cast<std::uint8_t>(src[pos++]);
      if (has_err > 1) return false;
      if (has_err)
      {
         if (!room_for(pos, 4, size)) return false;
         pos += 4;
      }
      idx.firmware_pos = pos;
      if (!safe_read_varuint32(src, pos, size, len) || !room_for(pos, len, size)) return false;
      pos += len;
      return pos == size;
   }
   struct SensorReadingView
   {
      const SensorReadingIndex* idx;
      std::uint64_t timestamp() const { std::uint64_t v; std::memcpy(&v, idx->base + 0, 8); return v; }
      std::string_view device_id() const
      {
         std::uint32_t p = idx->device_id_pos;
         std::uint32_t len = read_varuint32(idx->base, p);
         return {idx->base + p, len};
      }
      double read_d(std::uint32_t off) const
      {
         double v; std::memcpy(&v, idx->base + idx->after_device_id_pos + off, 8); return v;
      }
      double temp() const    { return read_d(0);  }
      double humidity() const{ return read_d(8);  }
      double pressure() const{ return read_d(16); }
      double accel_x() const { return read_d(24); }
      double accel_y() const { return read_d(32); }
      double accel_z() const { return read_d(40); }
      double gyro_x() const  { return read_d(48); }
      double gyro_y() const  { return read_d(56); }
      double gyro_z() const  { return read_d(64); }
      double mag_x() const   { return read_d(72); }
      double mag_y() const   { return read_d(80); }
      double mag_z() const   { return read_d(88); }
      float battery() const { float v; std::memcpy(&v, idx->base + idx->after_device_id_pos + 96, 4); return v; }
      std::int16_t signal_dbm() const { std::int16_t v; std::memcpy(&v, idx->base + idx->after_device_id_pos + 100, 2); return v; }
      std::optional<std::uint32_t> error_code() const
      {
         if (!idx->base[idx->error_code_pos]) return std::nullopt;
         std::uint32_t v; std::memcpy(&v, idx->base + idx->error_code_pos + 1, 4); return v;
      }
      std::string_view firmware() const
      {
         std::uint32_t p = idx->firmware_pos;
         std::uint32_t len = read_varuint32(idx->base, p);
         return {idx->base + p, len};
      }
   };
}  // namespace sidx

// ══════════════════════════════════════════════════════════════════════════
// Bench harness
// ══════════════════════════════════════════════════════════════════════════

template <typename Fn>
static double bench_ns(Fn fn)
{
   using clock = std::chrono::high_resolution_clock;
   for (int i = 0; i < 200; ++i) { fn(); clobber_memory(); }
   std::size_t cal_iters = 0;
   auto cal_start = clock::now();
   while (std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - cal_start).count() < 30'000)
   { fn(); clobber_memory(); ++cal_iters; }
   auto cal_us = std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - cal_start).count();
   double ns_per_op = cal_iters > 0 ? (cal_us * 1000.0 / cal_iters) : 1.0;
   std::size_t batch = 1;
   if (ns_per_op < 100.0)
      batch = std::max<std::size_t>(1, static_cast<std::size_t>(100.0 / std::max(ns_per_op, 0.01)));
   std::size_t target = std::max<std::size_t>(1000,
       static_cast<std::size_t>(200'000'000.0 / (ns_per_op * static_cast<double>(batch))));
   auto start = clock::now();
   for (std::size_t i = 0; i < target; ++i)
      for (std::size_t b = 0; b < batch; ++b) { fn(); clobber_memory(); }
   auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count();
   return static_cast<double>(ns) / (static_cast<double>(target) * static_cast<double>(batch));
}

// ══════════════════════════════════════════════════════════════════════════
// Per-type workloads.  For each T we measure:
//   decode_validate_frac — call validate_frac(frac_bytes)
//   decode_build_shadow  — build shadow index from pack_bin bytes
//   decode_from_bin      — full unpack (from_bin) into native struct
//   read_frac_view       — iterate all fields via frac_view  (after validate)
//   read_shadow_view     — iterate all fields via shadow view (after build)
//   read_native          — iterate fields of pre-decoded native struct
// ══════════════════════════════════════════════════════════════════════════

// BPoint — trivial (no variable fields, all fixed)
static double dec_validate(const std::vector<char>& b, BPoint)
{ return bench_ns([&] { auto r = psio::validate_frac<BPoint>({b.data(), b.size()}); do_not_optimize(r); }); }
static double dec_build   (const std::vector<char>& b, BPoint)
{ return bench_ns([&] { auto i = sidx::build(static_cast<const BPoint*>(nullptr), b.data(), b.size()); do_not_optimize(i.base); }); }
static double dec_build_checked(const std::vector<char>& b, BPoint)
{ return bench_ns([&] { sidx::BPointIndex i; bool ok = sidx::build_checked(static_cast<const BPoint*>(nullptr), b.data(), b.size(), i); do_not_optimize(ok); do_not_optimize(i.base); }); }
static double dec_unpack  (const std::vector<char>& b, BPoint)
{ return bench_ns([&] { BPoint u; psio::input_stream s{b.data(), b.size()}; psio::from_bin(u, s); do_not_optimize(u.x); }); }
static double read_frac   (const std::vector<char>& b, BPoint)
{ return bench_ns([&] { auto v = psio::frac_view<BPoint>::from_buffer(b.data()); do_not_optimize(v.x()); do_not_optimize(v.y()); }); }
static double read_shadow (const sidx::BPointIndex& idx)
{ return bench_ns([&] { sidx::BPointView v{&idx}; do_not_optimize(v.x()); do_not_optimize(v.y()); }); }
static double read_native (const BPoint& u)
{ return bench_ns([&] { do_not_optimize(u.x); do_not_optimize(u.y); }); }

// Token
static double dec_validate(const std::vector<char>& b, Token)
{ return bench_ns([&] { auto r = psio::validate_frac<Token>({b.data(), b.size()}); do_not_optimize(r); }); }
static double dec_build   (const std::vector<char>& b, Token)
{ return bench_ns([&] { auto i = sidx::build(static_cast<const Token*>(nullptr), b.data(), b.size()); do_not_optimize(i.text_pos); }); }
static double dec_build_checked(const std::vector<char>& b, Token)
{ return bench_ns([&] { sidx::TokenIndex i; bool ok = sidx::build_checked(static_cast<const Token*>(nullptr), b.data(), b.size(), i); do_not_optimize(ok); do_not_optimize(i.text_pos); }); }
static double dec_unpack  (const std::vector<char>& b, Token)
{ return bench_ns([&] { Token u; psio::input_stream s{b.data(), b.size()}; psio::from_bin(u, s); do_not_optimize(u.kind); }); }
static double read_frac   (const std::vector<char>& b, Token)
{ return bench_ns([&] { auto v = psio::frac_view<Token>::from_buffer(b.data());
   do_not_optimize(v.kind()); do_not_optimize(v.offset()); do_not_optimize(v.length());
   auto t = v.text(); do_not_optimize(t); }); }
static double read_shadow (const sidx::TokenIndex& idx)
{ return bench_ns([&] { sidx::TokenView v{&idx};
   do_not_optimize(v.kind()); do_not_optimize(v.offset()); do_not_optimize(v.length());
   auto t = v.text(); do_not_optimize(t); }); }
static double read_native (const Token& u)
{ return bench_ns([&] { do_not_optimize(u.kind); do_not_optimize(u.offset); do_not_optimize(u.length); do_not_optimize(u.text); }); }

// LineItem
static double dec_validate(const std::vector<char>& b, LineItem)
{ return bench_ns([&] { auto r = psio::validate_frac<LineItem>({b.data(), b.size()}); do_not_optimize(r); }); }
static double dec_build   (const std::vector<char>& b, LineItem)
{ return bench_ns([&] { auto i = sidx::build(static_cast<const LineItem*>(nullptr), b.data(), b.size()); do_not_optimize(i.qty_pos); }); }
static double dec_build_checked(const std::vector<char>& b, LineItem)
{ return bench_ns([&] { sidx::LineItemIndex i; bool ok = sidx::build_checked(static_cast<const LineItem*>(nullptr), b.data(), b.size(), i); do_not_optimize(ok); do_not_optimize(i.qty_pos); }); }
static double dec_unpack  (const std::vector<char>& b, LineItem)
{ return bench_ns([&] { LineItem u; psio::input_stream s{b.data(), b.size()}; psio::from_bin(u, s); do_not_optimize(u.qty); }); }
static double read_frac   (const std::vector<char>& b, LineItem)
{ return bench_ns([&] { auto v = psio::frac_view<LineItem>::from_buffer(b.data());
   auto p = v.product(); do_not_optimize(p); do_not_optimize(v.qty()); do_not_optimize(v.unit_price()); }); }
static double read_shadow (const sidx::LineItemIndex& idx)
{ return bench_ns([&] { sidx::LineItemView v{&idx};
   auto p = v.product(); do_not_optimize(p); do_not_optimize(v.qty()); do_not_optimize(v.unit_price()); }); }
static double read_native (const LineItem& u)
{ return bench_ns([&] { do_not_optimize(u.product); do_not_optimize(u.qty); do_not_optimize(u.unit_price); }); }

// UserProfile
static double dec_validate(const std::vector<char>& b, UserProfile)
{ return bench_ns([&] { auto r = psio::validate_frac<UserProfile>({b.data(), b.size()}); do_not_optimize(r); }); }
static double dec_build   (const std::vector<char>& b, UserProfile)
{ return bench_ns([&] { auto i = sidx::build(static_cast<const UserProfile*>(nullptr), b.data(), b.size()); do_not_optimize(i.age_pos); }); }
static double dec_build_checked(const std::vector<char>& b, UserProfile)
{ return bench_ns([&] { sidx::UserProfileIndex i; bool ok = sidx::build_checked(static_cast<const UserProfile*>(nullptr), b.data(), b.size(), i); do_not_optimize(ok); do_not_optimize(i.age_pos); }); }
static double dec_unpack  (const std::vector<char>& b, UserProfile)
{ return bench_ns([&] { UserProfile u; psio::input_stream s{b.data(), b.size()}; psio::from_bin(u, s); do_not_optimize(u.id); }); }
static double read_frac   (const std::vector<char>& b, UserProfile)
{ return bench_ns([&] { auto v = psio::frac_view<UserProfile>::from_buffer(b.data());
   do_not_optimize(v.id()); auto n=v.name(); do_not_optimize(n); auto e=v.email(); do_not_optimize(e);
   auto bi=v.bio(); do_not_optimize(bi); do_not_optimize(v.age()); do_not_optimize(v.score());
   auto tv=v.tags(); for(std::uint32_t i=0;i<tv.size();++i){auto t=tv[i]; do_not_optimize(t);}
   do_not_optimize(v.verified()); }); }
static double read_shadow (const sidx::UserProfileIndex& idx)
{ return bench_ns([&] { sidx::UserProfileView v{&idx};
   do_not_optimize(v.id()); auto n=v.name(); do_not_optimize(n); auto e=v.email(); do_not_optimize(e);
   auto bi=v.bio(); do_not_optimize(bi); do_not_optimize(v.age()); do_not_optimize(v.score());
   for(std::size_t i=0;i<v.tags_size();++i){auto t=v.tag(i); do_not_optimize(t);}
   do_not_optimize(v.verified()); }); }
static double read_native (const UserProfile& u)
{ return bench_ns([&] { do_not_optimize(u.id); do_not_optimize(u.name); do_not_optimize(u.email);
   do_not_optimize(u.bio); do_not_optimize(u.age); do_not_optimize(u.score);
   for(auto const& t:u.tags) do_not_optimize(t); do_not_optimize(u.verified); }); }

// SensorReading
static double dec_validate(const std::vector<char>& b, SensorReading)
{ return bench_ns([&] { auto r = psio::validate_frac<SensorReading>({b.data(), b.size()}); do_not_optimize(r); }); }
static double dec_build   (const std::vector<char>& b, SensorReading)
{ return bench_ns([&] { auto i = sidx::build(static_cast<const SensorReading*>(nullptr), b.data(), b.size()); do_not_optimize(i.firmware_pos); }); }
static double dec_build_checked(const std::vector<char>& b, SensorReading)
{ return bench_ns([&] { sidx::SensorReadingIndex i; bool ok = sidx::build_checked(static_cast<const SensorReading*>(nullptr), b.data(), b.size(), i); do_not_optimize(ok); do_not_optimize(i.firmware_pos); }); }
static double dec_unpack  (const std::vector<char>& b, SensorReading)
{ return bench_ns([&] { SensorReading u; psio::input_stream s{b.data(), b.size()}; psio::from_bin(u, s); do_not_optimize(u.timestamp); }); }
static double read_frac   (const std::vector<char>& b, SensorReading)
{ return bench_ns([&] { auto v = psio::frac_view<SensorReading>::from_buffer(b.data());
   do_not_optimize(v.timestamp()); auto d=v.device_id(); do_not_optimize(d);
   do_not_optimize(v.temp());    do_not_optimize(v.humidity()); do_not_optimize(v.pressure());
   do_not_optimize(v.accel_x()); do_not_optimize(v.accel_y());  do_not_optimize(v.accel_z());
   do_not_optimize(v.gyro_x());  do_not_optimize(v.gyro_y());   do_not_optimize(v.gyro_z());
   do_not_optimize(v.mag_x());   do_not_optimize(v.mag_y());    do_not_optimize(v.mag_z());
   do_not_optimize(v.battery()); do_not_optimize(v.signal_dbm()); do_not_optimize(v.error_code());
   auto fw=v.firmware(); do_not_optimize(fw); }); }
static double read_shadow (const sidx::SensorReadingIndex& idx)
{ return bench_ns([&] { sidx::SensorReadingView v{&idx};
   do_not_optimize(v.timestamp()); auto d=v.device_id(); do_not_optimize(d);
   do_not_optimize(v.temp());    do_not_optimize(v.humidity()); do_not_optimize(v.pressure());
   do_not_optimize(v.accel_x()); do_not_optimize(v.accel_y());  do_not_optimize(v.accel_z());
   do_not_optimize(v.gyro_x());  do_not_optimize(v.gyro_y());   do_not_optimize(v.gyro_z());
   do_not_optimize(v.mag_x());   do_not_optimize(v.mag_y());    do_not_optimize(v.mag_z());
   do_not_optimize(v.battery()); do_not_optimize(v.signal_dbm()); do_not_optimize(v.error_code());
   auto fw=v.firmware(); do_not_optimize(fw); }); }
static double read_native (const SensorReading& u)
{ return bench_ns([&] {
   do_not_optimize(u.timestamp); do_not_optimize(u.device_id);
   do_not_optimize(u.temp);    do_not_optimize(u.humidity); do_not_optimize(u.pressure);
   do_not_optimize(u.accel_x); do_not_optimize(u.accel_y);  do_not_optimize(u.accel_z);
   do_not_optimize(u.gyro_x);  do_not_optimize(u.gyro_y);   do_not_optimize(u.gyro_z);
   do_not_optimize(u.mag_x);   do_not_optimize(u.mag_y);    do_not_optimize(u.mag_z);
   do_not_optimize(u.battery); do_not_optimize(u.signal_dbm); do_not_optimize(u.error_code);
   do_not_optimize(u.firmware); }); }

// Order
static double dec_validate(const std::vector<char>& b, Order)
{ return bench_ns([&] { auto r = psio::validate_frac<Order>({b.data(), b.size()}); do_not_optimize(r); }); }
static double dec_build   (const std::vector<char>& b, Order)
{ return bench_ns([&] { auto i = sidx::build(static_cast<const Order*>(nullptr), b.data(), b.size()); do_not_optimize(i.total_pos); }); }
static double dec_build_checked(const std::vector<char>& b, Order)
{ return bench_ns([&] { sidx::OrderIndex i; bool ok = sidx::build_checked(static_cast<const Order*>(nullptr), b.data(), b.size(), i); do_not_optimize(ok); do_not_optimize(i.total_pos); }); }
static double dec_unpack  (const std::vector<char>& b, Order)
{ return bench_ns([&] { Order u; psio::input_stream s{b.data(), b.size()}; psio::from_bin(u, s); do_not_optimize(u.id); }); }
static double read_frac   (const std::vector<char>& b, Order)
{ return bench_ns([&] { auto v = psio::frac_view<Order>::from_buffer(b.data());
   do_not_optimize(v.id()); auto cu = v.customer(); do_not_optimize(cu.id());
   auto cn=cu.name(); do_not_optimize(cn); auto ce=cu.email(); do_not_optimize(ce);
   auto cb=cu.bio(); do_not_optimize(cb); do_not_optimize(cu.age()); do_not_optimize(cu.score());
   auto ct=cu.tags(); for(std::uint32_t i=0;i<ct.size();++i){auto t=ct[i]; do_not_optimize(t);}
   do_not_optimize(cu.verified());
   auto iv=v.items(); for(std::uint32_t i=0;i<iv.size();++i){
      auto it=iv[i]; auto p=it.product(); do_not_optimize(p);
      do_not_optimize(it.qty()); do_not_optimize(it.unit_price());}
   do_not_optimize(v.total()); auto nt=v.note(); do_not_optimize(nt); }); }
static double read_shadow (const sidx::OrderIndex& idx)
{ return bench_ns([&] { sidx::OrderView v{&idx};
   do_not_optimize(v.id()); auto cu = v.customer(); do_not_optimize(cu.id());
   auto cn=cu.name(); do_not_optimize(cn); auto ce=cu.email(); do_not_optimize(ce);
   auto cb=cu.bio(); do_not_optimize(cb); do_not_optimize(cu.age()); do_not_optimize(cu.score());
   for(std::size_t i=0;i<cu.tags_size();++i){auto t=cu.tag(i); do_not_optimize(t);}
   do_not_optimize(cu.verified());
   for(std::size_t i=0;i<v.items_size();++i){
      auto it=v.item(i); auto p=it.product(); do_not_optimize(p);
      do_not_optimize(it.qty()); do_not_optimize(it.unit_price());}
   do_not_optimize(v.total()); auto nt=v.note(); do_not_optimize(nt); }); }
static double read_native (const Order& u)
{ return bench_ns([&] {
   do_not_optimize(u.id); do_not_optimize(u.customer.id);
   do_not_optimize(u.customer.name); do_not_optimize(u.customer.email);
   do_not_optimize(u.customer.bio); do_not_optimize(u.customer.age); do_not_optimize(u.customer.score);
   for(auto const& t:u.customer.tags) do_not_optimize(t);
   do_not_optimize(u.customer.verified);
   for(auto const& it:u.items) { do_not_optimize(it.product); do_not_optimize(it.qty); do_not_optimize(it.unit_price); }
   do_not_optimize(u.total); do_not_optimize(u.note); }); }

// ══════════════════════════════════════════════════════════════════════════
// Runner
// ══════════════════════════════════════════════════════════════════════════

// ── Extensible mirrors of the types above, used for pack bench only ─────
// Same field layout, no definitionWillNotChange() — exercise the cache.

struct ETok { std::uint16_t kind; std::uint32_t offset; std::uint32_t length; std::string text; };
PSIO_REFLECT(ETok, kind, offset, length, text)

struct EUserProfile
{
   std::uint64_t              id;
   std::string                name;
   std::string                email;
   std::optional<std::string> bio;
   std::uint32_t              age;
   double                     score;
   std::vector<std::string>   tags;
   bool                       verified;
};
PSIO_REFLECT(EUserProfile, id, name, email, bio, age, score, tags, verified)

struct ELineItem { std::string product; std::uint32_t qty; double unit_price; };
PSIO_REFLECT(ELineItem, product, qty, unit_price)

struct EOrder
{
   std::uint64_t              id;
   EUserProfile               customer;
   std::vector<ELineItem>     items;
   double                     total;
   std::optional<std::string> note;
};
PSIO_REFLECT(EOrder, id, customer, items, total, note)

struct ESensor
{
   std::uint64_t                timestamp;
   std::string                  device_id;
   double                       temp, humidity, pressure;
   double                       accel_x, accel_y, accel_z;
   double                       gyro_x, gyro_y, gyro_z;
   double                       mag_x, mag_y, mag_z;
   float                        battery;
   std::int16_t                 signal_dbm;
   std::optional<std::uint32_t> error_code;
   std::string                  firmware;
};
PSIO_REFLECT(ESensor, timestamp, device_id, temp, humidity, pressure,
             accel_x, accel_y, accel_z, gyro_x, gyro_y, gyro_z,
             mag_x, mag_y, mag_z, battery, signal_dbm, error_code, firmware)

static ETok make_etok() { return {42, 1024, 15, "identifier_name"}; }
static ELineItem make_elineitem(int i)
{
   return {"Product-" + std::to_string(i), static_cast<std::uint32_t>(i + 1), 19.99 + i * 5.0};
}
static EUserProfile make_euser()
{
   return {123456789ULL, "Alice Johnson", "alice@example.com",
           std::string("Software engineer interested in distributed systems and WebAssembly"),
           32, 98.5, {"developer", "wasm", "c++", "open-source"}, true};
}
static EOrder make_eorder()
{
   std::vector<ELineItem> items;
   for (int i = 0; i < 5; ++i) items.push_back(make_elineitem(i));
   return {987654321ULL, make_euser(), std::move(items), 199.95,
           std::string("Please ship before Friday")};
}
static ESensor make_esensor()
{
   return {1700000000000ULL, "sensor-alpha-42", 23.5, 65.2, 1013.25, 0.01, -0.02, 9.81,
           0.001, -0.003, 0.002, 25.1, -12.3, 42.7, 3.7f, -65, std::nullopt, "v2.3.1-rc4"};
}

// ── Pack benchmarks ─────────────────────────────────────────────────────

template <typename T>
static std::vector<char> slow_convert_to_bin(const T& obj)
{
   // Slow path: size_stream pre-pass + write pass. Each extensible nested
   // struct internally does its own size_stream pass to compute its varuint
   // prefix, so complex trees pay O(depth × size) in the size phase.
   psio::size_stream ss;
   psio::to_bin(obj, ss);
   std::vector<char> out(ss.size);
   psio::fixed_buf_stream fbs(out.data(), ss.size);
   psio::to_bin(obj, fbs);
   return out;
}

template <typename T>
static void run_pack_row(const char* label, const T& orig)
{
   auto bin  = psio::convert_to_bin(orig);
   auto frac = psio::to_frac(orig);
   (void)bin;
   (void)frac;

   double cached = bench_ns([&]
                            {
                               auto v = psio::convert_to_bin(orig);
                               do_not_optimize(v.data());
                            });
   double slow   = bench_ns([&]
                          {
                             auto v = slow_convert_to_bin(orig);
                             do_not_optimize(v.data());
                          });
   double frac_p = bench_ns([&]
                            {
                               auto v = psio::to_frac(orig);
                               do_not_optimize(v.data());
                            });

   std::printf("  %-14s | %6.1f  %6.1f  %6.1f   | %5.2fx vs slow |\n",
               label, slow, cached, frac_p, slow / cached);
}

template <typename T>
static void run_row(const char* label, const T& orig)
{
   auto bin  = psio::convert_to_bin(orig);
   auto frac = psio::to_frac(orig);

   // Build decoded artifacts once; the read benchmarks iterate reads only.
   auto idx = sidx::build(static_cast<const T*>(nullptr), bin.data(), bin.size());
   T    native_copy;
   {
      psio::input_stream s{bin.data(), bin.size()};
      psio::from_bin(native_copy, s);
   }

   double dv  = dec_validate(frac, T{});
   double db  = dec_build(bin, T{});
   double dbc = dec_build_checked(bin, T{});
   double du  = dec_unpack(bin, T{});

   double rf = read_frac(frac, T{});
   double rs = read_shadow(idx);
   double rn = read_native(native_copy);

   std::printf("  %-14s | %5zu B | %5zu B | %6.1f  %6.1f  %6.1f  %6.1f | %6.1f  %6.1f  %6.1f |\n",
               label, bin.size(), frac.size(),
               dv, db, dbc, du,
               rf, rs, rn);
}

int main()
{
   std::printf("\n=== Decode/validate phase vs. read-only phase ===\n\n");
   std::printf("  (all times in ns/op; Release -O2)\n\n");
   std::printf("  %-14s | %-7s | %-7s | %-34s | %-25s |\n",
               "Type", "bin", "frac",
               "DECODE (ns)",
               "READ-ONLY (ns)");
   std::printf("  %-14s | %-7s | %-7s | %6s  %6s  %6s  %6s | %6s  %6s  %6s |\n",
               "", "", "",
               "frac",     "shadow",  "shadow",   "unpack",
               "frac",     "shadow",  "native");
   std::printf("  %-14s | %-7s | %-7s | %6s  %6s  %6s  %6s | %6s  %6s  %6s |\n",
               "", "", "",
               "validate", "build",   "build+",   "from_bin",
               "view",     "view",    "struct");
   std::printf("  %-14s | %-7s | %-7s | %6s  %6s  %6s  %6s | %6s  %6s  %6s |\n",
               "", "", "",
               "",         "",         "check",    "",
               "",         "",         "");
   std::printf("  %-14s | %-7s | %-7s | %-34s | %-25s |\n",
               "--------------", "-------", "-------",
               "----------------------------------",
               "-------------------------");

   run_row("BPoint",        make_point());
   run_row("Token",         make_token());
   run_row("LineItem",      make_line_item(0));
   run_row("UserProfile",   make_user());
   run_row("SensorReading", make_sensor());
   run_row("Order",         make_order());

   std::printf("\n=== PACK: slow (size_stream) vs cached vs fracpack ===\n\n");
   std::printf("  (convert_to_bin uses cache; slow_convert_to_bin shows old behavior)\n\n");
   std::printf("  %-14s | %-7s  %-7s  %-7s   | %-14s |\n",
               "Type", "slow",   "cached", "frac",   "speedup");
   std::printf("  %-14s | %-7s  %-7s  %-7s   | %-14s |\n",
               "--------------", "-------", "-------", "-------", "--------------");
   // Extensible (non-DWNC) — exercises the size cache.
   run_pack_row("BPoint",        make_point());          // DWNC, cache inert
   run_pack_row("ETok",          make_etok());           // 1 ext level
   run_pack_row("ELineItem",     make_elineitem(0));     // 1 ext level
   run_pack_row("EUserProfile",  make_euser());          // 1 ext level, vec<string>
   run_pack_row("ESensor",       make_esensor());        // 1 ext level, fixed-heavy
   run_pack_row("EOrder",        make_eorder());         // 3 ext levels (Order→UP, LI)

   std::printf("\nColumn meanings:\n");
   std::printf("  DECODE — buffer → decoded artifact (once per buffer)\n");
   std::printf("    frac validate   : validate_frac<T>(buf)      — full bounds check\n");
   std::printf("    shadow build    : unchecked shadow index     — trusts input (unsafe)\n");
   std::printf("    shadow build+chk: shadow with bounds checks  — rejects malformed input\n");
   std::printf("    unpack (from_bin): full deserialize          — allocates strings/vectors\n");
   std::printf("  READ    — iterate every field (buffer already decoded)\n");
   std::printf("    frac view       : frac_view<T>               — u32 offset loads, string_view\n");
   std::printf("    shadow view     : shadow_view<T>             — varuint decode per string\n");
   std::printf("    native struct   : pre-decoded struct         — direct C++ field access\n");
   return 0;
}
