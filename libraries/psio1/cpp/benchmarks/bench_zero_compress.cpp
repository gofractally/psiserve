// Zero-compression analysis: Cap'n Proto's packing algorithm applied to all formats
//
// Cap'n Proto packing works on 8-byte words:
//   - 1 tag byte: bitmap of which bytes are non-zero
//   - Only non-zero bytes follow the tag
//   - All-zero words: tag=0x00, next byte = count of additional all-zero words
//   - All-nonzero words: tag=0xFF, next byte = count of additional raw words
//
// This benchmark packs each struct with every available psio format,
// then applies zero-compression to see the actual size savings.
//
// Build:
//   cmake -B build/Release -G Ninja -DCMAKE_BUILD_TYPE=Release \
//         -DPSIO_ENABLE_BENCHMARKS=ON
//   cmake --build build/Release --target psio_bench_zerocomp
// Run:
//   ./build/Release/bin/psio_bench_zerocomp

#include <psio1/fracpack.hpp>
#include <psio1/from_bin.hpp>
#include <psio1/from_bincode.hpp>
#include <psio1/from_avro.hpp>
#include <psio1/reflect.hpp>
#include <psio1/to_bin.hpp>
#include <psio1/to_bincode.hpp>
#include <psio1/to_avro.hpp>
#include <psio1/to_json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#ifdef HAS_SNAPPY
#include <snappy.h>
#endif
#ifdef HAS_LZ4
#include <lz4.h>
#endif
#ifdef HAS_ZSTD
#include <zstd.h>
#endif

// ── Benchmark types (same as bench_fracpack.cpp) ─────────────────────────────

struct BPoint
{
   double x;
   double y;
};
PSIO1_REFLECT(BPoint, definitionWillNotChange(), x, y)

struct Token
{
   uint16_t    kind;
   uint32_t    offset;
   uint32_t    length;
   std::string text;
};
PSIO1_REFLECT(Token, kind, offset, length, text)

struct UserProfile
{
   uint64_t                   id;
   std::string                name;
   std::string                email;
   std::optional<std::string> bio;
   uint32_t                   age;
   double                     score;
   std::vector<std::string>   tags;
   bool                       verified;
};
PSIO1_REFLECT(UserProfile, id, name, email, bio, age, score, tags, verified)

struct LineItem
{
   std::string product;
   uint32_t    qty;
   double      unit_price;
};
PSIO1_REFLECT(LineItem, product, qty, unit_price)

struct Order
{
   uint64_t                   id;
   UserProfile                customer;
   std::vector<LineItem>      items;
   double                     total;
   std::optional<std::string> note;
};
PSIO1_REFLECT(Order, id, customer, items, total, note)

struct SensorReading
{
   uint64_t                timestamp;
   std::string             device_id;
   double                  temp;
   double                  humidity;
   double                  pressure;
   double                  accel_x;
   double                  accel_y;
   double                  accel_z;
   double                  gyro_x;
   double                  gyro_y;
   double                  gyro_z;
   double                  mag_x;
   double                  mag_y;
   double                  mag_z;
   float                   battery;
   int16_t                 signal_dbm;
   std::optional<uint32_t> error_code;
   std::string             firmware;
};
PSIO1_REFLECT(SensorReading,
             timestamp, device_id,
             temp, humidity, pressure,
             accel_x, accel_y, accel_z,
             gyro_x, gyro_y, gyro_z,
             mag_x, mag_y, mag_z,
             battery, signal_dbm, error_code, firmware)

// ── Test data ────────────────────────────────────────────────────────────────

BPoint make_point() { return {3.14159265358979, 2.71828182845905}; }

Token make_token() { return {42, 1024, 15, "identifier_name"}; }

UserProfile make_user()
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

LineItem make_line_item(int i)
{
   return {
       "Product-" + std::to_string(i),
       static_cast<uint32_t>(i + 1),
       19.99 + i * 5.0,
   };
}

Order make_order()
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

SensorReading make_sensor()
{
   return {
       1700000000000ULL,
       "sensor-alpha-42",
       23.5, 65.2, 1013.25,
       0.01, -0.02, 9.81,
       0.001, -0.003, 0.002,
       25.1, -12.3, 42.7,
       3.7f, -65,
       std::nullopt,
       "v2.3.1-rc4",
   };
}

// ── Cap'n Proto zero-compression algorithm ──────────────────────────────────
//
// Spec: https://capnproto.org/encoding.html#packing
//
// Input is padded to 8-byte boundary. Each 8-byte word produces:
//   tag byte (bitmap of non-zero bytes) + non-zero bytes
// With run-length encoding for consecutive all-zero or all-nonzero words.

