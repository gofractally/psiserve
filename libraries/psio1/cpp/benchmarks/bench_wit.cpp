// bench_wit.cpp — Benchmark for psio Canonical ABI (WIT) format
//
// Same schemas and data as bench_fracpack.cpp.
// Measures: wit::pack, wit::unpack, view<T,wit> field access, wit::validate,
//           and wire size.
//
// Build:
//   cmake -B build/Release -G Ninja -DCMAKE_BUILD_TYPE=Release \
//         -DPSIO_ENABLE_BENCHMARKS=ON
//   cmake --build build/Release --target psio_bench_wit
// Run:
//   ./build/Release/bin/psio_bench_wit

#include <psio1/wit_view.hpp>
#include <psio1/reflect.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cinttypes>
#include <optional>
#include <string>
#include <vector>

// ── Prevent dead-code elimination ────────────────────────────────────────────

template <typename T>
inline void do_not_optimize(T const& val)
{
   asm volatile("" : : "r,m"(val) : "memory");
}
inline void clobber_memory()
{
   asm volatile("" ::: "memory");
}

// ── Benchmark types (same as bench_fracpack.cpp) ─────────────────────────────

struct BPoint
{
   double x;
   double y;
};
PSIO1_REFLECT(BPoint, x, y)

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
   uint64_t                       id;
   std::string                    name;
   std::string                    email;
   std::optional<std::string>     bio;
   uint32_t                       age;
   double                         score;
   std::vector<std::string>       tags;
   bool                           verified;
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
   uint64_t                   timestamp;
   std::string                device_id;
   double                     temp;
   double                     humidity;
   double                     pressure;
   double                     accel_x;
   double                     accel_y;
   double                     accel_z;
   double                     gyro_x;
   double                     gyro_y;
   double                     gyro_z;
   double                     mag_x;
   double                     mag_y;
   double                     mag_z;
   float                      battery;
   int16_t                    signal_dbm;
   std::optional<uint32_t>    error_code;
   std::string                firmware;
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

// ── Test data factories ──────────────────────────────────────────────────────

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
       1700000000000ULL,
       "sensor-alpha-42",
       23.5,
       65.2,
       1013.25,
       0.01,
       -0.02,
       9.81,
       0.001,
       -0.003,
       0.002,
       25.1,
       -12.3,
       42.7,
       3.7f,
       -65,
       std::nullopt,
       "v2.3.1-rc4",
   };
}

// ── Benchmark harness (same as bench_fracpack.cpp) ───────────────────────────

struct BenchResult
{
   std::string name;
   uint64_t    ops_per_sec;
   double      mean_ns;
   size_t      bytes;
   size_t      iterations;
};

static std::vector<BenchResult> g_results;

template <typename Fn>
void bench(const char* name, size_t data_bytes, Fn fn)
{
   using clock = std::chrono::high_resolution_clock;

   for (int i = 0; i < 200; ++i)
   {
      fn();
      clobber_memory();
   }

   size_t cal_iters = 0;
   auto   cal_start = clock::now();
   while (std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - cal_start)
              .count() < 30'000)
   {
      fn();
      clobber_memory();
      ++cal_iters;
   }
   auto cal_elapsed_us =
       std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - cal_start).count();

   double ns_per_op = cal_iters > 0 ? (cal_elapsed_us * 1000.0 / cal_iters) : 1.0;

   size_t batch = 1;
   if (ns_per_op < 100.0)
      batch = std::max<size_t>(1, static_cast<size_t>(100.0 / std::max(ns_per_op, 0.01)));

   size_t target = std::max<size_t>(1000,
       static_cast<size_t>(200'000'000.0 / (ns_per_op * static_cast<double>(batch))));

   auto start = clock::now();
   for (size_t i = 0; i < target; ++i)
   {
      for (size_t b = 0; b < batch; ++b)
      {
         fn();
         clobber_memory();
      }
   }
   auto elapsed_ns =
       std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count();

   double total_ops   = static_cast<double>(target) * static_cast<double>(batch);
   double mean_ns_f   = static_cast<double>(elapsed_ns) / total_ops;
   uint64_t ops_per_sec = mean_ns_f > 0.001
                              ? static_cast<uint64_t>(1'000'000'000.0 / mean_ns_f)
                              : 0;

   g_results.push_back({name, ops_per_sec, mean_ns_f,
                         data_bytes, static_cast<size_t>(total_ops)});

   if (mean_ns_f < 10.0)
      std::printf("  %-45s %12" PRIu64 " ops/s  %6.1f ns  %6zu B  (%.0f ops)\n",
                  name, ops_per_sec, mean_ns_f, data_bytes, total_ops);
   else
      std::printf("  %-45s %12" PRIu64 " ops/s  %6.0f ns  %6zu B  (%.0f ops)\n",
                  name, ops_per_sec, mean_ns_f, data_bytes, total_ops);
}

