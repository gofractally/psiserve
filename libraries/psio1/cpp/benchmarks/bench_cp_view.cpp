// bench_cp_view.cpp — Benchmark for psio view<T, cp> (our capnp implementation)
//
// Same schemas and data as bench_fracpack.cpp and bench_capnp.cpp.
// Measures: capnp_pack, capnp_unpack, view<T,cp> field access, validate_capnp,
//           and wire size.
//
// Build:
//   cmake -B build/Release -G Ninja -DCMAKE_BUILD_TYPE=Release \
//         -DPSIO_ENABLE_BENCHMARKS=ON
//   cmake --build build/Release --target psio_bench_cp
// Run:
//   ./build/Release/bin/psio_bench_cp

#include <psio1/fracpack.hpp>
#include <psio1/capnp_view.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cinttypes>
#include <string>
#include <vector>

// ── Prevent dead-code elimination ────────────────────────────────────────────
// Uses output constraint "+r,m" (matching Google Benchmark's DoNotOptimize)
// to force the compiler to both produce the value AND assume it may be
// modified, preventing dead-code elimination even for trivial types.

template <typename T>
inline void do_not_optimize(T& val)
{
   asm volatile("" : "+r,m"(val) : : "memory");
}
template <typename T>
inline void do_not_optimize(T const& val)
{
   // For const refs (temporaries), use input constraint — the value must
   // be computed but we can't use output constraint on const.
   asm volatile("" : : "r,m"(val) : "memory");
}
inline void clobber_memory()
{
   asm volatile("" ::: "memory");
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
      std::printf("  %-50s %12" PRIu64 " ops/s  %6.1f ns  %6zu B  (%.0f ops)\n",
                  name, ops_per_sec, mean_ns_f, data_bytes, total_ops);
   else
      std::printf("  %-50s %12" PRIu64 " ops/s  %6.0f ns  %6zu B  (%.0f ops)\n",
                  name, ops_per_sec, mean_ns_f, data_bytes, total_ops);
}

void print_header(const char* group)
{
   std::printf("\n=== %s ===\n", group);
   std::printf("  %-50s %12s  %8s  %6s\n", "Benchmark", "Throughput", "Latency", "Bytes");
   std::printf("  %s\n", std::string(50 + 12 + 8 + 6 + 10, '-').c_str());
}

// ── Benchmark types (same schema as bench_fracpack.cpp & bench_capnp.cpp) ────
//
// These match the .capnp schema in bench_schemas.capnp exactly:
// sequential ordinals, same field types.

struct BPoint
{
   double x;
   double y;
};
PSIO1_REFLECT(BPoint, definitionWillNotChange(), x, y)

struct BToken
{
   uint16_t    kind;
   uint32_t    offset;
   uint32_t    length;
   std::string text;
};
PSIO1_REFLECT(BToken, kind, offset, length, text)

struct BUserProfile
{
   uint64_t                 id;
   std::string              name;
   std::string              email;
   std::string              bio;
   uint32_t                 age;
   double                   score;
   std::vector<std::string> tags;
   bool                     verified;
};
PSIO1_REFLECT(BUserProfile, id, name, email, bio, age, score, tags, verified)

struct BLineItem
{
   std::string product;
   uint32_t    qty;
   double      unit_price;
};
PSIO1_REFLECT(BLineItem, product, qty, unit_price)

struct BOrder
{
   uint64_t                  id;
   BUserProfile              customer;
   std::vector<BLineItem>    items;
   double                    total;
   std::string               note;
};
PSIO1_REFLECT(BOrder, id, customer, items, total, note)