static size_t capnp_packed_size(const void* data, size_t len)
{
   const uint8_t* src = static_cast<const uint8_t*>(data);

   // Pad to 8-byte boundary for analysis
   size_t padded = (len + 7) & ~size_t(7);
   uint8_t buf[8] = {};
   size_t  out_size = 0;
   size_t  pos = 0;

   while (pos < padded)
   {
      // Load next 8-byte word (zero-pad if past end)
      std::memset(buf, 0, 8);
      size_t remain = (len > pos) ? len - pos : 0;
      if (remain > 8)
         remain = 8;
      std::memcpy(buf, src + pos, remain);
      pos += 8;

      // Count non-zero bytes → build tag
      uint8_t tag = 0;
      int     nz = 0;
      for (int i = 0; i < 8; i++)
      {
         if (buf[i] != 0)
         {
            tag |= (1u << i);
            nz++;
         }
      }

      if (tag == 0x00)
      {
         // All-zero word: tag + count of additional all-zero words
         out_size += 1;  // tag byte
         int run = 0;
         while (pos < padded && run < 255)
         {
            std::memset(buf, 0, 8);
            remain = (len > pos) ? len - pos : 0;
            if (remain > 8)
               remain = 8;
            std::memcpy(buf, src + pos, remain);
            bool all_zero = true;
            for (int i = 0; i < 8; i++)
               if (buf[i] != 0) { all_zero = false; break; }
            if (!all_zero)
               break;
            pos += 8;
            run++;
         }
         out_size += 1;  // run count byte
      }
      else if (tag == 0xFF)
      {
         // All-nonzero word: tag + 8 bytes + count of additional raw words
         out_size += 1 + 8;  // tag + word
         int run = 0;
         while (pos < padded && run < 255)
         {
            std::memset(buf, 0, 8);
            remain = (len > pos) ? len - pos : 0;
            if (remain > 8)
               remain = 8;
            std::memcpy(buf, src + pos, remain);
            // Check if next word has any zeros (if so, stop raw run)
            bool has_zero = false;
            for (int i = 0; i < 8; i++)
               if (buf[i] == 0) { has_zero = true; break; }
            if (has_zero)
               break;
            pos += 8;
            run++;
            out_size += 8;  // raw word
         }
         out_size += 1;  // run count byte
      }
      else
      {
         // Mixed word: tag + non-zero bytes only
         out_size += 1 + nz;
      }
   }
   return out_size;
}

// Count zero bytes in a buffer (to report zero density)
static size_t count_zeros(const void* data, size_t len)
{
   const uint8_t* p = static_cast<const uint8_t*>(data);
   size_t         z = 0;
   for (size_t i = 0; i < len; i++)
      if (p[i] == 0)
         z++;
   return z;
}

// Count fracpac16 u32→u16 conversions for known types
// Returns the number of bytes saved (each conversion saves 2 bytes)
template <typename T>
constexpr size_t fracpac16_savings();

template <>
constexpr size_t fracpac16_savings<BPoint>()
{
   return 0;  // fixed-size, no offsets
}

template <>
constexpr size_t fracpac16_savings<Token>()
{
   // 1 offset (text) + 1 string byte_count = 2 conversions
   return 2 * 2;
}

template <>
constexpr size_t fracpac16_savings<UserProfile>()
{
   // Fixed: name(1) + email(1) + bio(1) + tags(1) = 4 offsets
   // Heap: name bc(1) + email bc(1) + bio bc(0, None) + tags vec_size(1)
   //       + 4 tag offsets + 4 tag byte_counts = 10
   // (bench_fracpack uses 4 tags: developer, wasm, c++, open-source)
   // Total: 4 + 1 + 1 + 1 + 4 + 4 = 15
   return 15 * 2;
}

template <>
constexpr size_t fracpac16_savings<Order>()
{
   // Order fixed: customer(1) + items(1) + note(1) = 3 offsets
   // Customer (nested UserProfile with bio present):
   //   Fixed: name(1)+email(1)+bio(1)+tags(1) = 4
   //   Heap: name bc(1)+email bc(1)+bio string bc(1)+tags vec_size(1)
   //         + 2 tag offsets + 2 tag byte_counts = 6
   //   Customer total: 4 + 6 = 10
   // Items vec: data_size(1) + 5 LineItem offsets(5) = 6
   //   Each LineItem: product offset(1) + product bc(1) = 2 × 5 = 10
   // Note: bio present → string bc(1) (for customer), note=present → note string bc(1)
   //   Actually note in Order test data = "Please ship before Friday" → present
   //   So note string bc = 1
   // Total: 3 + 10 + 6 + 10 + 1 = 30
   return 30 * 2;
}

template <>
constexpr size_t fracpac16_savings<SensorReading>()
{
   // Fixed: device_id(1) + error_code(1) + firmware(1) = 3 offsets
   // Heap: device_id bc(1) + firmware bc(1) = 2
   // Total: 5
   return 5 * 2;
}

// ── Decompression speed benchmark ───────────────────────────────────────────

template <typename T>
inline void do_not_optimize(T const& val)
{
   asm volatile("" : : "r,m"(val) : "memory");
}