void print_header(const char* group)
{
   std::printf("\n=== %s ===\n", group);
   std::printf("  %-45s %12s  %8s  %6s\n", "Benchmark", "Throughput", "Latency", "Bytes");
   std::printf("  %s\n",
               std::string(45 + 12 + 8 + 6 + 10, '-').c_str());
}

// ── Wire size reporting ──────────────────────────────────────────────────────

void report_wire_sizes()
{
   auto point  = make_point();
   auto token  = make_token();
   auto user   = make_user();
   auto order  = make_order();
   auto sensor = make_sensor();

   auto point_buf  = psio1::wit::pack(point);
   auto token_buf  = psio1::wit::pack(token);
   auto user_buf   = psio1::wit::pack(user);
   auto order_buf  = psio1::wit::pack(order);
   auto sensor_buf = psio1::wit::pack(sensor);

   std::printf("=== Wire sizes (Canonical ABI / WIT) ===\n");
   std::printf("  %-20s %6zu bytes\n", "BPoint",        point_buf.size());
   std::printf("  %-20s %6zu bytes\n", "Token",         token_buf.size());
   std::printf("  %-20s %6zu bytes\n", "UserProfile",   user_buf.size());
   std::printf("  %-20s %6zu bytes\n", "Order",         order_buf.size());
   std::printf("  %-20s %6zu bytes\n", "SensorReading", sensor_buf.size());
   std::printf("\n");
}

// ── Pack benchmarks ──────────────────────────────────────────────────────────

void bench_pack()
{
   print_header("Pack (value -> bytes)");

   auto point  = make_point();
   auto token  = make_token();
   auto user   = make_user();
   auto order  = make_order();
   auto sensor = make_sensor();

   auto point_buf  = psio1::wit::pack(point);
   auto token_buf  = psio1::wit::pack(token);
   auto user_buf   = psio1::wit::pack(user);
   auto order_buf  = psio1::wit::pack(order);
   auto sensor_buf = psio1::wit::pack(sensor);

   bench("pack/BPoint", point_buf.size(), [&] {
      auto r = psio1::wit::pack(point);
      do_not_optimize(r);
   });

   bench("pack/Token", token_buf.size(), [&] {
      auto r = psio1::wit::pack(token);
      do_not_optimize(r);
   });

   bench("pack/UserProfile", user_buf.size(), [&] {
      auto r = psio1::wit::pack(user);
      do_not_optimize(r);
   });

   bench("pack/Order", order_buf.size(), [&] {
      auto r = psio1::wit::pack(order);
      do_not_optimize(r);
   });

   bench("pack/SensorReading", sensor_buf.size(), [&] {
      auto r = psio1::wit::pack(sensor);
      do_not_optimize(r);
   });
}

// ── Unpack benchmarks ────────────────────────────────────────────────────────

void bench_unpack()
{
   print_header("Unpack (bytes -> value)");

   auto point_buf  = psio1::wit::pack(make_point());
   auto token_buf  = psio1::wit::pack(make_token());
   auto user_buf   = psio1::wit::pack(make_user());
   auto order_buf  = psio1::wit::pack(make_order());
   auto sensor_buf = psio1::wit::pack(make_sensor());

   bench("unpack/BPoint", point_buf.size(), [&] {
      auto r = psio1::wit::unpack<BPoint>(point_buf);
      do_not_optimize(r);
   });

   bench("unpack/Token", token_buf.size(), [&] {
      auto r = psio1::wit::unpack<Token>(token_buf);
      do_not_optimize(r);
   });

   bench("unpack/UserProfile", user_buf.size(), [&] {
      auto r = psio1::wit::unpack<UserProfile>(user_buf);
      do_not_optimize(r);
   });

   bench("unpack/Order", order_buf.size(), [&] {
      auto r = psio1::wit::unpack<Order>(order_buf);
      do_not_optimize(r);
   });

   bench("unpack/SensorReading", sensor_buf.size(), [&] {
      auto r = psio1::wit::unpack<SensorReading>(sensor_buf);
      do_not_optimize(r);
   });
}