struct BSensorReading
{
   uint64_t    timestamp;
   std::string device_id;
   double      temp;
   double      humidity;
   double      pressure;
   double      accel_x;
   double      accel_y;
   double      accel_z;
   double      gyro_x;
   double      gyro_y;
   double      gyro_z;
   double      mag_x;
   double      mag_y;
   double      mag_z;
   float       battery;
   int16_t     signal_dbm;
   uint32_t    error_code;
   std::string firmware;
};
PSIO1_REFLECT(BSensorReading, timestamp, device_id, temp, humidity, pressure,
             accel_x, accel_y, accel_z, gyro_x, gyro_y, gyro_z,
             mag_x, mag_y, mag_z, battery, signal_dbm, error_code, firmware)

// ── Test data factories ──────────────────────────────────────────────────────

BPoint make_point()
{
   return {3.14159265358979, 2.71828182845905};
}

BToken make_token()
{
   return {42, 1024, 15, "identifier_name"};
}

BUserProfile make_user()
{
   return {
       123456789ULL,
       "Alice Johnson",
       "alice@example.com",
       "Software engineer interested in distributed systems and WebAssembly",
       32,
       98.5,
       {"developer", "wasm", "c++", "open-source"},
       true,
   };
}

BLineItem make_line_item(int i)
{
   return {
       "Product-" + std::to_string(i),
       static_cast<uint32_t>(i + 1),
       19.99 + i * 5.0,
   };
}

BOrder make_order()
{
   std::vector<BLineItem> items;
   for (int i = 0; i < 5; ++i)
      items.push_back(make_line_item(i));
   return {
       987654321ULL,
       make_user(),
       std::move(items),
       199.95,
       "Please ship before Friday",
   };
}

BSensorReading make_sensor()
{
   return {
       1700000000000ULL,
       "sensor-alpha-42",
       23.5,  65.2,  1013.25,
       0.01, -0.02,  9.81,
       0.001, -0.003, 0.002,
       25.1, -12.3,  42.7,
       3.7f,
       -65,
       0,
       "v2.3.1-rc4",
   };
}

// ── Pack benchmarks ──────────────────────────────────────────────────────────

void bench_pack()
{
   print_header("view<T,cp>: Pack (native struct -> capnp wire bytes)");

   auto pt    = make_point();
   auto tok   = make_token();
   auto user  = make_user();
   auto order = make_order();
   auto sens  = make_sensor();

   // Wire sizes
   auto pt_buf    = psio1::capnp_pack(pt);
   auto tok_buf   = psio1::capnp_pack(tok);
   auto user_buf  = psio1::capnp_pack(user);
   auto order_buf = psio1::capnp_pack(order);
   auto sens_buf  = psio1::capnp_pack(sens);

   std::printf("  Point wire size: %zu B\n", pt_buf.size());
   std::printf("  Token wire size: %zu B\n", tok_buf.size());
   std::printf("  UserProfile wire size: %zu B\n", user_buf.size());
   std::printf("  Order wire size: %zu B\n", order_buf.size());
   std::printf("  SensorReading wire size: %zu B\n\n", sens_buf.size());

   bench("cp-pack/Point", pt_buf.size(), [&] {
      auto buf = psio1::capnp_pack(pt);
      do_not_optimize(buf.data());
   });
   bench("cp-pack/Token", tok_buf.size(), [&] {
      auto buf = psio1::capnp_pack(tok);
      do_not_optimize(buf.data());
   });
   bench("cp-pack/UserProfile", user_buf.size(), [&] {
      auto buf = psio1::capnp_pack(user);
      do_not_optimize(buf.data());
   });
   bench("cp-pack/Order", order_buf.size(), [&] {
      auto buf = psio1::capnp_pack(order);
      do_not_optimize(buf.data());
   });
   bench("cp-pack/SensorReading", sens_buf.size(), [&] {
      auto buf = psio1::capnp_pack(sens);
      do_not_optimize(buf.data());
   });
}

// ── Unpack benchmarks ────────────────────────────────────────────────────────

