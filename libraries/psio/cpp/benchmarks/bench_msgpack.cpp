// MessagePack benchmark — same schemas and data as bench_fracpack.cpp
//
// Measures: pack (serialize), unpack (deserialize to native structs),
//           and wire size.
//
// MessagePack is schema-less (like fracpack) — no code generation required.
// Uses MSGPACK_DEFINE to add serialization to plain C++ structs.
// No zero-copy view is possible — must fully deserialize to access fields.

#include <type_traits>  // must precede msgpack.hpp for LLVM 22+ libc++
#include <msgpack.hpp>

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

// ── Data types (plain structs with MSGPACK_DEFINE) ──────────────────────────

struct MPoint
{
   double x;
   double y;
   MSGPACK_DEFINE(x, y)
};

struct MToken
{
   uint16_t    kind;
   uint32_t    offset;
   uint32_t    length;
   std::string text;
   MSGPACK_DEFINE(kind, offset, length, text)
};

struct MUserProfile
{
   uint64_t                 id;
   std::string              name;
   std::string              email;
   std::string              bio;
   uint32_t                 age;
   double                   score;
   std::vector<std::string> tags;
   bool                     verified;
   MSGPACK_DEFINE(id, name, email, bio, age, score, tags, verified)
};

struct MLineItem
{
   std::string product;
   uint32_t    qty;
   double      unit_price;
   MSGPACK_DEFINE(product, qty, unit_price)
};

struct MOrder
{
   uint64_t                id;
   MUserProfile            customer;
   std::vector<MLineItem>  items;
   double                  total;
   std::string             note;
   MSGPACK_DEFINE(id, customer, items, total, note)
};

struct MSensorReading
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
   MSGPACK_DEFINE(timestamp, device_id, temp, humidity, pressure,
                  accel_x, accel_y, accel_z,
                  gyro_x, gyro_y, gyro_z,
                  mag_x, mag_y, mag_z,
                  battery, signal_dbm, error_code, firmware)
};

// ── Test data factories ─────────────────────────────────────────────────────

MPoint make_point()
{
   return {3.14159265358979, 2.71828182845905};
}

MToken make_token()
{
   return {42, 1024, 15, "identifier_name"};
}

MUserProfile make_user()
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

MOrder make_order()
{
   MOrder o;
   o.id       = 987654321ULL;
   o.customer = make_user();
   for (int i = 0; i < 5; ++i)
      o.items.push_back({"Product-" + std::to_string(i),
                          static_cast<uint32_t>(i + 1), 19.99 + i * 5.0});
   o.total = 199.95;
   o.note  = "Please ship before Friday";
   return o;
}

MSensorReading make_sensor()
{
   return {
       1700000000000ULL, "sensor-alpha-42",
       23.5, 65.2, 1013.25,
       0.01, -0.02, 9.81,
       0.001, -0.003, 0.002,
       25.1, -12.3, 42.7,
       3.7f, -65, 0,
       "v2.3.1-rc4",
   };
}

// ── Pack benchmarks ─────────────────────────────────────────────────────────

void bench_msgpack_pack()
{
   print_header("MessagePack: Pack (serialize)");

   // Print wire sizes
   auto print_wire_size = [](const char* name, auto make_fn) {
      auto obj = make_fn();
      msgpack::sbuffer sbuf;
      msgpack::pack(sbuf, obj);
      std::printf("  %s wire size: %zu B\n", name, sbuf.size());
   };
   print_wire_size("Point", make_point);
   print_wire_size("Token", make_token);
   print_wire_size("UserProfile", make_user);
   print_wire_size("Order", make_order);
   print_wire_size("SensorReading", make_sensor);

   // Pack benchmarks
   auto bench_pack = [](const char* name, auto make_fn) {
      auto obj = make_fn();
      msgpack::sbuffer tmp;
      msgpack::pack(tmp, obj);
      size_t sz = tmp.size();
      std::string bname = std::string("msgpack-pack/") + name;
      bench(bname.c_str(), sz, [&] {
         msgpack::sbuffer sbuf;
         msgpack::pack(sbuf, obj);
         do_not_optimize(sbuf.data());
         return sbuf.size();
      });
   };

   bench_pack("Point", make_point);
   bench_pack("Token", make_token);
   bench_pack("UserProfile", make_user);
   bench_pack("Order", make_order);
   bench_pack("SensorReading", make_sensor);
}

// ── Unpack benchmarks ───────────────────────────────────────────────────────

void bench_msgpack_unpack()
{
   print_header("MessagePack: Unpack (deserialize to native structs)");

   auto bench_unpack = [](const char* name, auto make_fn) {
      using T = decltype(make_fn());
      auto obj = make_fn();
      msgpack::sbuffer sbuf;
      msgpack::pack(sbuf, obj);
      size_t sz = sbuf.size();
      std::string bname = std::string("msgpack-unpack/") + name;
      bench(bname.c_str(), sz, [&] {
         auto handle = msgpack::unpack(sbuf.data(), sbuf.size());
         T result;
         handle.get().convert(result);
         do_not_optimize(result);
         return sz;
      });
   };

   bench_unpack("Point", make_point);
   bench_unpack("Token", make_token);
   bench_unpack("UserProfile", make_user);
   bench_unpack("Order", make_order);
   bench_unpack("SensorReading", make_sensor);
}

