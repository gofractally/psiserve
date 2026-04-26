// Fracpack size/perf comparison: frac_format_32 vs frac_format_16.
//
// Uses the same benchmark types as bench_fracpack.cpp so the results
// are directly comparable to the CapNProto / FlatBuffers benchmarks
// in this directory.
//
// Build:
//   cmake -B build/Release -G Ninja -DCMAKE_BUILD_TYPE=Release \
//         -DPSIO_ENABLE_BENCHMARKS=ON
//   cmake --build build/Release --target psio_bench16
// Run:
//   ./build/Release/bin/psio_bench16

#include <psio1/fracpack.hpp>

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

template <typename T>
inline void do_not_optimize(T const& val)
{
   asm volatile("" : : "r,m"(val) : "memory");
}
inline void clobber_memory()
{
   asm volatile("" ::: "memory");
}

// ── Same types as bench_fracpack.cpp / bench_schemas.capnp / .fbs ─────────

struct BPoint
{
   double x, y;
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
   double                  temp, humidity, pressure;
   double                  accel_x, accel_y, accel_z;
   double                  gyro_x, gyro_y, gyro_z;
   double                  mag_x, mag_y, mag_z;
   float                   battery;
   int16_t                 signal_dbm;
   std::optional<uint32_t> error_code;
   std::string             firmware;
};
PSIO1_REFLECT(SensorReading,
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

// ── Test data factories ───────────────────────────────────────────────────

BPoint make_point()
{
   return {3.14159265358979, 2.71828182845905};
}
Token make_token()
{
   return {42, 1024, 15, "identifier_name"};
}
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
       1700000000000ULL, "sensor-alpha-42", 23.5,  65.2,  1013.25,  0.01,  -0.02, 9.81,
       0.001,            -0.003,            0.002, 25.1,  -12.3,    42.7,  3.7f,  -65,
       std::nullopt,     "v2.3.1-rc4",
   };
}

// ── Bench harness (same shape as bench_fracpack.cpp) ──────────────────────

template <typename Fn>
double bench_ns(Fn fn)
{
   using clock = std::chrono::high_resolution_clock;
   for (int i = 0; i < 200; ++i)
   {
      fn();
      clobber_memory();
   }
   size_t cal_iters = 0;
   auto   cal_start = clock::now();
   while (std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - cal_start).count() <
          30'000)
   {
      fn();
      clobber_memory();
      ++cal_iters;
   }
   auto cal_us =
       std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - cal_start).count();
   double ns_per_op = cal_iters > 0 ? (cal_us * 1000.0 / cal_iters) : 1.0;

   size_t batch = 1;
   if (ns_per_op < 100.0)
      batch = std::max<size_t>(1, static_cast<size_t>(100.0 / std::max(ns_per_op, 0.01)));
   size_t target = std::max<size_t>(
       1000, static_cast<size_t>(200'000'000.0 / (ns_per_op * static_cast<double>(batch))));

   auto start = clock::now();
   for (size_t i = 0; i < target; ++i)
   {
      for (size_t b = 0; b < batch; ++b)
      {
         fn();
         clobber_memory();
      }
   }
   auto ns =
       std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count();
   return static_cast<double>(ns) / (static_cast<double>(target) * static_cast<double>(batch));
}

// ── Runner: report pack/unpack ns and byte size for f32 & f16 ─────────────

template <typename T>
void compare(const char* label, const T& value)
{
   auto f32 = psio1::to_frac(value);
   auto f16 = psio1::to_frac16(value);

   double pack32 = bench_ns([&] {
      auto r = psio1::to_frac(value);
      do_not_optimize(r.data());
   });
   double pack16 = bench_ns([&] {
      auto r = psio1::to_frac16(value);
      do_not_optimize(r.data());
   });

   double unpack32 = bench_ns([&] {
      T out;
      (void)psio1::from_frac(out, f32);
      do_not_optimize(out);
   });
   double unpack16 = bench_ns([&] {
      T out;
      (void)psio1::from_frac16(out, f16);
      do_not_optimize(out);
   });

   size_t s32 = f32.size(), s16 = f16.size();
   double size_ratio = static_cast<double>(s16) / static_cast<double>(s32);
   double pack_ratio = pack16 / pack32;
   double unpk_ratio = unpack16 / unpack32;

   std::printf("  %-16s  f32 %4zuB  f16 %4zuB  (%4.2fx)   pack f32 %6.0fns  f16 %6.0fns (%4.2fx)"
               "   unpack f32 %6.0fns  f16 %6.0fns (%4.2fx)\n",
               label, s32, s16, size_ratio, pack32, pack16, pack_ratio, unpack32, unpack16,
               unpk_ratio);
}

int main()
{
   std::printf("\n=== Fracpack f32 vs f16 size & speed comparison ===\n\n");
   std::printf("  %-16s  %-18s %-18s   %-34s   %-34s\n", "Type", "size f32", "size f16",
               "pack (f32 / f16 / ratio)", "unpack (f32 / f16 / ratio)");
   std::printf("  %s\n", std::string(170, '-').c_str());

   compare("BPoint", make_point());
   compare("Token", make_token());
   compare("UserProfile", make_user());
   compare("LineItem", make_line_item(0));
   compare("Order", make_order());
   compare("SensorReading", make_sensor());

   std::printf("\nNotes:\n");
   std::printf("  - frac_format_32: u32 offsets/sizes (existing default)\n");
   std::printf("  - frac_format_16: u16 offsets/sizes (new, records must fit in 64KB)\n");
   std::printf("  - Fixed-size structs (definitionWillNotChange) are format-independent.\n");
   std::printf("  - Savings come from u32→u16 replacement of string/vector sizes,\n");
   std::printf("    offset fields, option tombstones, and struct fixed-size prefixes.\n");
   return 0;
}