void bench_unpack()
{
   print_header("view<T,cp>: Unpack (capnp wire bytes -> native struct)");

   auto pt_buf    = psio1::capnp_pack(make_point());
   auto tok_buf   = psio1::capnp_pack(make_token());
   auto user_buf  = psio1::capnp_pack(make_user());
   auto order_buf = psio1::capnp_pack(make_order());
   auto sens_buf  = psio1::capnp_pack(make_sensor());

   bench("cp-unpack/Point", pt_buf.size(), [&] {
      auto pt = psio1::capnp_unpack<BPoint>(pt_buf.data());
      do_not_optimize(pt.x);
      do_not_optimize(pt.y);
   });
   bench("cp-unpack/Token", tok_buf.size(), [&] {
      auto tok = psio1::capnp_unpack<BToken>(tok_buf.data());
      do_not_optimize(tok.kind);
      do_not_optimize(tok.offset);
      do_not_optimize(tok.length);
      do_not_optimize(tok.text);
   });
   bench("cp-unpack/UserProfile", user_buf.size(), [&] {
      auto u = psio1::capnp_unpack<BUserProfile>(user_buf.data());
      do_not_optimize(u.id);
      do_not_optimize(u.name);
      do_not_optimize(u.email);
      do_not_optimize(u.bio);
      do_not_optimize(u.age);
      do_not_optimize(u.score);
      do_not_optimize(u.tags);
      do_not_optimize(u.verified);
   });
   bench("cp-unpack/Order", order_buf.size(), [&] {
      auto o = psio1::capnp_unpack<BOrder>(order_buf.data());
      do_not_optimize(o.id);
      do_not_optimize(o.customer.name);
      do_not_optimize(o.customer.email);
      do_not_optimize(o.customer.bio);
      do_not_optimize(o.customer.tags);
      do_not_optimize(o.items);
      do_not_optimize(o.total);
      do_not_optimize(o.note);
   });
   bench("cp-unpack/SensorReading", sens_buf.size(), [&] {
      auto s = psio1::capnp_unpack<BSensorReading>(sens_buf.data());
      do_not_optimize(s.timestamp);
      do_not_optimize(s.device_id);
      do_not_optimize(s.temp);
      do_not_optimize(s.humidity);
      do_not_optimize(s.pressure);
      do_not_optimize(s.accel_x);
      do_not_optimize(s.accel_y);
      do_not_optimize(s.accel_z);
      do_not_optimize(s.gyro_x);
      do_not_optimize(s.gyro_y);
      do_not_optimize(s.gyro_z);
      do_not_optimize(s.mag_x);
      do_not_optimize(s.mag_y);
      do_not_optimize(s.mag_z);
      do_not_optimize(s.battery);
      do_not_optimize(s.signal_dbm);
      do_not_optimize(s.error_code);
      do_not_optimize(s.firmware);
   });
}

// ── View benchmarks (zero-copy field access) ─────────────────────────────────