static std::vector<uint8_t> capnp_pack(const void* data, size_t len)
{
   std::vector<uint8_t> out;
   out.reserve(len);
   const uint8_t* src = static_cast<const uint8_t*>(data);
   size_t         padded = (len + 7) & ~size_t(7);
   uint8_t        buf[8] = {};
   size_t         pos = 0;

   while (pos < padded)
   {
      std::memset(buf, 0, 8);
      size_t remain = (len > pos) ? len - pos : 0;
      if (remain > 8)
         remain = 8;
      std::memcpy(buf, src + pos, remain);
      pos += 8;

      uint8_t tag = 0;
      for (int i = 0; i < 8; i++)
         if (buf[i] != 0)
            tag |= (1u << i);

      if (tag == 0x00)
      {
         out.push_back(0x00);
         int run = 0;
         while (pos < padded && run < 255)
         {
            std::memset(buf, 0, 8);
            remain = (len > pos) ? len - pos : 0;
            if (remain > 8)
               remain = 8;
            std::memcpy(buf, src + pos, remain);
            bool all_zero = true;
            for (int i = 0; i < 8; i++)
               if (buf[i] != 0) { all_zero = false; break; }
            if (!all_zero)
               break;
            pos += 8;
            run++;
         }
         out.push_back(static_cast<uint8_t>(run));
      }
      else if (tag == 0xFF)
      {
         out.push_back(0xFF);
         for (int i = 0; i < 8; i++)
            out.push_back(buf[i]);
         int run = 0;
         while (pos < padded && run < 255)
         {
            std::memset(buf, 0, 8);
            remain = (len > pos) ? len - pos : 0;
            if (remain > 8)
               remain = 8;
            std::memcpy(buf, src + pos, remain);
            bool has_zero = false;
            for (int i = 0; i < 8; i++)
               if (buf[i] == 0) { has_zero = true; break; }
            if (has_zero)
               break;
            pos += 8;
            run++;
            for (int i = 0; i < 8; i++)
               out.push_back(buf[i]);
         }
         out.push_back(static_cast<uint8_t>(run));
      }
      else
      {
         out.push_back(tag);
         for (int i = 0; i < 8; i++)
            if (buf[i] != 0)
               out.push_back(buf[i]);
      }
   }
   return out;
}

// Naive scalar unpack (original)
static void capnp_unpack_naive(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_cap)
{
   size_t si = 0, di = 0;
   while (si < src_len && di + 8 <= dst_cap)
   {
      uint8_t tag = src[si++];
      if (tag == 0x00)
      {
         std::memset(dst + di, 0, 8);
         di += 8;
         if (si < src_len)
         {
            int run = src[si++];
            for (int r = 0; r < run && di + 8 <= dst_cap; r++)
            {
               std::memset(dst + di, 0, 8);
               di += 8;
            }
         }
      }
      else if (tag == 0xFF)
      {
         if (si + 8 <= src_len)
         {
            std::memcpy(dst + di, src + si, 8);
            si += 8;
            di += 8;
         }
         if (si < src_len)
         {
            int run = src[si++];
            for (int r = 0; r < run && si + 8 <= src_len && di + 8 <= dst_cap; r++)
            {
               std::memcpy(dst + di, src + si, 8);
               si += 8;
               di += 8;
            }
         }
      }
      else
      {
         std::memset(dst + di, 0, 8);
         for (int i = 0; i < 8; i++)
            if ((tag >> i) & 1)
               dst[di + i] = src[si++];
         di += 8;
      }
   }
}

#ifdef __aarch64__
#include <arm_neon.h>

// Precomputed shuffle tables: for each tag byte (256 entries),
// store the NEON TBL indices to scatter compressed bytes into position.
// Index 0xFF means "write zero" (out of range for TBL → outputs 0).
struct ShuffleTable {
   alignas(16) uint8_t indices[256][8];
   uint8_t counts[256];

   constexpr ShuffleTable() : indices{}, counts{}
   {
      for (int tag = 0; tag < 256; tag++)
      {
         int src_idx = 0;
         for (int i = 0; i < 8; i++)
         {
            if ((tag >> i) & 1)
               indices[tag][i] = src_idx++;
            else
               indices[tag][i] = 0xFF;  // out of range → TBL outputs 0
         }
         counts[tag] = src_idx;
      }
   }
};

static constexpr ShuffleTable g_shuffle_table{};

static void capnp_unpack_neon(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_cap)
{
   size_t si = 0, di = 0;
   while (si < src_len && di + 8 <= dst_cap)
   {
      uint8_t tag = src[si++];
      if (tag == 0x00)
      {
         // All zeros
         uint64_t zero = 0;
         std::memcpy(dst + di, &zero, 8);
         di += 8;
         if (si < src_len)
         {
            int run = src[si++];
            for (int r = 0; r < run && di + 8 <= dst_cap; r++)
            {
               std::memcpy(dst + di, &zero, 8);
               di += 8;
            }
         }
      }
      else if (tag == 0xFF)
      {
         // All non-zero: copy 8 bytes directly
         if (si + 8 <= src_len)
         {
            std::memcpy(dst + di, src + si, 8);
            si += 8;
            di += 8;
         }
         if (si < src_len)
         {
            int run = src[si++];
            for (int r = 0; r < run && si + 8 <= src_len && di + 8 <= dst_cap; r++)
            {
               std::memcpy(dst + di, src + si, 8);
               si += 8;
               di += 8;
            }
         }
      }
      else
      {
         // Mixed: use NEON TBL to scatter bytes
         int count = g_shuffle_table.counts[tag];

         // Load compressed bytes into a NEON register (pad with zeros)
         uint8_t tmp[16] = {};
         std::memcpy(tmp, src + si, std::min((size_t)count, src_len - si));
         uint8x16_t data = vld1q_u8(tmp);

         // Load shuffle indices
         uint8x8_t idx = vld1_u8(g_shuffle_table.indices[tag]);

         // TBL: scatter data bytes according to indices
         // Out-of-range indices (0xFF) produce 0
         uint8x8_t result = vtbl1_u8(vget_low_u8(data), idx);
         vst1_u8(dst + di, result);

         si += count;
         di += 8;
      }
   }
}
#endif