// ── View benchmarks ─────────────────────────────────────────────────────────
// MessagePack has no zero-copy view. We measure parse + convert + read all
// fields, comparable to protobuf's view-all.

void bench_msgpack_view()
{
   print_header("MessagePack: View-All (unpack + read all fields)");

   // Point
   {
      auto obj = make_point();
      msgpack::sbuffer sbuf;
      msgpack::pack(sbuf, obj);
      size_t sz = sbuf.size();
      bench("msgpack-view-all/Point", sz, [&] {
         auto handle = msgpack::unpack(sbuf.data(), sbuf.size());
         MPoint p;
         handle.get().convert(p);
         do_not_optimize(p.x);
         do_not_optimize(p.y);
         return p.x + p.y;
      });
   }
   // Token
   {
      auto obj = make_token();
      msgpack::sbuffer sbuf;
      msgpack::pack(sbuf, obj);
      size_t sz = sbuf.size();
      bench("msgpack-view-all/Token", sz, [&] {
         auto handle = msgpack::unpack(sbuf.data(), sbuf.size());
         MToken t;
         handle.get().convert(t);
         do_not_optimize(t.kind);
         do_not_optimize(t.offset);
         do_not_optimize(t.length);
         do_not_optimize(t.text.data());
         return t.kind;
      });
   }
   // UserProfile
   {
      auto obj = make_user();
      msgpack::sbuffer sbuf;
      msgpack::pack(sbuf, obj);
      size_t sz = sbuf.size();
      bench("msgpack-view-all/UserProfile", sz, [&] {
         auto handle = msgpack::unpack(sbuf.data(), sbuf.size());
         MUserProfile u;
         handle.get().convert(u);
         do_not_optimize(u.id);
         do_not_optimize(u.name.data());
         do_not_optimize(u.email.data());
         do_not_optimize(u.bio.data());
         do_not_optimize(u.age);
         do_not_optimize(u.score);
         do_not_optimize(u.tags.size());
         do_not_optimize(u.verified);
         return u.id;
      });
   }
   // Order
   {
      auto obj = make_order();
      msgpack::sbuffer sbuf;
      msgpack::pack(sbuf, obj);
      size_t sz = sbuf.size();
      bench("msgpack-view-all/Order", sz, [&] {
         auto handle = msgpack::unpack(sbuf.data(), sbuf.size());
         MOrder o;
         handle.get().convert(o);
         do_not_optimize(o.id);
         do_not_optimize(o.customer.name.data());
         do_not_optimize(o.items.size());
         do_not_optimize(o.total);
         do_not_optimize(o.note.data());
         return o.id;
      });
   }
   // SensorReading
   {
      auto obj = make_sensor();
      msgpack::sbuffer sbuf;
      msgpack::pack(sbuf, obj);
      size_t sz = sbuf.size();
      bench("msgpack-view-all/SensorReading", sz, [&] {
         auto handle = msgpack::unpack(sbuf.data(), sbuf.size());
         MSensorReading s;
         handle.get().convert(s);
         do_not_optimize(s.timestamp);
         do_not_optimize(s.device_id.data());
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
         do_not_optimize(s.firmware.data());
         return s.timestamp;
      });
   }
}

void bench_msgpack_view_one()
{
   print_header("MessagePack: View-One (unpack + single field read)");

   // UserProfile.name — must unpack entire message to read one field
   {
      auto obj = make_user();
      msgpack::sbuffer sbuf;
      msgpack::pack(sbuf, obj);
      size_t sz = sbuf.size();
      bench("msgpack-view-one/UserProfile.name", sz, [&] {
         auto handle = msgpack::unpack(sbuf.data(), sbuf.size());
         MUserProfile u;
         handle.get().convert(u);
         do_not_optimize(u.name.data());
         return u.name.size();
      });
   }
   // UserProfile.id
   {
      auto obj = make_user();
      msgpack::sbuffer sbuf;
      msgpack::pack(sbuf, obj);
      size_t sz = sbuf.size();
      bench("msgpack-view-one/UserProfile.id", sz, [&] {
         auto handle = msgpack::unpack(sbuf.data(), sbuf.size());
         MUserProfile u;
         handle.get().convert(u);
         do_not_optimize(u.id);
         return u.id;
      });
   }
   // Order.customer.name
   {
      auto obj = make_order();
      msgpack::sbuffer sbuf;
      msgpack::pack(sbuf, obj);
      size_t sz = sbuf.size();
      bench("msgpack-view-one/Order.customer.name", sz, [&] {
         auto handle = msgpack::unpack(sbuf.data(), sbuf.size());
         MOrder o;
         handle.get().convert(o);
         do_not_optimize(o.customer.name.data());
         return o.customer.name.size();
      });
   }
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main()
{
   std::printf("MessagePack Benchmark (comparison with psio formats)\n");
   std::printf("=====================================================\n");

   bench_msgpack_pack();
   bench_msgpack_unpack();
   bench_msgpack_view();
   bench_msgpack_view_one();

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