void bench_view()
{
   print_header("view<T,cp>: View (zero-copy read all fields)");

   auto pt_buf    = psio1::capnp_pack(make_point());
   auto tok_buf   = psio1::capnp_pack(make_token());
   auto user_buf  = psio1::capnp_pack(make_user());
   auto order_buf = psio1::capnp_pack(make_order());
   auto sens_buf  = psio1::capnp_pack(make_sensor());

   bench("cp-view/Point (all fields)", pt_buf.size(), [&] {
      auto v = psio1::capnp_view<BPoint>::from_buffer(pt_buf.data());
      do_not_optimize(v.x());
      do_not_optimize(v.y());
   });

   bench("cp-view/Token (all fields)", tok_buf.size(), [&] {
      auto v = psio1::capnp_view<BToken>::from_buffer(tok_buf.data());
      do_not_optimize(v.kind());
      do_not_optimize(v.offset());
      do_not_optimize(v.length());
      auto t = v.text();
      do_not_optimize(t.data());
   });

   bench("cp-view/UserProfile (all fields)", user_buf.size(), [&] {
      auto v = psio1::capnp_view<BUserProfile>::from_buffer(user_buf.data());
      do_not_optimize(v.id());
      auto n = v.name(); do_not_optimize(n.data());
      auto e = v.email(); do_not_optimize(e.data());
      auto b = v.bio(); do_not_optimize(b.data());
      do_not_optimize(v.age());
      do_not_optimize(v.score());
      auto tg = v.tags(); do_not_optimize(tg.size());
      do_not_optimize(v.verified());
   });

   bench("cp-view/Order (all fields + nested)", order_buf.size(), [&] {
      auto v = psio1::capnp_view<BOrder>::from_buffer(order_buf.data());
      do_not_optimize(v.id());
      auto c = v.customer();
      auto cn = c.name(); do_not_optimize(cn.data());
      auto items = v.items();
      do_not_optimize(items.size());
      do_not_optimize(v.total());
      auto n = v.note(); do_not_optimize(n.data());
   });

   bench("cp-view/SensorReading (all fields)", sens_buf.size(), [&] {
      auto v = psio1::capnp_view<BSensorReading>::from_buffer(sens_buf.data());
      do_not_optimize(v.timestamp());
      auto d = v.device_id(); do_not_optimize(d.data());
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
      auto fw = v.firmware(); do_not_optimize(fw.data());
   });
}

// ── View single-field benchmarks (comparable to bench_capnp view-one) ────────

void bench_view_one()
{
   print_header("view<T,cp>: View-One (single field from serialized)");

   auto user_buf  = psio1::capnp_pack(make_user());
   auto order_buf = psio1::capnp_pack(make_order());

   bench("cp-view-one/UserProfile.name", user_buf.size(), [&] {
      auto v = psio1::capnp_view<BUserProfile>::from_buffer(user_buf.data());
      auto n = v.name();
      do_not_optimize(n.data());
   });

   bench("cp-view-one/UserProfile.id", user_buf.size(), [&] {
      auto v = psio1::capnp_view<BUserProfile>::from_buffer(user_buf.data());
      auto id = v.id();
      do_not_optimize(id);
   });

   bench("cp-view-one/Order.customer.name", order_buf.size(), [&] {
      auto v = psio1::capnp_view<BOrder>::from_buffer(order_buf.data());
      auto n = v.customer().name();
      do_not_optimize(n.data());
   });
}

// ── Validate benchmarks ──────────────────────────────────────────────────────

void bench_validate()
{
   print_header("view<T,cp>: Validate (bounds-check all pointers)");

   auto pt_buf    = psio1::capnp_pack(make_point());
   auto tok_buf   = psio1::capnp_pack(make_token());
   auto user_buf  = psio1::capnp_pack(make_user());
   auto order_buf = psio1::capnp_pack(make_order());
   auto sens_buf  = psio1::capnp_pack(make_sensor());

   bench("cp-validate/Point", pt_buf.size(), [&] {
      bool ok = psio1::validate_capnp(pt_buf.data(), pt_buf.size());
      do_not_optimize(ok);
   });
   bench("cp-validate/Token", tok_buf.size(), [&] {
      bool ok = psio1::validate_capnp(tok_buf.data(), tok_buf.size());
      do_not_optimize(ok);
   });
   bench("cp-validate/UserProfile", user_buf.size(), [&] {
      bool ok = psio1::validate_capnp(user_buf.data(), user_buf.size());
      do_not_optimize(ok);
   });
   bench("cp-validate/Order", order_buf.size(), [&] {
      bool ok = psio1::validate_capnp(order_buf.data(), order_buf.size());
      do_not_optimize(ok);
   });
   bench("cp-validate/SensorReading", sens_buf.size(), [&] {
      bool ok = psio1::validate_capnp(sens_buf.data(), sens_buf.size());
      do_not_optimize(ok);
   });
}

// ── Mutation benchmarks ──────────────────────────────────────────────────────