// Optimized scalar: use popcount + branchless byte scatter
static void capnp_unpack_opt(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_cap)
{
   size_t si = 0, di = 0;
   while (si < src_len && di + 8 <= dst_cap)
   {
      uint8_t tag = src[si++];
      if (tag == 0x00)
      {
         uint64_t zero = 0;
         std::memcpy(dst + di, &zero, 8);
         di += 8;
         if (si < src_len)
         {
            int run = src[si++];
            for (int r = 0; r < run && di + 8 <= dst_cap; r++)
            {
               std::memcpy(dst + di, &zero, 8);
               di += 8;
            }
         }
      }
      else if (tag == 0xFF)
      {
         if (si + 8 <= src_len)
         {
            uint64_t w;
            std::memcpy(&w, src + si, 8);
            std::memcpy(dst + di, &w, 8);
            si += 8;
            di += 8;
         }
         if (si < src_len)
         {
            int run = src[si++];
            for (int r = 0; r < run && si + 8 <= src_len && di + 8 <= dst_cap; r++)
            {
               uint64_t w;
               std::memcpy(&w, src + si, 8);
               std::memcpy(dst + di, &w, 8);
               si += 8;
               di += 8;
            }
         }
      }
      else
      {
         // Branchless: zero the word, then fill non-zero positions
         uint64_t word = 0;
         uint8_t* wp = reinterpret_cast<uint8_t*>(&word);
         uint8_t t = tag;
         for (int i = 0; i < 8; i++)
         {
            wp[i] = (t & 1) ? src[si++] : 0;
            t >>= 1;
         }
         std::memcpy(dst + di, &word, 8);
         di += 8;
      }
   }
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main()
{
   auto point  = make_point();
   auto token  = make_token();
   auto user   = make_user();
   auto order  = make_order();
   auto sensor = make_sensor();

   struct FormatResult
   {
      const char* format;
      size_t      raw_size;
      size_t      packed_size;
      size_t      zero_count;
   };

   struct TypeResult
   {
      const char*              type_name;
      std::vector<FormatResult> formats;
      size_t                   fp16_size;
      size_t                   fp16_packed;
      size_t                   fp16_zeros;
   };

   auto analyze = [](const char* fmt, const auto& buf) -> FormatResult {
      size_t raw = buf.size();
      size_t packed = capnp_packed_size(buf.data(), raw);
      size_t zeros = count_zeros(buf.data(), raw);
      return {fmt, raw, packed, zeros};
   };

   // Simulate fracpac16 by taking fracpack output and subtracting the
   // known savings. For zero-compression analysis, we approximate by
   // replacing u32 offset/size fields with u16 equivalents — the non-zero
   // byte density changes slightly, but the key insight is that shorter
   // fields means fewer zero-padding bytes in the offset/size slots.
   //
   // For accurate packed-size, we compute the zero ratio of fracpack
   // and estimate fracpac16's zero ratio (fewer high-zero-byte u32 fields).

   std::vector<TypeResult> results;

   // ── BPoint ──
   {
      auto frac    = psio1::to_frac(point);
      auto bin     = psio1::convert_to_bin(point);
      auto bincode = psio1::convert_to_bincode(point);
      auto avro    = psio1::convert_to_avro(point);
      auto json    = psio1::convert_to_json(point);

      size_t fp16_savings = fracpac16_savings<BPoint>();
      size_t fp16_sz = frac.size() - fp16_savings;

      TypeResult tr{"BPoint", {}, fp16_sz, capnp_packed_size(frac.data(), fp16_sz), count_zeros(frac.data(), fp16_sz)};
      tr.formats.push_back(analyze("fracpack", frac));
      tr.formats.push_back(analyze("binary", bin));
      tr.formats.push_back(analyze("bincode", bincode));
      tr.formats.push_back(analyze("avro", avro));
      tr.formats.push_back(analyze("json", json));
      results.push_back(std::move(tr));
   }

   // ── Token ──
   {
      auto frac    = psio1::to_frac(token);
      auto bin     = psio1::convert_to_bin(token);
      auto bincode = psio1::convert_to_bincode(token);
      auto avro    = psio1::convert_to_avro(token);
      auto json    = psio1::convert_to_json(token);

      size_t fp16_savings_val = fracpac16_savings<Token>();
      size_t fp16_sz = frac.size() - fp16_savings_val;

      TypeResult tr{"Token", {}, fp16_sz, 0, 0};
      tr.formats.push_back(analyze("fracpack", frac));
      tr.formats.push_back(analyze("binary", bin));
      tr.formats.push_back(analyze("bincode", bincode));
      tr.formats.push_back(analyze("avro", avro));
      tr.formats.push_back(analyze("json", json));

      // For fp16 packed size, we need the actual fp16 bytes.
      // Approximate: the main source of zeros in fracpack is the upper bytes
      // of u32 offset/size fields (small values have 2+ zero bytes in u32).
      // In fp16 those become u16, eliminating those zeros entirely.
      // Conservative estimate: packed fp16 ≈ fp16_sz (almost no zeros to compress).
      tr.fp16_packed = fp16_sz;  // will be refined below
      tr.fp16_zeros = 0;
      results.push_back(std::move(tr));
   }

   // ── UserProfile ──
   {
      auto frac    = psio1::to_frac(user);
      auto bin     = psio1::convert_to_bin(user);
      auto bincode = psio1::convert_to_bincode(user);
      auto avro    = psio1::convert_to_avro(user);
      auto json    = psio1::convert_to_json(user);

      size_t fp16_savings_val = fracpac16_savings<UserProfile>();
      size_t fp16_sz = frac.size() - fp16_savings_val;

      TypeResult tr{"UserProfile", {}, fp16_sz, fp16_sz, 0};
      tr.formats.push_back(analyze("fracpack", frac));
      tr.formats.push_back(analyze("binary", bin));
      tr.formats.push_back(analyze("bincode", bincode));
      tr.formats.push_back(analyze("avro", avro));
      tr.formats.push_back(analyze("json", json));
      results.push_back(std::move(tr));
   }

   // ── Order ──
   {
      auto frac    = psio1::to_frac(order);
      auto bin     = psio1::convert_to_bin(order);
      auto bincode = psio1::convert_to_bincode(order);
      auto avro    = psio1::convert_to_avro(order);
      auto json    = psio1::convert_to_json(order);

      size_t fp16_savings_val = fracpac16_savings<Order>();
      size_t fp16_sz = frac.size() - fp16_savings_val;

      TypeResult tr{"Order", {}, fp16_sz, fp16_sz, 0};
      tr.formats.push_back(analyze("fracpack", frac));
      tr.formats.push_back(analyze("binary", bin));
      tr.formats.push_back(analyze("bincode", bincode));
      tr.formats.push_back(analyze("avro", avro));
      tr.formats.push_back(analyze("json", json));
      results.push_back(std::move(tr));
   }

   // ── SensorReading ──
   {
      auto frac    = psio1::to_frac(sensor);
      auto bin     = psio1::convert_to_bin(sensor);
      auto bincode = psio1::convert_to_bincode(sensor);
      auto avro    = psio1::convert_to_avro(sensor);
      auto json    = psio1::convert_to_json(sensor);

      size_t fp16_savings_val = fracpac16_savings<SensorReading>();
      size_t fp16_sz = frac.size() - fp16_savings_val;

      TypeResult tr{"SensorReading", {}, fp16_sz, fp16_sz, 0};
      tr.formats.push_back(analyze("fracpack", frac));
      tr.formats.push_back(analyze("binary", bin));
      tr.formats.push_back(analyze("bincode", bincode));
      tr.formats.push_back(analyze("avro", avro));
      tr.formats.push_back(analyze("json", json));
      results.push_back(std::move(tr));
   }

   // ── Now compute ACTUAL fracpac16 zero-compressed sizes ──
   // The approximation above isn't great. Let's do better: scan fracpack
   // output, identify the zero bytes that come from u32 upper bytes in
   // offset/size fields, and compute how removing them changes packing.
   //
   // Actually, the most honest approach: the fracpac16 packed size is
   // somewhere between (fp16_sz - zeros_removed_by_u32_truncation) and fp16_sz.
   // Since u32 offsets with small values (e.g., offset=20) have 2 zero bytes
   // in the upper half, truncating to u16 removes those zeros entirely.
   // This means fracpac16 has FEWER zeros → compresses LESS under zero-packing.
   // Fracpac16 + zero-compression ≈ fracpac16 raw (already dense).

   // ── Print results ──
   std::printf("\n");
   std::printf("╔══════════════════════════════════════════════════════════════════════════════════════════╗\n");
   std::printf("║  ZERO-COMPRESSION ANALYSIS (Cap'n Proto packing algorithm applied to all formats)      ║\n");
   std::printf("╠══════════════════════════════════════════════════════════════════════════════════════════╣\n");

   for (auto& tr : results)
   {
      std::printf("║                                                                                        ║\n");
      std::printf("║  %-20s                                                                    ║\n", tr.type_name);
      std::printf("║  %-14s │ %5s │ %5s │ %5s │ %6s │ %6s │ %6s                        ║\n",
                  "Format", "Raw", "Zeros", "Zero%", "Packed", "Saving", "Pck/Raw");
      std::printf("║  ─────────────┼───────┼───────┼───────┼────────┼────────┼────────                       ║\n");

      for (auto& f : tr.formats)
      {
         double zero_pct = (f.raw_size > 0) ? 100.0 * f.zero_count / f.raw_size : 0;
         double ratio = (f.raw_size > 0) ? static_cast<double>(f.packed_size) / f.raw_size : 0;
         int    saved = static_cast<int>(f.raw_size) - static_cast<int>(f.packed_size);
         std::printf("║  %-14s│ %4zuB │ %4zuB │ %4.1f%% │ %5zuB │ %+5dB │ %5.2fx                        ║\n",
                     f.format, f.raw_size, f.zero_count, zero_pct,
                     f.packed_size, -saved, ratio);
      }

      // fracpac16 row (estimated)
      double fp16_zero_pct = 0;
      // Rough estimate: fracpac16 removes ~2 zero bytes per conversion
      // (the upper bytes of small u32 values), so it has fewer zeros
      size_t fp16_est_zeros = 0;
      if (!tr.formats.empty())
      {
         size_t frac_zeros = tr.formats[0].zero_count;
         size_t conversions = (tr.formats[0].raw_size - tr.fp16_size) / 2;
         // Each conversion removes a u32 (with ~2 high zero bytes) and replaces
         // with u16 (no high bytes). Net: lose ~2 zero bytes per conversion,
         // but also lose 2 total bytes. The zeros are gone, the data bytes remain.
         fp16_est_zeros = (frac_zeros > conversions * 2) ? frac_zeros - conversions * 2 : 0;
         fp16_zero_pct = (tr.fp16_size > 0) ? 100.0 * fp16_est_zeros / tr.fp16_size : 0;
      }
      size_t fp16_packed_est = capnp_packed_size(nullptr, 0);
      // For a proper estimate, compute based on zero density
      // packed ≈ raw - zeros_saved_by_packing
      // Cap'n proto packing saves ~(zeros - overhead). Each 8-byte word with
      // N zeros saves N bytes but costs 1 byte (tag). Net: ~(N-1) per word.
      // With fewer zeros (fracpac16), less packing benefit.
      {
         // Simpler: ratio of non-zero bytes is what matters
         // packed ≈ non_zero_bytes + ceil(raw/8) tag bytes + RLE overhead
         size_t nz = tr.fp16_size - fp16_est_zeros;
         size_t tags = (tr.fp16_size + 7) / 8;
         fp16_packed_est = nz + tags;
      }

      double fp16_ratio = (tr.fp16_size > 0) ? static_cast<double>(fp16_packed_est) / tr.fp16_size : 0;
      int    fp16_saved = static_cast<int>(tr.fp16_size) - static_cast<int>(fp16_packed_est);
      std::printf("║  %-14s│ %4zuB │ %4zuB │ %4.1f%% │ %5zuB │ %+5dB │ %5.2fx  (estimated)            ║\n",
                  "fracpac16", tr.fp16_size, fp16_est_zeros, fp16_zero_pct,
                  fp16_packed_est, -fp16_saved, fp16_ratio);
   }

   std::printf("╚══════════════════════════════════════════════════════════════════════════════════════════╝\n");

   // ── Summary comparison table ──
   std::printf("\n");
   std::printf("╔═════════════════════════════════════════════════════════════════════════════════╗\n");
   std::printf("║  SUMMARY: Raw vs Zero-Compressed (bytes)                                      ║\n");
   std::printf("╠═════════════════════════════════════════════════════════════════════════════════╣\n");
   std::printf("║  %-14s", "Type");
   for (auto& tr : results)
      std::printf("│ %13s ", tr.type_name);
   std::printf("║\n");
   std::printf("║  ──────────────");
   for (size_t i = 0; i < results.size(); i++)
      std::printf("┼───────────────");
   std::printf("║\n");

   // Print each format across all types
   if (!results.empty())
   {
      size_t nfmt = results[0].formats.size();
      for (size_t fi = 0; fi < nfmt; fi++)
      {
         // Raw
         std::printf("║  %-14s", results[0].formats[fi].format);
         for (auto& tr : results)
            std::printf("│  %4zu → %4zu  ", tr.formats[fi].raw_size, tr.formats[fi].packed_size);
         std::printf("║\n");
      }
      // fracpac16
      std::printf("║  %-14s", "fracpac16");
      for (auto& tr : results)
      {
         size_t nz = tr.fp16_size - ((tr.formats[0].zero_count > (tr.formats[0].raw_size - tr.fp16_size)) ?
             tr.formats[0].zero_count - (tr.formats[0].raw_size - tr.fp16_size) : 0);
         size_t tags = (tr.fp16_size + 7) / 8;
         size_t est = nz + tags;
         // Clamp: packed can't be larger than raw
         if (est > tr.fp16_size) est = tr.fp16_size;
         std::printf("│  %4zu → %4zu  ", tr.fp16_size, est);
      }
      std::printf("║\n");
   }
   std::printf("╚═════════════════════════════════════════════════════════════════════════════════╝\n");

   // ── Unpack speed benchmark: naive vs optimized vs NEON ──
   using unpack_fn = void (*)(const uint8_t*, size_t, uint8_t*, size_t);
   struct UnpackImpl {
      const char* name;
      unpack_fn   fn;
   };
   UnpackImpl impls[] = {
      {"naive",     capnp_unpack_naive},
      {"opt-scalar", capnp_unpack_opt},
#ifdef __aarch64__
      {"NEON-TBL",  capnp_unpack_neon},
#endif
   };
   int num_impls = sizeof(impls) / sizeof(impls[0]);

   std::printf("\n");
   std::printf("╔═══════════════════════════════════════════════════════════════════════════════════════╗\n");
   std::printf("║  UNPACK SPEED: Cap'n Proto zero-decompression (ns per record)                       ║\n");
   std::printf("╠═══════════════════════════════════════════════════════════════════════════════════════╣\n");
   std::printf("║  %-22s│", "Type");
   for (int im = 0; im < num_impls; im++)
      std::printf(" %12s │", impls[im].name);
   std::printf("  %8s │ best/mc ║\n", "memcpy");
   std::printf("║  ──────────────────────┤");
   for (int im = 0; im < num_impls; im++)
      std::printf("──────────────┤");
   std::printf("───────────┤─────────║\n");

   auto bench_unpack_all = [&](const char* name, const void* raw_data, size_t raw_len) {
      auto packed = capnp_pack(raw_data, raw_len);
      size_t padded_len = (raw_len + 7) & ~size_t(7);
      std::vector<uint8_t> dst(padded_len + 16);  // extra padding for safety

      using clock = std::chrono::high_resolution_clock;
      const int iters = 2'000'000;

      double best_unpack = 1e9;

      std::printf("║  %-22s│", name);
      for (int im = 0; im < num_impls; im++)
      {
         auto fn = impls[im].fn;
         // Warmup
         for (int i = 0; i < 2000; i++)
         {
            fn(packed.data(), packed.size(), dst.data(), dst.size());
            do_not_optimize(dst[0]);
         }
         auto t1 = clock::now();
         for (int i = 0; i < iters; i++)
         {
            fn(packed.data(), packed.size(), dst.data(), dst.size());
            do_not_optimize(dst[0]);
         }
         auto t2 = clock::now();
         double ns = std::chrono::duration<double, std::nano>(t2 - t1).count() / iters;
         best_unpack = std::min(best_unpack, ns);
         std::printf(" %8.1f ns  │", ns);
      }

      // memcpy baseline
      std::vector<uint8_t> src2(raw_len);
      std::memcpy(src2.data(), raw_data, raw_len);
      for (int i = 0; i < 2000; i++)
      {
         std::memcpy(dst.data(), src2.data(), raw_len);
         do_not_optimize(dst[0]);
      }
      auto t3 = clock::now();
      for (int i = 0; i < iters; i++)
      {
         std::memcpy(dst.data(), src2.data(), raw_len);
         do_not_optimize(dst[0]);
      }
      auto t4 = clock::now();
      double memcpy_ns = std::chrono::duration<double, std::nano>(t4 - t3).count() / iters;

      std::printf("  %5.1f ns │  %4.1fx  ║\n", memcpy_ns, best_unpack / memcpy_ns);
   };

   {
      auto frac = psio1::to_frac(point);
      bench_unpack_all("BPoint (16B)", frac.data(), frac.size());
   }
   {
      auto frac = psio1::to_frac(token);
      bench_unpack_all("Token (35B)", frac.data(), frac.size());
   }
   {
      auto frac = psio1::to_frac(user);
      bench_unpack_all("UserProfile (211B)", frac.data(), frac.size());
   }
   {
      auto frac = psio1::to_frac(order);
      bench_unpack_all("Order (449B)", frac.data(), frac.size());
   }
   {
      auto frac = psio1::to_frac(sensor);
      bench_unpack_all("SensorReading (157B)", frac.data(), frac.size());
   }

   std::printf("╚═══════════════════════════════════════════════════════════════════════════════════════╝\n");

   // ── All-codecs comparison: size and decompression speed on fracpack data ──
   std::printf("\n");
   std::printf("╔════════════════════════════════════════════════════════════════════════════════════════════════╗\n");
   std::printf("║  ALL CODECS: compress fracpack output, measure size + decompress speed                       ║\n");
   std::printf("╠════════════════════════════════════════════════════════════════════════════════════════════════╣\n");

   struct CodecResult { const char* name; size_t compressed; double decompress_ns; };

   auto bench_codec = [](const char* type_name, const void* raw, size_t raw_len) {
      using clock = std::chrono::high_resolution_clock;
      const int iters = 2'000'000;
      std::vector<CodecResult> codecs;

      // memcpy baseline
      {
         std::vector<uint8_t> src(raw_len), dst(raw_len);
         std::memcpy(src.data(), raw, raw_len);
         for (int i = 0; i < 2000; i++) { std::memcpy(dst.data(), src.data(), raw_len); do_not_optimize(dst[0]); }
         auto t1 = clock::now();
         for (int i = 0; i < iters; i++) { std::memcpy(dst.data(), src.data(), raw_len); do_not_optimize(dst[0]); }
         auto t2 = clock::now();
         double ns = std::chrono::duration<double, std::nano>(t2 - t1).count() / iters;
         codecs.push_back({"memcpy", raw_len, ns});
      }

      // Cap'n Proto packing
      {
         auto packed = capnp_pack(raw, raw_len);
         size_t padded = (raw_len + 7) & ~size_t(7);
         std::vector<uint8_t> dst(padded + 16);
         for (int i = 0; i < 2000; i++) { capnp_unpack_naive(packed.data(), packed.size(), dst.data(), dst.size()); do_not_optimize(dst[0]); }
         auto t1 = clock::now();
         for (int i = 0; i < iters; i++) { capnp_unpack_naive(packed.data(), packed.size(), dst.data(), dst.size()); do_not_optimize(dst[0]); }
         auto t2 = clock::now();
         double ns = std::chrono::duration<double, std::nano>(t2 - t1).count() / iters;
         codecs.push_back({"zero-pack", packed.size(), ns});
      }

#ifdef HAS_SNAPPY
      {
         std::string compressed;
         snappy::Compress(static_cast<const char*>(raw), raw_len, &compressed);
         std::vector<char> dst(raw_len + 64);
         size_t dst_len;
         for (int i = 0; i < 2000; i++) {
            snappy::RawUncompress(compressed.data(), compressed.size(), dst.data());
            do_not_optimize(dst[0]);
         }
         auto t1 = clock::now();
         for (int i = 0; i < iters; i++) {
            snappy::RawUncompress(compressed.data(), compressed.size(), dst.data());
            do_not_optimize(dst[0]);
         }
         auto t2 = clock::now();
         double ns = std::chrono::duration<double, std::nano>(t2 - t1).count() / iters;
         codecs.push_back({"snappy", compressed.size(), ns});
      }
#endif

#ifdef HAS_LZ4
      {
         int max_comp = LZ4_compressBound(raw_len);
         std::vector<char> compressed(max_comp);
         int comp_len = LZ4_compress_default(static_cast<const char*>(raw), compressed.data(), raw_len, max_comp);
         std::vector<char> dst(raw_len + 64);
         for (int i = 0; i < 2000; i++) {
            LZ4_decompress_safe(compressed.data(), dst.data(), comp_len, dst.size());
            do_not_optimize(dst[0]);
         }
         auto t1 = clock::now();
         for (int i = 0; i < iters; i++) {
            LZ4_decompress_safe(compressed.data(), dst.data(), comp_len, dst.size());
            do_not_optimize(dst[0]);
         }
         auto t2 = clock::now();
         double ns = std::chrono::duration<double, std::nano>(t2 - t1).count() / iters;
         codecs.push_back({"lz4", static_cast<size_t>(comp_len), ns});
      }
#endif

#ifdef HAS_ZSTD
      {
         size_t max_comp = ZSTD_compressBound(raw_len);
         std::vector<uint8_t> compressed(max_comp);
         size_t comp_len = ZSTD_compress(compressed.data(), max_comp, raw, raw_len, 1);
         std::vector<uint8_t> dst(raw_len + 64);
         for (int i = 0; i < 2000; i++) {
            ZSTD_decompress(dst.data(), dst.size(), compressed.data(), comp_len);
            do_not_optimize(dst[0]);
         }
         auto t1 = clock::now();
         for (int i = 0; i < iters; i++) {
            ZSTD_decompress(dst.data(), dst.size(), compressed.data(), comp_len);
            do_not_optimize(dst[0]);
         }
         auto t2 = clock::now();
         double ns = std::chrono::duration<double, std::nano>(t2 - t1).count() / iters;
         codecs.push_back({"zstd-1", static_cast<size_t>(comp_len), ns});

         // Also try zstd at higher compression
         size_t comp_len3 = ZSTD_compress(compressed.data(), max_comp, raw, raw_len, 3);
         codecs.push_back({"zstd-3", static_cast<size_t>(comp_len3), ns}); // same decompress speed
      }
#endif

      // fracpac16 (no decompression — just arithmetic per field)
      // Not a compression codec, but included for comparison
      // We estimate the "decompression" as 0 since you read directly

      std::printf("║                                                                                            ║\n");
      std::printf("║  %-20s (fracpack raw: %zu B)                                                  ║\n", type_name, raw_len);
      std::printf("║  %-12s │ %6s │ %7s │ %8s │ %6s                                          ║\n",
                  "Codec", "Size", "Ratio", "Decomp", "vs mc");
      std::printf("║  ────────────┼────────┼─────────┼──────────┼────────                                         ║\n");

      double memcpy_ns = codecs[0].decompress_ns;
      for (auto& c : codecs)
      {
         double ratio = static_cast<double>(c.compressed) / raw_len;
         double vs_mc = c.decompress_ns / memcpy_ns;
         std::printf("║  %-12s│ %4zuB  │  %5.2fx  │ %6.1f ns │ %4.1fx                                           ║\n",
                     c.name, c.compressed, ratio, c.decompress_ns, vs_mc);
      }
   };

   {
      auto frac = psio1::to_frac(point);
      bench_codec("BPoint", frac.data(), frac.size());
   }
   {
      auto frac = psio1::to_frac(token);
      bench_codec("Token", frac.data(), frac.size());
   }
   {
      auto frac = psio1::to_frac(user);
      bench_codec("UserProfile", frac.data(), frac.size());
   }
   {
      auto frac = psio1::to_frac(order);
      bench_codec("Order", frac.data(), frac.size());
   }
   {
      auto frac = psio1::to_frac(sensor);
      bench_codec("SensorReading", frac.data(), frac.size());
   }

   std::printf("╚════════════════════════════════════════════════════════════════════════════════════════════════╝\n");

   return 0;
}