// ── View benchmarks (zero-copy access) ───────────────────────────────────────

void bench_view()
{
   print_header("View (zero-copy read all fields)");

   auto point_buf  = psio1::wit::pack(make_point());
   auto token_buf  = psio1::wit::pack(make_token());
   auto user_buf   = psio1::wit::pack(make_user());
   auto order_buf  = psio1::wit::pack(make_order());
   auto sensor_buf = psio1::wit::pack(make_sensor());

   bench("view-all/BPoint", point_buf.size(), [&] {
      auto v = psio1::view<BPoint, psio1::wit>::from_buffer(point_buf.data());
      do_not_optimize(v.x());
      do_not_optimize(v.y());
   });

   bench("view-all/Token", token_buf.size(), [&] {
      auto v = psio1::view<Token, psio1::wit>::from_buffer(token_buf.data());
      do_not_optimize(v.kind());
      do_not_optimize(v.offset());
      do_not_optimize(v.length());
      do_not_optimize(v.text());
   });

   bench("view-all/UserProfile", user_buf.size(), [&] {
      auto v = psio1::view<UserProfile, psio1::wit>::from_buffer(user_buf.data());
      do_not_optimize(v.id());
      do_not_optimize(v.name());
      do_not_optimize(v.email());
      do_not_optimize(v.bio());
      do_not_optimize(v.age());
      do_not_optimize(v.score());
      auto tags = v.tags();
      do_not_optimize(tags.size());
      for (size_t i = 0; i < tags.size(); ++i)
         do_not_optimize(tags[i]);
      do_not_optimize(v.verified());
   });

   bench("view-all/Order", order_buf.size(), [&] {
      auto v = psio1::view<Order, psio1::wit>::from_buffer(order_buf.data());
      do_not_optimize(v.id());
      auto cust = v.customer();
      do_not_optimize(cust.id());
      do_not_optimize(cust.name());
      do_not_optimize(cust.email());
      do_not_optimize(cust.bio());
      do_not_optimize(cust.age());
      do_not_optimize(cust.score());
      do_not_optimize(cust.verified());
      auto items = v.items();
      for (size_t i = 0; i < items.size(); ++i) {
         auto item = items[i];
         do_not_optimize(item.product());
         do_not_optimize(item.qty());
         do_not_optimize(item.unit_price());
      }
      do_not_optimize(v.total());
      do_not_optimize(v.note());
   });

   bench("view-all/SensorReading", sensor_buf.size(), [&] {
      auto v = psio1::view<SensorReading, psio1::wit>::from_buffer(sensor_buf.data());
      do_not_optimize(v.timestamp());
      do_not_optimize(v.device_id());
      do_not_optimize(v.temp());
      do_not_optimize(v.humidity());
      do_not_optimize(v.pressure());
      do_not_optimize(v.accel_x());
      do_not_optimize(v.accel_y());
      do_not_optimize(v.accel_z());
      do_not_optimize(v.gyro_x());
      do_not_optimize(v.gyro_y());
      do_not_optimize(v.gyro_z());
      do_not_optimize(v.mag_x());
      do_not_optimize(v.mag_y());
      do_not_optimize(v.mag_z());
      do_not_optimize(v.battery());
      do_not_optimize(v.signal_dbm());
      do_not_optimize(v.error_code());
      do_not_optimize(v.firmware());
   });
}

// ── View-one benchmarks (single field access) ────────────────────────────────