void bench_mutate()
{
   print_header("Mutate (capnp_ref — in-place field mutation)");

   // --- Point: mutate 2 scalars ---
   auto pt_data = psio1::capnp_pack(make_point());
   bench("cp-mutate-scalar/Point", pt_data.size(),
         [&]
         {
            psio1::capnp_ref<BPoint> ref(pt_data);
            auto                    f = ref.fields();
            f.x() = 99.0;
            f.y() = -1.0;
            do_not_optimize(ref.data());
            pt_data = std::move(ref.buffer());
         });

   // --- Token: mutate scalar + string ---
   auto tok_data = psio1::capnp_pack(make_token());
   bench("cp-mutate-scalar+string/Token", tok_data.size(),
         [&]
         {
            psio1::capnp_ref<BToken> ref(tok_data);
            auto                    f = ref.fields();
            f.kind()   = uint16_t(99);
            f.offset() = uint32_t(2048);
            f.text()   = "mutated_identifier";
            do_not_optimize(ref.data());
            // Reset to original size to avoid unbounded growth
            tok_data = psio1::capnp_pack(make_token());
         });

   // --- UserProfile: mutate nested string + scalar ---
   auto user_data = psio1::capnp_pack(make_user());
   bench("cp-mutate-string/UserProfile", user_data.size(),
         [&]
         {
            psio1::capnp_ref<BUserProfile> ref(user_data);
            auto                          f = ref.fields();
            f.name()  = "Bob Smith";
            f.score() = 77.7;
            do_not_optimize(ref.data());
            user_data = psio1::capnp_pack(make_user());
         });

   // --- Point: mutate scalar only (no growth, pure in-place) ---
   auto pt2 = psio1::capnp_pack(make_point());
   bench("cp-mutate-scalar-only/Point", pt2.size(),
         [&]
         {
            psio1::capnp_ref<BPoint> ref(pt2);
            auto                    f = ref.fields();
            f.x() = 1.0;
            f.y() = 2.0;
            do_not_optimize(ref.data());
            pt2 = std::move(ref.buffer());
         });

   // --- SensorReading: mutate many scalars (13 fields) ---
   auto sens_data = psio1::capnp_pack(make_sensor());
   bench("cp-mutate-many-scalars/Sensor", sens_data.size(),
         [&]
         {
            psio1::capnp_ref<BSensorReading> ref(sens_data);
            auto                            f = ref.fields();
            f.temp()     = 25.0;
            f.humidity() = 60.0;
            f.pressure() = 1013.0;
            f.accel_x()  = 0.1;
            f.accel_y()  = 0.2;
            f.accel_z()  = 9.8;
            f.battery()  = 3.7f;
            f.signal_dbm() = int16_t(-50);
            f.error_code() = uint32_t(0);
            do_not_optimize(ref.data());
            sens_data = std::move(ref.buffer());
         });
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main()
{
   std::printf("view<T, cp> Benchmark (psio capnp implementation)\n");
   std::printf("==================================================\n");

   bench_pack();
   bench_unpack();
   bench_view();
   bench_view_one();
   bench_validate();
   bench_mutate();

   // Summary
   std::printf("\n=== Summary ===\n");
   std::printf("  %-50s %12s  %8s  %6s\n", "Benchmark", "Throughput", "Latency", "Bytes");
   std::printf("  %s\n", std::string(50 + 12 + 8 + 6 + 10, '-').c_str());
   for (auto& r : g_results)
   {
      if (r.mean_ns < 10.0)
         std::printf("  %-50s %12" PRIu64 " ops/s  %6.1f ns  %6zu B\n", r.name.c_str(),
                     r.ops_per_sec, r.mean_ns, r.bytes);
      else
         std::printf("  %-50s %12" PRIu64 " ops/s  %6.0f ns  %6zu B\n", r.name.c_str(),
                     r.ops_per_sec, r.mean_ns, r.bytes);
   }

   return 0;
}