void bench_view_one()
{
   print_header("View-one (single field zero-copy read)");

   auto user_buf   = psio1::wit::pack(make_user());
   auto order_buf  = psio1::wit::pack(make_order());
   auto sensor_buf = psio1::wit::pack(make_sensor());

   bench("view-one/UserProfile.name", user_buf.size(), [&] {
      auto v = psio1::view<UserProfile, psio1::wit>::from_buffer(user_buf.data());
      do_not_optimize(v.name());
   });

   bench("view-one/UserProfile.score", user_buf.size(), [&] {
      auto v = psio1::view<UserProfile, psio1::wit>::from_buffer(user_buf.data());
      do_not_optimize(v.score());
   });

   bench("view-one/Order.total", order_buf.size(), [&] {
      auto v = psio1::view<Order, psio1::wit>::from_buffer(order_buf.data());
      do_not_optimize(v.total());
   });

   bench("view-one/SensorReading.temp", sensor_buf.size(), [&] {
      auto v = psio1::view<SensorReading, psio1::wit>::from_buffer(sensor_buf.data());
      do_not_optimize(v.temp());
   });
}

// ── Validate benchmarks ──────────────────────────────────────────────────────

void bench_validate()
{
   print_header("Validate (bounds + alignment check)");

   auto point_buf  = psio1::wit::pack(make_point());
   auto token_buf  = psio1::wit::pack(make_token());
   auto user_buf   = psio1::wit::pack(make_user());
   auto order_buf  = psio1::wit::pack(make_order());
   auto sensor_buf = psio1::wit::pack(make_sensor());

   bench("validate/BPoint", point_buf.size(), [&] {
      bool ok = psio1::wit::validate<BPoint>(point_buf);
      do_not_optimize(ok);
   });

   bench("validate/Token", token_buf.size(), [&] {
      bool ok = psio1::wit::validate<Token>(token_buf);
      do_not_optimize(ok);
   });

   bench("validate/UserProfile", user_buf.size(), [&] {
      bool ok = psio1::wit::validate<UserProfile>(user_buf);
      do_not_optimize(ok);
   });

   bench("validate/Order", order_buf.size(), [&] {
      bool ok = psio1::wit::validate<Order>(order_buf);
      do_not_optimize(ok);
   });

   bench("validate/SensorReading", sensor_buf.size(), [&] {
      bool ok = psio1::wit::validate<SensorReading>(sensor_buf);
      do_not_optimize(ok);
   });
}

// ── Modify benchmarks (in-place scalar writes) ──────────────────────────────

void bench_modify()
{
   print_header("Modify (in-place scalar write)");

   auto user_buf   = psio1::wit::pack(make_user());
   auto sensor_buf = psio1::wit::pack(make_sensor());

   bench("modify/UserProfile.age", user_buf.size(), [&] {
      auto m = psio1::wit_mut<UserProfile>::from_buffer(user_buf);
      m.age() = 33;
      clobber_memory();
   });

   bench("modify/UserProfile.score", user_buf.size(), [&] {
      auto m = psio1::wit_mut<UserProfile>::from_buffer(user_buf);
      m.score() = 99.5;
      clobber_memory();
   });

   bench("modify/SensorReading.temp", sensor_buf.size(), [&] {
      auto m = psio1::wit_mut<SensorReading>::from_buffer(sensor_buf);
      m.temp() = 24.5;
      clobber_memory();
   });
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main()
{
   std::printf("Canonical ABI (WIT) Serialization Benchmarks\n");
   std::printf("=============================================\n");

   report_wire_sizes();
   bench_pack();
   bench_unpack();
   bench_view();
   bench_view_one();
   bench_validate();
   bench_modify();

   // ── Summary table ─────────────────────────────────────────────────────────
   std::printf("\n=== Summary ===\n");
   std::printf("  %-45s %12s  %8s  %6s\n", "Benchmark", "Throughput", "Latency", "Bytes");
   std::printf("  %s\n",
               std::string(45 + 12 + 8 + 6 + 10, '-').c_str());
   for (auto& r : g_results)
   {
      if (r.mean_ns < 10.0)
         std::printf("  %-45s %12" PRIu64 " ops/s  %6.1f ns  %6zu B\n",
                     r.name.c_str(), r.ops_per_sec, r.mean_ns, r.bytes);
      else
         std::printf("  %-45s %12" PRIu64 " ops/s  %6.0f ns  %6zu B\n",
                     r.name.c_str(), r.ops_per_sec, r.mean_ns, r.bytes);
   }

   return 0;
}
