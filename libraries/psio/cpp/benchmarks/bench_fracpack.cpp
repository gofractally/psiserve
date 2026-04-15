// Fracpack serialization benchmarks
//
// Covers: pack, unpack, frac_ref view (zero-copy), validate,
//         JSON write/read, mutation (in-place vs repack), and array scaling.
//
// Build:
//   cmake -B build/Release -G Ninja -DCMAKE_BUILD_TYPE=Release \
//         -DPSIO_ENABLE_BENCHMARKS=ON
//   cmake --build build/Release --target psio_bench
// Run:
//   ./build/Release/bin/psio_bench

#include <psio/fracpack.hpp>
#include <psio/frac_ref.hpp>
#include <psio/from_json.hpp>
#include <psio/from_bin.hpp>
#include <psio/from_bincode.hpp>
#include <psio/from_avro.hpp>
#include <psio/reflect.hpp>
#include <psio/to_json.hpp>
#include <psio/to_bin.hpp>
#include <psio/to_bincode.hpp>
#include <psio/to_avro.hpp>
#include <psio/to_key.hpp>
#include <psio/from_key.hpp>
#include <psio/schema.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <cinttypes>
#include <optional>
#include <string>
#include <vector>

// ── Prevent dead-code elimination ────────────────────────────────────────────
//
// do_not_optimize(x): tells the compiler x is consumed (prevents DCE).
// escape(p):          tells the compiler *p may be read/written by opaque code,
//                     preventing the optimizer from caching values loaded from
//                     the pointed-to memory across calls.  Use on input buffers
//                     before the operation under test so the compiler cannot
//                     hoist loads or fold repeated reads.
// clobber_memory():   tells the compiler all memory may have changed.

template <typename T>
inline void do_not_optimize(T const& val)
{
   asm volatile("" : : "r,m"(val) : "memory");
}
template <typename T>
inline void escape(T* p)
{
   asm volatile("" : : "g"(p) : "memory");
}
inline void clobber_memory()
{
   asm volatile("" ::: "memory");
}

// ── Benchmark types ──────────────────────────────────────────────────────────

// Tier 1: Micro (fixed-size, no heap)
struct BPoint
{
   double x;
   double y;
};
PSIO_REFLECT(BPoint, definitionWillNotChange(), x, y)

inline bool operator==(const BPoint& a, const BPoint& b)
{
   return a.x == b.x && a.y == b.y;
}

// Tier 2: Small (1 variable field)
struct Token
{
   uint16_t    kind;
   uint32_t    offset;
   uint32_t    length;
   std::string text;
};
PSIO_REFLECT(Token, kind, offset, length, text)

// Tier 3: Medium (many fields, mixed types)
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
PSIO_REFLECT(UserProfile, id, name, email, bio, age, score, tags, verified)

// Tier 4: Nested
struct LineItem
{
   std::string product;
   uint32_t    qty;
   double      unit_price;
};
PSIO_REFLECT(LineItem, product, qty, unit_price)

struct Order
{
   uint64_t                   id;
   UserProfile                customer;
   std::vector<LineItem>      items;
   double                     total;
   std::optional<std::string> note;
};
PSIO_REFLECT(Order, id, customer, items, total, note)

// Tier 5: Wide (many numeric fields)
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
PSIO_REFLECT(SensorReading,
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

// Tier 7: Deep nesting (recursive tree)
struct TreeNode
{
   uint32_t               value;
   std::string            label;
   std::vector<TreeNode>  children;
};
PSIO_REFLECT(TreeNode, value, label, children)

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

TreeNode make_tree(int depth)
{
   if (depth <= 0)
      return {0, "leaf", {}};
   return {
       static_cast<uint32_t>(depth),
       "node-" + std::to_string(depth),
       {make_tree(depth - 1), make_tree(depth - 1)},
   };
}

std::vector<BPoint> make_point_vec(size_t n)
{
   std::vector<BPoint> v;
   v.reserve(n);
   for (size_t i = 0; i < n; ++i)
      v.push_back({static_cast<double>(i) * 1.1, static_cast<double>(i) * 2.2});
   return v;
}

// ── Benchmark harness ────────────────────────────────────────────────────────

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

   // Warmup
   for (int i = 0; i < 200; ++i)
   {
      fn();
      clobber_memory();
   }

   // Calibration: run for ~30ms to estimate iteration cost
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

   // Auto-batch fast operations so each outer iteration takes ≥100ns,
   // well above clock resolution and loop-overhead noise.  The inner loop
   // counter + clobber_memory() costs ~1-2ns per iteration, so at batch=1
   // anything faster than ~5ns is dominated by overhead.
   size_t batch = 1;
   if (ns_per_op < 100.0)
      batch = std::max<size_t>(1, static_cast<size_t>(100.0 / std::max(ns_per_op, 0.01)));

   // Target enough iterations for ~200ms of measurement
   size_t target = std::max<size_t>(1000,
       static_cast<size_t>(200'000'000.0 / (ns_per_op * static_cast<double>(batch))));

   // Measured run
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

   // Show fractional nanoseconds for sub-10ns operations
   if (mean_ns_f < 10.0)
      std::printf("  %-45s %12" PRIu64 " ops/s  %6.1f ns  %6zu B  (%.0f ops)\n",
                  name, ops_per_sec, mean_ns_f, data_bytes, total_ops);
   else
      std::printf("  %-45s %12" PRIu64 " ops/s  %6.0f ns  %6zu B  (%.0f ops)\n",
                  name, ops_per_sec, mean_ns_f, data_bytes, total_ops);
}

// ── Print helpers ────────────────────────────────────────────────────────────

void print_header(const char* group)
{
   std::printf("\n=== %s ===\n", group);
   std::printf("  %-45s %12s  %8s  %6s\n", "Benchmark", "Throughput", "Latency", "Bytes");
   std::printf("  %s\n",
               std::string(45 + 12 + 8 + 6 + 10, '-').c_str());
}

void print_comparison(const char* label,
                      const char* name_a,
                      double      ns_a,
                      const char* name_b,
                      double      ns_b)
{
   if (ns_a > 0.001 && ns_b > 0.001)
   {
      double ratio = ns_b / ns_a;
      std::printf("    %s: %s is %.1fx %s than %s\n", label, name_a,
                  ratio > 1.0 ? ratio : 1.0 / ratio, ratio > 1.0 ? "faster" : "slower", name_b);
   }
}

// ── Benchmark groups ─────────────────────────────────────────────────────────

// Two-pass fracpack: compute size, allocate once, write into fixed buffer.
// Same pattern binary/bincode/avro use.
template <typename T>
std::vector<char> to_frac_two_pass(const T& value)
{
   psio::size_stream ss;
   psio::to_frac(value, ss);
   std::vector<char> result(ss.size);
   psio::fixed_buf_stream fbs(result.data(), ss.size);
   psio::to_frac(value, fbs);
   return result;
}

void bench_pack()
{
   print_header("Pack (value -> bytes)");

   auto point  = make_point();
   auto token  = make_token();
   auto user   = make_user();
   auto order  = make_order();
   auto sensor = make_sensor();
   auto tree   = make_tree(6);  // 63 nodes

   auto point_packed  = psio::to_frac(point);
   auto token_packed  = psio::to_frac(token);
   auto user_packed   = psio::to_frac(user);
   auto order_packed  = psio::to_frac(order);
   auto sensor_packed = psio::to_frac(sensor);
   auto tree_packed   = psio::to_frac(tree);

   bench("pack/BPoint", point_packed.size(), [&] {
      auto r = psio::to_frac(point);
      do_not_optimize(r.data());
      return r;
   });
   bench("pack/Token", token_packed.size(), [&] {
      auto r = psio::to_frac(token);
      do_not_optimize(r.data());
      return r;
   });
   bench("pack/UserProfile", user_packed.size(), [&] {
      auto r = psio::to_frac(user);
      do_not_optimize(r.data());
      return r;
   });
   bench("pack/Order", order_packed.size(), [&] {
      auto r = psio::to_frac(order);
      do_not_optimize(r.data());
      return r;
   });
   bench("pack/SensorReading", sensor_packed.size(), [&] {
      auto r = psio::to_frac(sensor);
      do_not_optimize(r.data());
      return r;
   });
   bench("pack/TreeNode(depth=6)", tree_packed.size(), [&] {
      auto r = psio::to_frac(tree);
      do_not_optimize(r.data());
      return r;
   });

   // --- Fracpack pack cost breakdown: size-only vs two-pass vs current ---
   print_header("Fracpack Pack Breakdown: size-only / two-pass / current (vector growth)");

   bench("fracsize/BPoint", point_packed.size(), [&] {
      auto sz = psio::fracpack_size(point);
      do_not_optimize(sz);
      return sz;
   });
   bench("fracsize/Token", token_packed.size(), [&] {
      auto sz = psio::fracpack_size(token);
      do_not_optimize(sz);
      return sz;
   });
   bench("fracsize/UserProfile", user_packed.size(), [&] {
      auto sz = psio::fracpack_size(user);
      do_not_optimize(sz);
      return sz;
   });
   bench("fracsize/Order", order_packed.size(), [&] {
      auto sz = psio::fracpack_size(order);
      do_not_optimize(sz);
      return sz;
   });
   bench("fracsize/SensorReading", sensor_packed.size(), [&] {
      auto sz = psio::fracpack_size(sensor);
      do_not_optimize(sz);
      return sz;
   });

   bench("pack-twopass/BPoint", point_packed.size(), [&] {
      auto r = to_frac_two_pass(point);
      do_not_optimize(r.data());
      return r;
   });
   bench("pack-twopass/Token", token_packed.size(), [&] {
      auto r = to_frac_two_pass(token);
      do_not_optimize(r.data());
      return r;
   });
   bench("pack-twopass/UserProfile", user_packed.size(), [&] {
      auto r = to_frac_two_pass(user);
      do_not_optimize(r.data());
      return r;
   });
   bench("pack-twopass/Order", order_packed.size(), [&] {
      auto r = to_frac_two_pass(order);
      do_not_optimize(r.data());
      return r;
   });
   bench("pack-twopass/SensorReading", sensor_packed.size(), [&] {
      auto r = to_frac_two_pass(sensor);
      do_not_optimize(r.data());
      return r;
   });

   // Print comparison
   std::printf("\n  -- Pack Breakdown Summary --\n");
   std::printf("  %-16s %10s %10s %10s\n", "", "size-only", "two-pass", "current");

   auto find = [](const char* name) -> double {
      for (auto& r : g_results)
         if (r.name == name)
            return r.mean_ns;
      return 0.0;
   };

   const char* types[] = {"BPoint", "Token", "UserProfile", "Order", "SensorReading"};
   for (auto* t : types)
   {
      auto sz  = find((std::string("fracsize/") + t).c_str());
      auto tp  = find((std::string("pack-twopass/") + t).c_str());
      auto cur = find((std::string("pack/") + t).c_str());
      std::printf("  %-16s %7.0f ns %7.0f ns %7.0f ns\n", t, sz, tp, cur);
   }
}

void bench_unpack()
{
   print_header("Unpack (bytes -> value)");

   auto point_data  = psio::to_frac(make_point());
   auto token_data  = psio::to_frac(make_token());
   auto user_data   = psio::to_frac(make_user());
   auto order_data  = psio::to_frac(make_order());
   auto sensor_data = psio::to_frac(make_sensor());
   auto tree_data   = psio::to_frac(make_tree(6));

   bench("unpack/BPoint", point_data.size(), [&] {
      auto r = psio::from_frac<BPoint>(
          std::span<const char>(point_data.data(), point_data.size()));
      do_not_optimize(r.x);
      return r;
   });
   bench("unpack/Token", token_data.size(), [&] {
      auto r =
          psio::from_frac<Token>(std::span<const char>(token_data.data(), token_data.size()));
      do_not_optimize(r.kind);
      return r;
   });
   bench("unpack/UserProfile", user_data.size(), [&] {
      auto r = psio::from_frac<UserProfile>(
          std::span<const char>(user_data.data(), user_data.size()));
      do_not_optimize(r.id);
      return r;
   });
   bench("unpack/Order", order_data.size(), [&] {
      auto r =
          psio::from_frac<Order>(std::span<const char>(order_data.data(), order_data.size()));
      do_not_optimize(r.id);
      return r;
   });
   bench("unpack/SensorReading", sensor_data.size(), [&] {
      auto r = psio::from_frac<SensorReading>(
          std::span<const char>(sensor_data.data(), sensor_data.size()));
      do_not_optimize(r.timestamp);
      return r;
   });
   bench("unpack/TreeNode(depth=6)", tree_data.size(), [&] {
      auto r = psio::from_frac<TreeNode>(
          std::span<const char>(tree_data.data(), tree_data.size()));
      do_not_optimize(r.value);
      return r;
   });
}

void bench_view()
{
   print_header("View (zero-copy field access via frac_ref)");

   auto user_data  = psio::to_frac(make_user());
   auto order_data = psio::to_frac(make_order());
   auto sensor_data = psio::to_frac(make_sensor());

   auto user_span   = std::span<const char>(user_data.data(), user_data.size());
   auto order_span  = std::span<const char>(order_data.data(), order_data.size());
   auto sensor_span = std::span<const char>(sensor_data.data(), sensor_data.size());

   // Single field access
   bench("view-one/UserProfile.id", user_data.size(), [&] {
      escape(user_data.data());
      auto     ref = psio::frac_ref<UserProfile, std::span<const char>>(user_span);
      uint64_t id  = ref.fields().id();
      do_not_optimize(id);
      return id;
   });
   bench("view-one/UserProfile.name", user_data.size(), [&] {
      escape(user_data.data());
      auto             ref  = psio::frac_ref<UserProfile, std::span<const char>>(user_span);
      std::string_view name = ref.fields().name().str_view();
      do_not_optimize(name.data());
      return name;
   });
   bench("view-one/UserProfile.verified (last)", user_data.size(), [&] {
      escape(user_data.data());
      auto ref = psio::frac_ref<UserProfile, std::span<const char>>(user_span);
      bool v   = ref.fields().verified();
      do_not_optimize(v);
      return v;
   });

   // All fields
   bench("view-all/UserProfile", user_data.size(), [&] {
      escape(user_data.data());
      auto     ref      = psio::frac_ref<UserProfile, std::span<const char>>(user_span);
      uint64_t id       = ref.fields().id();
      do_not_optimize(id);
      std::string_view name  = ref.fields().name().str_view();
      do_not_optimize(name.size());
      std::string_view email = ref.fields().email().str_view();
      do_not_optimize(email.size());
      bool bio_set      = ref.fields().bio().has_value();
      do_not_optimize(bio_set);
      uint32_t age      = ref.fields().age();
      do_not_optimize(age);
      double score      = ref.fields().score();
      do_not_optimize(score);
      uint32_t tags_sz  = ref.fields().tags().raw_byte_size();
      do_not_optimize(tags_sz);
      bool verified     = ref.fields().verified();
      do_not_optimize(verified);
      return verified;
   });

   // Nested field access
   bench("view-one/Order.customer.name (nested)", order_data.size(), [&] {
      escape(order_data.data());
      auto             ref  = psio::frac_ref<Order, std::span<const char>>(order_span);
      std::string_view name = ref.fields().customer().name().str_view();
      do_not_optimize(name.size());
      return name;
   });

   // Wide struct single field
   bench("view-one/SensorReading.firmware (last str)", sensor_data.size(), [&] {
      escape(sensor_data.data());
      auto             ref = psio::frac_ref<SensorReading, std::span<const char>>(sensor_span);
      std::string_view fw  = ref.fields().firmware().str_view();
      do_not_optimize(fw.data());
      return fw;
   });

   // All fields — Order (nested struct + vector)
   bench("view-all/Order", order_data.size(), [&] {
      escape(order_data.data());
      auto ref = psio::frac_ref<Order, std::span<const char>>(order_span);
      auto f   = ref.fields();
      uint64_t         id    = f.id();
      do_not_optimize(id);
      std::string_view cname = f.customer().name().str_view();
      do_not_optimize(cname.data());
      uint32_t         nitems = f.items().raw_byte_size();
      do_not_optimize(nitems);
      double           total = f.total();
      do_not_optimize(total);
      bool             note_set = f.note().has_value();
      do_not_optimize(note_set);
      return id;
   });

   // All fields — Token
   {
      auto token_data = psio::to_frac(make_token());
      auto token_span = std::span<const char>(token_data.data(), token_data.size());
      bench("view-all/Token", token_data.size(), [&] {
         escape(token_data.data());
         auto ref = psio::frac_ref<Token, std::span<const char>>(token_span);
         auto f   = ref.fields();
         auto kind   = f.kind();
         do_not_optimize(kind);
         auto offset = f.offset();
         do_not_optimize(offset);
         auto length = f.length();
         do_not_optimize(length);
         std::string_view text = f.text().str_view();
         do_not_optimize(text.data());
         return kind;
      });
   }

   // All fields — Point (fixed-size, trivial)
   {
      auto point_data = psio::to_frac(make_point());
      auto point_span = std::span<const char>(point_data.data(), point_data.size());
      bench("view-all/Point", point_data.size(), [&] {
         escape(point_data.data());
         auto   ref = psio::frac_ref<BPoint, std::span<const char>>(point_span);
         double x   = ref.fields().x();
         double y   = ref.fields().y();
         double r   = x + y;
         do_not_optimize(r);
         return r;
      });
   }

   // All fields — SensorReading (18 fields)
   bench("view-all/SensorReading", sensor_data.size(), [&] {
      escape(sensor_data.data());
      auto ref = psio::frac_ref<SensorReading, std::span<const char>>(sensor_span);
      auto f   = ref.fields();
      uint64_t         ts         = f.timestamp();
      do_not_optimize(ts);
      std::string_view device_id  = f.device_id().str_view();
      do_not_optimize(device_id.data());
      double           temp       = f.temp();
      do_not_optimize(temp);
      double           humidity   = f.humidity();
      do_not_optimize(humidity);
      double           pressure   = f.pressure();
      do_not_optimize(pressure);
      double           accel_x    = f.accel_x();
      do_not_optimize(accel_x);
      double           accel_y    = f.accel_y();
      do_not_optimize(accel_y);
      double           accel_z    = f.accel_z();
      do_not_optimize(accel_z);
      double           gyro_x     = f.gyro_x();
      do_not_optimize(gyro_x);
      double           gyro_y     = f.gyro_y();
      do_not_optimize(gyro_y);
      double           gyro_z     = f.gyro_z();
      do_not_optimize(gyro_z);
      double           mag_x      = f.mag_x();
      do_not_optimize(mag_x);
      double           mag_y      = f.mag_y();
      do_not_optimize(mag_y);
      double           mag_z      = f.mag_z();
      do_not_optimize(mag_z);
      float            battery    = f.battery();
      do_not_optimize(battery);
      int16_t          signal_dbm = f.signal_dbm();
      do_not_optimize(signal_dbm);
      bool             err_set    = f.error_code().has_value();
      do_not_optimize(err_set);
      std::string_view firmware   = f.firmware().str_view();
      do_not_optimize(firmware.data());
      return ts;
   });
}

void bench_validate()
{
   print_header("Validate (zero-copy integrity check)");

   auto user_data   = psio::to_frac(make_user());
   auto order_data  = psio::to_frac(make_order());
   auto sensor_data = psio::to_frac(make_sensor());

   bench("validate/UserProfile", user_data.size(), [&] {
      auto r = psio::fracpack_validate<UserProfile>(
          std::span<const char>(user_data.data(), user_data.size()));
      do_not_optimize(r);
      return r;
   });
   bench("validate/Order", order_data.size(), [&] {
      auto r = psio::fracpack_validate<Order>(
          std::span<const char>(order_data.data(), order_data.size()));
      do_not_optimize(r);
      return r;
   });
   bench("validate/SensorReading", sensor_data.size(), [&] {
      auto r = psio::fracpack_validate<SensorReading>(
          std::span<const char>(sensor_data.data(), sensor_data.size()));
      do_not_optimize(r);
      return r;
   });

   auto point_data = psio::to_frac(make_point());
   bench("validate/Point", point_data.size(), [&] {
      auto r = psio::fracpack_validate<BPoint>(
          std::span<const char>(point_data.data(), point_data.size()));
      do_not_optimize(r);
      return r;
   });

   auto token_data = psio::to_frac(make_token());
   bench("validate/Token", token_data.size(), [&] {
      auto r = psio::fracpack_validate<Token>(
          std::span<const char>(token_data.data(), token_data.size()));
      do_not_optimize(r);
      return r;
   });
}

void bench_json()
{
   print_header("JSON (serialize / deserialize)");

   auto point = make_point();
   auto user  = make_user();
   auto order = make_order();

   auto point_json = psio::convert_to_json(point);
   auto user_json  = psio::convert_to_json(user);
   auto order_json = psio::convert_to_json(order);

   bench("json-write/BPoint", point_json.size(), [&] {
      auto r = psio::convert_to_json(point);
      do_not_optimize(r.data());
      return r;
   });
   bench("json-write/UserProfile", user_json.size(), [&] {
      auto r = psio::convert_to_json(user);
      do_not_optimize(r.data());
      return r;
   });
   bench("json-write/Order", order_json.size(), [&] {
      auto r = psio::convert_to_json(order);
      do_not_optimize(r.data());
      return r;
   });

   auto sensor      = make_sensor();
   auto sensor_json = psio::convert_to_json(sensor);
   bench("json-write/SensorReading", sensor_json.size(), [&] {
      auto r = psio::convert_to_json(sensor);
      do_not_optimize(r.data());
      return r;
   });

   // from_json expects integers as quoted strings for >32-bit types
   // Use the JSON we produce from to_json (integers are unquoted for <64-bit)
   // but that may not round-trip. Build JSON strings that from_json can parse.

   // For from_json, build compatible JSON with quoted integers for numeric fields
   std::string point_json_in = "{\"x\":3.14159265358979,\"y\":2.71828182845905}";

   // UserProfile JSON with quoted integers (from_json expects quoted for uint64)
   std::string user_json_in =
       "{\"id\":\"123456789\","
       "\"name\":\"Alice Johnson\","
       "\"email\":\"alice@example.com\","
       "\"bio\":\"Software engineer interested in distributed systems and WebAssembly\","
       "\"age\":\"32\","
       "\"score\":98.5,"
       "\"tags\":[\"developer\",\"wasm\",\"c++\",\"open-source\"],"
       "\"verified\":true}";

   bench("json-read/BPoint", point_json_in.size(), [&] {
      auto r = psio::convert_from_json<BPoint>(std::string(point_json_in));
      do_not_optimize(r.x);
      return r;
   });
   bench("json-read/UserProfile", user_json_in.size(), [&] {
      auto r = psio::convert_from_json<UserProfile>(std::string(user_json_in));
      do_not_optimize(r.id);
      return r;
   });

   std::string order_json_in =
       "{\"id\":\"42\","
       "\"customer\":" + user_json_in + ","
       "\"items\":[{\"product\":\"Widget A\",\"qty\":\"3\",\"unit_price\":19.99},"
                  "{\"product\":\"Widget B\",\"qty\":\"1\",\"unit_price\":49.99},"
                  "{\"product\":\"Widget C\",\"qty\":\"2\",\"unit_price\":9.99}],"
       "\"total\":139.93,"
       "\"note\":\"Please ship ASAP\"}";

   bench("json-read/Order", order_json_in.size(), [&] {
      auto r = psio::convert_from_json<Order>(std::string(order_json_in));
      do_not_optimize(r.id);
      return r;
   });

   std::string sensor_json_in =
       "{\"timestamp\":\"1700000000000\","
       "\"device_id\":\"sensor-42-alpha\","
       "\"temp\":23.5,\"humidity\":45.2,\"pressure\":1013.25,"
       "\"accel_x\":0.01,\"accel_y\":-0.02,\"accel_z\":9.81,"
       "\"gyro_x\":0.001,\"gyro_y\":-0.003,\"gyro_z\":0.002,"
       "\"mag_x\":25.0,\"mag_y\":-12.5,\"mag_z\":42.0,"
       "\"battery\":3.7,\"signal_dbm\":\"-67\","
       "\"error_code\":null,"
       "\"firmware\":\"v2.1.0-beta3\"}";

   bench("json-read/SensorReading", sensor_json_in.size(), [&] {
      auto r = psio::convert_from_json<SensorReading>(std::string(sensor_json_in));
      do_not_optimize(r.timestamp);
      return r;
   });
}

void bench_mutation()
{
   print_header("Mutation (in-place frac_ref vs unpack-modify-repack)");

   auto user_data  = psio::to_frac(make_user());
   auto order_data = psio::to_frac(make_order());

   // --- In-place mutation via frac_ref (vector<char> buffer) ---

   // Fixed field: best case
   bench("mutate-inplace/UserProfile.id (fixed)", user_data.size(), [&] {
      auto buf = user_data;  // copy
      auto doc = psio::frac_ref<UserProfile, std::vector<char>>(std::move(buf));
      doc.fields().id() = 99999ULL;
      do_not_optimize(doc.data());
      return doc.size();
   });

   // Variable field: same size
   bench("mutate-inplace/UserProfile.name (same-len)", user_data.size(), [&] {
      auto buf = user_data;  // copy
      auto doc = psio::frac_ref<UserProfile, std::vector<char>>(std::move(buf));
      doc.fields().name() = std::string("Bobby Johnson");  // same length as "Alice Johnson"
      do_not_optimize(doc.data());
      return doc.size();
   });

   // Variable field: grow
   bench("mutate-inplace/UserProfile.name (grow)", user_data.size(), [&] {
      auto buf = user_data;  // copy
      auto doc = psio::frac_ref<UserProfile, std::vector<char>>(std::move(buf));
      doc.fields().name() = std::string("Alexandrina Marguerite von Lichtensteinium III");
      do_not_optimize(doc.data());
      return doc.size();
   });

   // Variable field: shrink
   bench("mutate-inplace/UserProfile.name (shrink)", user_data.size(), [&] {
      auto buf = user_data;  // copy
      auto doc = psio::frac_ref<UserProfile, std::vector<char>>(std::move(buf));
      doc.fields().name() = std::string("Al");
      do_not_optimize(doc.data());
      return doc.size();
   });

   // Nested field mutation
   bench("mutate-inplace/Order.customer.age (nested)", order_data.size(), [&] {
      auto buf = order_data;  // copy
      auto doc = psio::frac_ref<Order, std::vector<char>>(std::move(buf));
      doc.fields().customer().age() = 55U;
      do_not_optimize(doc.data());
      return doc.size();
   });

   // Nested string mutation
   bench("mutate-inplace/Order.customer.name (nested str)", order_data.size(), [&] {
      auto buf = order_data;  // copy
      auto doc = psio::frac_ref<Order, std::vector<char>>(std::move(buf));
      doc.fields().customer().name() = std::string("Robert");
      do_not_optimize(doc.data());
      return doc.size();
   });

   // --- Naive alternative: full unpack -> modify -> repack ---

   bench("mutate-repack/UserProfile.id (full round-trip)", user_data.size(), [&] {
      auto u = psio::from_frac<UserProfile>(
          std::span<const char>(user_data.data(), user_data.size()));
      u.id = 99999;
      auto r = psio::to_frac(u);
      do_not_optimize(r.data());
      return r;
   });

   bench("mutate-repack/UserProfile.name (full round-trip)", user_data.size(), [&] {
      auto u = psio::from_frac<UserProfile>(
          std::span<const char>(user_data.data(), user_data.size()));
      u.name = "Bobby Johnson";
      auto r = psio::to_frac(u);
      do_not_optimize(r.data());
      return r;
   });

   bench("mutate-repack/Order.customer.age (full round-trip)", order_data.size(), [&] {
      auto o =
          psio::from_frac<Order>(std::span<const char>(order_data.data(), order_data.size()));
      o.customer.age = 55;
      auto r         = psio::to_frac(o);
      do_not_optimize(r.data());
      return r;
   });

   // SensorReading: fixed field mutation
   auto sensor_data = psio::to_frac(make_sensor());

   bench("mutate-inplace/SensorReading.signal_dbm (fixed)", sensor_data.size(), [&] {
      auto buf = sensor_data;  // copy
      auto doc = psio::frac_ref<SensorReading, std::vector<char>>(std::move(buf));
      doc.fields().signal_dbm() = -42;
      do_not_optimize(doc.data());
      return doc.size();
   });

   bench("mutate-repack/SensorReading.signal_dbm (full round-trip)", sensor_data.size(), [&] {
      auto s = psio::from_frac<SensorReading>(
          std::span<const char>(sensor_data.data(), sensor_data.size()));
      s.signal_dbm = -42;
      auto r       = psio::to_frac(s);
      do_not_optimize(r.data());
      return r;
   });
}

void bench_array_scaling()
{
   print_header("Array Scaling (vec<BPoint>)");

   for (int n : {10, 100, 1000, 10000})
   {
      auto pts  = make_point_vec(n);
      auto data = psio::to_frac(pts);

      std::string name_pack = "pack/vec<BPoint>[" + std::to_string(n) + "]";
      bench(name_pack.c_str(), data.size(), [&] {
         auto r = psio::to_frac(pts);
         do_not_optimize(r.data());
         return r;
      });

      std::string name_unpack = "unpack/vec<BPoint>[" + std::to_string(n) + "]";
      bench(name_unpack.c_str(), data.size(), [&] {
         auto r = psio::from_frac<std::vector<BPoint>>(
             std::span<const char>(data.data(), data.size()));
         do_not_optimize(r.data());
         return r;
      });

      // View: read all elements directly from the packed buffer.
      // Packed vec layout: [u32 byte_count] [element0] [element1] ...
      // BPoint is fixed-size (16 bytes: 2 × f64), so element i is at offset 4 + i*16.
      std::string name_view = "view/vec<BPoint>[" + std::to_string(n) + "]";
      bench(name_view.c_str(), data.size(), [&] {
         const char* base      = data.data();
         uint32_t    byte_count;
         std::memcpy(&byte_count, base, 4);
         int    count = static_cast<int>(byte_count / 16);
         double sum   = 0;
         for (int i = 0; i < count; ++i)
         {
            double x;
            std::memcpy(&x, base + 4 + i * 16, 8);
            sum += x;
         }
         do_not_optimize(sum);
         return sum;
      });
   }
}

void bench_view_vs_unpack()
{
   print_header("View vs Unpack Comparison (single field access)");

   auto user_data = psio::to_frac(make_user());
   auto user_span = std::span<const char>(user_data.data(), user_data.size());

   double view_ns = 0, unpack_ns = 0;

   bench("access-via-view/UserProfile.name", user_data.size(), [&] {
      escape(user_data.data());
      auto        ref = psio::frac_ref<UserProfile, std::span<const char>>(user_span);
      std::string n   = ref.fields().name();
      do_not_optimize(n.data());
      return n;
   });

   bench("access-via-unpack/UserProfile.name", user_data.size(), [&] {
      auto u = psio::from_frac<UserProfile>(user_span);
      do_not_optimize(u.name.data());
      return u.name;
   });

   // After running, find results and print comparison
   for (auto& r : g_results)
   {
      if (r.name == "access-via-view/UserProfile.name")
         view_ns = r.mean_ns;
      if (r.name == "access-via-unpack/UserProfile.name")
         unpack_ns = r.mean_ns;
   }
   if (view_ns > 0 && unpack_ns > 0)
      print_comparison("Single-field access", "view", view_ns, "full unpack", unpack_ns);
}

void bench_view_vs_native()
{
   print_header("View vs Native Struct Access");

   // ── BPoint: fixed-size — view should be identical to native ──
   {
      BPoint native_point = make_point();
      auto   point_packed = psio::to_frac(native_point);
      auto   point_span   = std::span<const char>(point_packed.data(), point_packed.size());

      bench("Point/native", point_packed.size(), [&] {
         double r = native_point.x + native_point.y;
         do_not_optimize(r);
         return r;
      });

      bench("Point/view", point_packed.size(), [&] {
         escape(point_packed.data());
         auto   ref = psio::frac_ref<BPoint, std::span<const char>>(point_span);
         double x   = ref.fields().x();
         double y   = ref.fields().y();
         double r   = x + y;
         do_not_optimize(r);
         return r;
      });

      bench("Point/unpack+access", point_packed.size(), [&] {
         escape(point_packed.data());
         auto   p = psio::from_frac<BPoint>(point_span);
         double r = p.x + p.y;
         do_not_optimize(r);
         return r;
      });
   }

   // ── UserProfile: complex — strings, vec, optional ──
   {
      UserProfile native_user = make_user();
      auto        user_packed = psio::to_frac(native_user);
      auto        user_span =
          std::span<const char>(user_packed.data(), user_packed.size());

      bench("UserProfile/native", user_packed.size(), [&] {
         do_not_optimize(native_user.id);
         do_not_optimize(native_user.name.data());
         do_not_optimize(native_user.email.data());
         do_not_optimize(native_user.bio.has_value());
         do_not_optimize(native_user.age);
         do_not_optimize(native_user.score);
         do_not_optimize(native_user.tags.size());
         do_not_optimize(native_user.verified);
         return native_user.verified;
      });

      bench("UserProfile/view", user_packed.size(), [&] {
         escape(user_packed.data());
         auto ref = psio::frac_ref<UserProfile, std::span<const char>>(user_span);
         auto f   = ref.fields();
         uint64_t         id       = f.id();
         do_not_optimize(id);
         std::string_view name     = f.name().str_view();
         do_not_optimize(name.data());
         std::string_view email    = f.email().str_view();
         do_not_optimize(email.data());
         bool             bio_set  = f.bio().has_value();
         do_not_optimize(bio_set);
         uint32_t         age      = f.age();
         do_not_optimize(age);
         double           score    = f.score();
         do_not_optimize(score);
         uint32_t         tags_sz  = f.tags().raw_byte_size();
         do_not_optimize(tags_sz);
         bool             verified = f.verified();
         do_not_optimize(verified);
         return verified;
      });

      bench("UserProfile/unpack+access", user_packed.size(), [&] {
         escape(user_packed.data());
         auto u = psio::from_frac<UserProfile>(user_span);
         do_not_optimize(u.id);
         do_not_optimize(u.name.data());
         do_not_optimize(u.email.data());
         do_not_optimize(u.bio.has_value());
         do_not_optimize(u.age);
         do_not_optimize(u.score);
         do_not_optimize(u.tags.size());
         do_not_optimize(u.verified);
         return u.verified;
      });
   }

   // ── SensorReading: wide — 18 fields ──
   {
      SensorReading native_sensor = make_sensor();
      auto          sensor_packed = psio::to_frac(native_sensor);
      auto          sensor_span =
          std::span<const char>(sensor_packed.data(), sensor_packed.size());

      bench("SensorReading/native", sensor_packed.size(), [&] {
         do_not_optimize(native_sensor.timestamp);
         do_not_optimize(native_sensor.device_id.data());
         do_not_optimize(native_sensor.temp);
         do_not_optimize(native_sensor.humidity);
         do_not_optimize(native_sensor.pressure);
         do_not_optimize(native_sensor.accel_x);
         do_not_optimize(native_sensor.accel_y);
         do_not_optimize(native_sensor.accel_z);
         do_not_optimize(native_sensor.gyro_x);
         do_not_optimize(native_sensor.gyro_y);
         do_not_optimize(native_sensor.gyro_z);
         do_not_optimize(native_sensor.mag_x);
         do_not_optimize(native_sensor.mag_y);
         do_not_optimize(native_sensor.mag_z);
         do_not_optimize(native_sensor.battery);
         do_not_optimize(native_sensor.signal_dbm);
         do_not_optimize(native_sensor.error_code.has_value());
         do_not_optimize(native_sensor.firmware.data());
         return native_sensor.timestamp;
      });

      bench("SensorReading/view", sensor_packed.size(), [&] {
         escape(sensor_packed.data());
         auto ref = psio::frac_ref<SensorReading, std::span<const char>>(sensor_span);
         auto f   = ref.fields();
         uint64_t                ts         = f.timestamp();
         do_not_optimize(ts);
         std::string_view        device_id  = f.device_id().str_view();
         do_not_optimize(device_id.data());
         double                  temp       = f.temp();
         do_not_optimize(temp);
         double                  humidity   = f.humidity();
         do_not_optimize(humidity);
         double                  pressure   = f.pressure();
         do_not_optimize(pressure);
         double                  accel_x    = f.accel_x();
         do_not_optimize(accel_x);
         double                  accel_y    = f.accel_y();
         do_not_optimize(accel_y);
         double                  accel_z    = f.accel_z();
         do_not_optimize(accel_z);
         double                  gyro_x     = f.gyro_x();
         do_not_optimize(gyro_x);
         double                  gyro_y     = f.gyro_y();
         do_not_optimize(gyro_y);
         double                  gyro_z     = f.gyro_z();
         do_not_optimize(gyro_z);
         double                  mag_x      = f.mag_x();
         do_not_optimize(mag_x);
         double                  mag_y      = f.mag_y();
         do_not_optimize(mag_y);
         double                  mag_z      = f.mag_z();
         do_not_optimize(mag_z);
         float                   battery    = f.battery();
         do_not_optimize(battery);
         int16_t                 signal_dbm = f.signal_dbm();
         do_not_optimize(signal_dbm);
         std::optional<uint32_t> error_code = f.error_code();
         do_not_optimize(error_code.has_value());
         std::string_view        firmware   = f.firmware().str_view();
         do_not_optimize(firmware.data());
         return ts;
      });

      bench("SensorReading/unpack+access", sensor_packed.size(), [&] {
         escape(sensor_packed.data());
         auto s = psio::from_frac<SensorReading>(sensor_span);
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
         do_not_optimize(s.error_code.has_value());
         do_not_optimize(s.firmware.data());
         return s.timestamp;
      });
   }

   // ── Key comparisons ──
   auto find = [](const char* name) -> double {
      for (auto& r : g_results)
         if (r.name == name)
            return r.mean_ns;
      return 0.0;
   };

   auto print_ratio = [](const char* label, double native, double view, double unpack) {
      if (native > 0.001 && view > 0.001)
      {
         double vr = view / native;
         double ur = unpack / native;
         std::printf("    %-14s view is %.1fx native, unpack is %.1fx native "
                     "(native=%.1fns, view=%.1fns, unpack=%.1fns)\n",
                     label, vr, ur, native, view, unpack);
      }
   };

   std::printf("\n  -- View vs Native Ratios --\n");
   print_ratio("Point:", find("Point/native"), find("Point/view"), find("Point/unpack+access"));
   print_ratio("UserProfile:", find("UserProfile/native"), find("UserProfile/view"), find("UserProfile/unpack+access"));
   print_ratio("SensorReading:", find("SensorReading/native"), find("SensorReading/view"), find("SensorReading/unpack+access"));
}

void bench_pack_vs_json()
{
   print_header("Fracpack vs JSON Size & Speed");

   auto user     = make_user();
   auto frac_buf = psio::to_frac(user);
   auto json_buf = psio::convert_to_json(user);

   std::printf("    UserProfile fracpack: %zu bytes\n", frac_buf.size());
   std::printf("    UserProfile JSON:     %zu bytes\n", json_buf.size());
   std::printf("    Ratio: fracpack is %.1fx smaller\n",
               static_cast<double>(json_buf.size()) / frac_buf.size());

   auto order     = make_order();
   auto frac_ord  = psio::to_frac(order);
   auto json_ord  = psio::convert_to_json(order);

   std::printf("    Order fracpack:       %zu bytes\n", frac_ord.size());
   std::printf("    Order JSON:           %zu bytes\n", json_ord.size());
   std::printf("    Ratio: fracpack is %.1fx smaller\n",
               static_cast<double>(json_ord.size()) / frac_ord.size());
}

// ── Key Format (sortable serialization) ─────────────────────────────────────

void bench_to_key()
{
   print_header("Key Format: size-only / two-pass / vector-growth");

   auto bench_key = [](const char* name, auto& value) {
      using T = std::decay_t<decltype(value)>;

      auto frac_data = psio::to_frac(value);
      auto key_data  = psio::convert_to_key(value);

      std::printf("  %s: fracpack %zu B, key %zu B\n", name, frac_data.size(), key_data.size());

      // Schema-driven to_key (original approach, for comparison)
      using namespace psio::schema_types;
      auto           schema = SchemaBuilder{}.insert<T>("T").build();
      CompiledSchema cschema{schema};
      auto           sd_name = std::string("to-key-schema/") + name;
      bench(sd_name.c_str(), key_data.size(), [&] {
         escape(const_cast<char*>(reinterpret_cast<const char*>(&value)));
         auto                  packed = psio::to_frac(value);
         std::span<const char> sp(packed.data(), packed.size());
         FracParser            parser(sp, cschema, "T", false);
         std::vector<char>     result;
         psio::vector_stream   vs{result};
         to_key(parser, vs);
         do_not_optimize(result.data());
      });

      // key_size benchmark (first pass only)
      auto size_name = std::string("keysize/") + name;
      bench(size_name.c_str(), key_data.size(), [&] {
         escape(const_cast<char*>(reinterpret_cast<const char*>(&value)));
         auto sz = psio::key_size(value);
         do_not_optimize(sz);
      });

      // to_key two-pass (key_size + fixed_buf_stream)
      auto tp_name = std::string("to-key/") + name;
      bench(tp_name.c_str(), key_data.size(), [&] {
         escape(const_cast<char*>(reinterpret_cast<const char*>(&value)));
         auto k = psio::convert_to_key(value);
         do_not_optimize(k.data());
      });

      // from_key benchmark
      auto from_name = std::string("from-key/") + name;
      bench(from_name.c_str(), key_data.size(), [&] {
         escape(const_cast<char*>(key_data.data()));
         auto v = psio::convert_from_key<T>(key_data);
         do_not_optimize(v);
      });
   };

   auto point  = make_point();
   auto token  = make_token();
   auto user   = make_user();
   auto order  = make_order();
   auto sensor = make_sensor();

   bench_key("BPoint", point);
   bench_key("Token", token);
   bench_key("UserProfile", user);
   bench_key("Order", order);
   bench_key("SensorReading", sensor);
}

// ── Multiformat Comparison ───────────────────────────────────────────────────

void bench_multiformat()
{
   print_header("Multiformat: Pack Speed, Unpack Speed, Wire Size");

   auto point  = make_point();
   auto token  = make_token();
   auto user   = make_user();
   auto order  = make_order();
   auto sensor = make_sensor();

   // Helper: pack all formats for a given type and print size table
   auto print_sizes = [](const char* type_name,
                         size_t frac_sz, size_t bin_sz, size_t bincode_sz,
                         size_t avro_sz, size_t key_sz, size_t json_sz) {
      std::printf("\n  Wire sizes for %s:\n", type_name);
      std::printf("    %-12s %6zu B\n", "fracpack", frac_sz);
      std::printf("    %-12s %6zu B  (%.1fx fracpack)\n", "binary", bin_sz,
                  static_cast<double>(bin_sz) / frac_sz);
      std::printf("    %-12s %6zu B  (%.1fx fracpack)\n", "bincode", bincode_sz,
                  static_cast<double>(bincode_sz) / frac_sz);
      std::printf("    %-12s %6zu B  (%.1fx fracpack)\n", "avro", avro_sz,
                  static_cast<double>(avro_sz) / frac_sz);
      std::printf("    %-12s %6zu B  (%.1fx fracpack)\n", "key", key_sz,
                  static_cast<double>(key_sz) / frac_sz);
      std::printf("    %-12s %6zu B  (%.1fx fracpack)\n", "json", json_sz,
                  static_cast<double>(json_sz) / frac_sz);
   };

   // ── BPoint ──
   {
      auto frac_buf    = psio::to_frac(point);
      auto bin_buf     = psio::convert_to_bin(point);
      auto bincode_buf = psio::convert_to_bincode(point);
      auto avro_buf    = psio::convert_to_avro(point);
      auto key_buf     = psio::convert_to_key(point);
      auto json_buf    = psio::convert_to_json(point);

      print_sizes("BPoint", frac_buf.size(), bin_buf.size(),
                  bincode_buf.size(), avro_buf.size(), key_buf.size(), json_buf.size());

      bench("multiformat-pack/BPoint/fracpack", frac_buf.size(), [&] {
         auto r = psio::to_frac(point);
         do_not_optimize(r.data());
         return r;
      });
      bench("multiformat-pack/BPoint/binary", bin_buf.size(), [&] {
         auto r = psio::convert_to_bin(point);
         do_not_optimize(r.data());
         return r;
      });
      bench("multiformat-pack/BPoint/bincode", bincode_buf.size(), [&] {
         auto r = psio::convert_to_bincode(point);
         do_not_optimize(r.data());
         return r;
      });
      bench("multiformat-pack/BPoint/avro", avro_buf.size(), [&] {
         auto r = psio::convert_to_avro(point);
         do_not_optimize(r.data());
         return r;
      });
      bench("multiformat-pack/BPoint/key", key_buf.size(), [&] {
         auto r = psio::convert_to_key(point);
         do_not_optimize(r.data());
         return r;
      });
      bench("multiformat-pack/BPoint/json", json_buf.size(), [&] {
         auto r = psio::convert_to_json(point);
         do_not_optimize(r.data());
         return r;
      });

      bench("multiformat-unpack/BPoint/fracpack", frac_buf.size(), [&] {
         auto r = psio::from_frac<BPoint>(
             std::span<const char>(frac_buf.data(), frac_buf.size()));
         do_not_optimize(r.x);
         return r;
      });
      bench("multiformat-unpack/BPoint/binary", bin_buf.size(), [&] {
         auto r = psio::convert_from_bin<BPoint>(bin_buf);
         do_not_optimize(r.x);
         return r;
      });
      bench("multiformat-unpack/BPoint/bincode", bincode_buf.size(), [&] {
         auto r = psio::convert_from_bincode<BPoint>(bincode_buf);
         do_not_optimize(r.x);
         return r;
      });
      bench("multiformat-unpack/BPoint/avro", avro_buf.size(), [&] {
         auto r = psio::convert_from_avro<BPoint>(avro_buf);
         do_not_optimize(r.x);
         return r;
      });
      bench("multiformat-unpack/BPoint/key", key_buf.size(), [&] {
         auto r = psio::convert_from_key<BPoint>(key_buf);
         do_not_optimize(r.x);
         return r;
      });
   }

   // ── Token ──
   {
      auto frac_buf    = psio::to_frac(token);
      auto bin_buf     = psio::convert_to_bin(token);
      auto bincode_buf = psio::convert_to_bincode(token);
      auto avro_buf    = psio::convert_to_avro(token);
      auto key_buf     = psio::convert_to_key(token);
      auto json_buf    = psio::convert_to_json(token);

      print_sizes("Token", frac_buf.size(), bin_buf.size(),
                  bincode_buf.size(), avro_buf.size(), key_buf.size(), json_buf.size());

      bench("multiformat-pack/Token/fracpack", frac_buf.size(), [&] {
         auto r = psio::to_frac(token);
         do_not_optimize(r.data());
         return r;
      });
      bench("multiformat-pack/Token/binary", bin_buf.size(), [&] {
         auto r = psio::convert_to_bin(token);
         do_not_optimize(r.data());
         return r;
      });
      bench("multiformat-pack/Token/bincode", bincode_buf.size(), [&] {
         auto r = psio::convert_to_bincode(token);
         do_not_optimize(r.data());
         return r;
      });
      bench("multiformat-pack/Token/avro", avro_buf.size(), [&] {
         auto r = psio::convert_to_avro(token);
         do_not_optimize(r.data());
         return r;
      });
      bench("multiformat-pack/Token/key", key_buf.size(), [&] {
         auto r = psio::convert_to_key(token);
         do_not_optimize(r.data());
         return r;
      });
      bench("multiformat-pack/Token/json", json_buf.size(), [&] {
         auto r = psio::convert_to_json(token);
         do_not_optimize(r.data());
         return r;
      });

      bench("multiformat-unpack/Token/fracpack", frac_buf.size(), [&] {
         auto r = psio::from_frac<Token>(
             std::span<const char>(frac_buf.data(), frac_buf.size()));
         do_not_optimize(r.kind);
         return r;
      });
      bench("multiformat-unpack/Token/binary", bin_buf.size(), [&] {
         auto r = psio::convert_from_bin<Token>(bin_buf);
         do_not_optimize(r.kind);
         return r;
      });
      bench("multiformat-unpack/Token/bincode", bincode_buf.size(), [&] {
         auto r = psio::convert_from_bincode<Token>(bincode_buf);
         do_not_optimize(r.kind);
         return r;
      });
      bench("multiformat-unpack/Token/avro", avro_buf.size(), [&] {
         auto r = psio::convert_from_avro<Token>(avro_buf);
         do_not_optimize(r.kind);
         return r;
      });
      bench("multiformat-unpack/Token/key", key_buf.size(), [&] {
         auto r = psio::convert_from_key<Token>(key_buf);
         do_not_optimize(r.kind);
         return r;
      });
   }

   // ── UserProfile ──
   {
      auto frac_buf    = psio::to_frac(user);
      auto bin_buf     = psio::convert_to_bin(user);
      auto bincode_buf = psio::convert_to_bincode(user);
      auto avro_buf    = psio::convert_to_avro(user);
      auto key_buf     = psio::convert_to_key(user);
      auto json_buf    = psio::convert_to_json(user);

      print_sizes("UserProfile", frac_buf.size(), bin_buf.size(),
                  bincode_buf.size(), avro_buf.size(), key_buf.size(), json_buf.size());

      bench("multiformat-pack/UserProfile/fracpack", frac_buf.size(), [&] {
         auto r = psio::to_frac(user);
         do_not_optimize(r.data());
         return r;
      });
      bench("multiformat-pack/UserProfile/binary", bin_buf.size(), [&] {
         auto r = psio::convert_to_bin(user);
         do_not_optimize(r.data());
         return r;
      });
      bench("multiformat-pack/UserProfile/bincode", bincode_buf.size(), [&] {
         auto r = psio::convert_to_bincode(user);
         do_not_optimize(r.data());
         return r;
      });
      bench("multiformat-pack/UserProfile/avro", avro_buf.size(), [&] {
         auto r = psio::convert_to_avro(user);
         do_not_optimize(r.data());
         return r;
      });
      bench("multiformat-pack/UserProfile/key", key_buf.size(), [&] {
         auto r = psio::convert_to_key(user);
         do_not_optimize(r.data());
         return r;
      });
      bench("multiformat-pack/UserProfile/json", json_buf.size(), [&] {
         auto r = psio::convert_to_json(user);
         do_not_optimize(r.data());
         return r;
      });

      bench("multiformat-unpack/UserProfile/fracpack", frac_buf.size(), [&] {
         auto r = psio::from_frac<UserProfile>(
             std::span<const char>(frac_buf.data(), frac_buf.size()));
         do_not_optimize(r.id);
         return r;
      });
      bench("multiformat-unpack/UserProfile/binary", bin_buf.size(), [&] {
         auto r = psio::convert_from_bin<UserProfile>(bin_buf);
         do_not_optimize(r.id);
         return r;
      });
      bench("multiformat-unpack/UserProfile/bincode", bincode_buf.size(), [&] {
         auto r = psio::convert_from_bincode<UserProfile>(bincode_buf);
         do_not_optimize(r.id);
         return r;
      });
      bench("multiformat-unpack/UserProfile/avro", avro_buf.size(), [&] {
         auto r = psio::convert_from_avro<UserProfile>(avro_buf);
         do_not_optimize(r.id);
         return r;
      });
      bench("multiformat-unpack/UserProfile/key", key_buf.size(), [&] {
         auto r = psio::convert_from_key<UserProfile>(key_buf);
         do_not_optimize(r.id);
         return r;
      });
   }

   // ── Order ──
   {
      auto frac_buf    = psio::to_frac(order);
      auto bin_buf     = psio::convert_to_bin(order);
      auto bincode_buf = psio::convert_to_bincode(order);
      auto avro_buf    = psio::convert_to_avro(order);
      auto key_buf     = psio::convert_to_key(order);
      auto json_buf    = psio::convert_to_json(order);

      print_sizes("Order", frac_buf.size(), bin_buf.size(),
                  bincode_buf.size(), avro_buf.size(), key_buf.size(), json_buf.size());

      bench("multiformat-pack/Order/fracpack", frac_buf.size(), [&] {
         auto r = psio::to_frac(order);
         do_not_optimize(r.data());
         return r;
      });
      bench("multiformat-pack/Order/binary", bin_buf.size(), [&] {
         auto r = psio::convert_to_bin(order);
         do_not_optimize(r.data());
         return r;
      });
      bench("multiformat-pack/Order/bincode", bincode_buf.size(), [&] {
         auto r = psio::convert_to_bincode(order);
         do_not_optimize(r.data());
         return r;
      });
      bench("multiformat-pack/Order/avro", avro_buf.size(), [&] {
         auto r = psio::convert_to_avro(order);
         do_not_optimize(r.data());
         return r;
      });
      bench("multiformat-pack/Order/key", key_buf.size(), [&] {
         auto r = psio::convert_to_key(order);
         do_not_optimize(r.data());
         return r;
      });
      bench("multiformat-pack/Order/json", json_buf.size(), [&] {
         auto r = psio::convert_to_json(order);
         do_not_optimize(r.data());
         return r;
      });

      bench("multiformat-unpack/Order/fracpack", frac_buf.size(), [&] {
         auto r = psio::from_frac<Order>(
             std::span<const char>(frac_buf.data(), frac_buf.size()));
         do_not_optimize(r.id);
         return r;
      });
      bench("multiformat-unpack/Order/binary", bin_buf.size(), [&] {
         auto r = psio::convert_from_bin<Order>(bin_buf);
         do_not_optimize(r.id);
         return r;
      });
      bench("multiformat-unpack/Order/bincode", bincode_buf.size(), [&] {
         auto r = psio::convert_from_bincode<Order>(bincode_buf);
         do_not_optimize(r.id);
         return r;
      });
      bench("multiformat-unpack/Order/avro", avro_buf.size(), [&] {
         auto r = psio::convert_from_avro<Order>(avro_buf);
         do_not_optimize(r.id);
         return r;
      });
      bench("multiformat-unpack/Order/key", key_buf.size(), [&] {
         auto r = psio::convert_from_key<Order>(key_buf);
         do_not_optimize(r.id);
         return r;
      });
   }

   // ── SensorReading ──
   {
      auto frac_buf    = psio::to_frac(sensor);
      auto bin_buf     = psio::convert_to_bin(sensor);
      auto bincode_buf = psio::convert_to_bincode(sensor);
      auto avro_buf    = psio::convert_to_avro(sensor);
      auto key_buf     = psio::convert_to_key(sensor);
      auto json_buf    = psio::convert_to_json(sensor);

      print_sizes("SensorReading", frac_buf.size(), bin_buf.size(),
                  bincode_buf.size(), avro_buf.size(), key_buf.size(), json_buf.size());

      bench("multiformat-pack/SensorReading/fracpack", frac_buf.size(), [&] {
         auto r = psio::to_frac(sensor);
         do_not_optimize(r.data());
         return r;
      });
      bench("multiformat-pack/SensorReading/binary", bin_buf.size(), [&] {
         auto r = psio::convert_to_bin(sensor);
         do_not_optimize(r.data());
         return r;
      });
      bench("multiformat-pack/SensorReading/bincode", bincode_buf.size(), [&] {
         auto r = psio::convert_to_bincode(sensor);
         do_not_optimize(r.data());
         return r;
      });
      bench("multiformat-pack/SensorReading/avro", avro_buf.size(), [&] {
         auto r = psio::convert_to_avro(sensor);
         do_not_optimize(r.data());
         return r;
      });
      bench("multiformat-pack/SensorReading/key", key_buf.size(), [&] {
         auto r = psio::convert_to_key(sensor);
         do_not_optimize(r.data());
         return r;
      });
      bench("multiformat-pack/SensorReading/json", json_buf.size(), [&] {
         auto r = psio::convert_to_json(sensor);
         do_not_optimize(r.data());
         return r;
      });

      bench("multiformat-unpack/SensorReading/fracpack", frac_buf.size(), [&] {
         auto r = psio::from_frac<SensorReading>(
             std::span<const char>(frac_buf.data(), frac_buf.size()));
         do_not_optimize(r.timestamp);
         return r;
      });
      bench("multiformat-unpack/SensorReading/binary", bin_buf.size(), [&] {
         auto r = psio::convert_from_bin<SensorReading>(bin_buf);
         do_not_optimize(r.timestamp);
         return r;
      });
      bench("multiformat-unpack/SensorReading/bincode", bincode_buf.size(), [&] {
         auto r = psio::convert_from_bincode<SensorReading>(bincode_buf);
         do_not_optimize(r.timestamp);
         return r;
      });
      bench("multiformat-unpack/SensorReading/avro", avro_buf.size(), [&] {
         auto r = psio::convert_from_avro<SensorReading>(avro_buf);
         do_not_optimize(r.timestamp);
         return r;
      });
      bench("multiformat-unpack/SensorReading/key", key_buf.size(), [&] {
         auto r = psio::convert_from_key<SensorReading>(key_buf);
         do_not_optimize(r.timestamp);
         return r;
      });
   }

   // ── Multiformat Summary Table ──
   std::printf("\n  -- Multiformat Summary (pack ns / unpack ns / wire bytes) --\n");
   std::printf("  %-16s %8s %8s %8s %8s %8s %8s\n",
               "", "fracpack", "binary", "bincode", "avro", "key", "json");
   std::printf("  %s\n", std::string(16 + 6*9, '-').c_str());

   auto find = [](const char* name) -> std::pair<double, size_t> {
      for (auto& r : g_results)
         if (r.name == name)
            return {r.mean_ns, r.bytes};
      return {0.0, 0};
   };

   const char* types[] = {"BPoint", "Token", "UserProfile", "Order", "SensorReading"};
   for (auto* t : types)
   {
      std::string fp = std::string("multiformat-pack/")  + t + "/fracpack";
      std::string bp = std::string("multiformat-pack/")  + t + "/binary";
      std::string cp = std::string("multiformat-pack/")  + t + "/bincode";
      std::string ap = std::string("multiformat-pack/")  + t + "/avro";
      std::string kp = std::string("multiformat-pack/")  + t + "/key";
      std::string jp = std::string("multiformat-pack/")  + t + "/json";

      auto [fns, fsz] = find(fp.c_str());
      auto [bns, bsz] = find(bp.c_str());
      auto [cns, csz] = find(cp.c_str());
      auto [ans, asz] = find(ap.c_str());
      auto [kns, ksz] = find(kp.c_str());
      auto [jns, jsz] = find(jp.c_str());

      std::printf("  %-16s %5.0f ns %5.0f ns %5.0f ns %5.0f ns %5.0f ns %5.0f ns   (pack)\n",
                  t, fns, bns, cns, ans, kns, jns);

      std::string fu = std::string("multiformat-unpack/") + t + "/fracpack";
      std::string bu = std::string("multiformat-unpack/") + t + "/binary";
      std::string cu = std::string("multiformat-unpack/") + t + "/bincode";
      std::string au = std::string("multiformat-unpack/") + t + "/avro";
      std::string ku = std::string("multiformat-unpack/") + t + "/key";

      auto [funs, _1] = find(fu.c_str());
      auto [buns, _2] = find(bu.c_str());
      auto [cuns, _3] = find(cu.c_str());
      auto [auns, _4] = find(au.c_str());
      auto [kuns, _5] = find(ku.c_str());

      std::printf("  %-16s %5.0f ns %5.0f ns %5.0f ns %5.0f ns %5.0f ns %8s   (unpack)\n",
                  "", funs, buns, cuns, auns, kuns, "—");
      std::printf("  %-16s %5zu B  %5zu B  %5zu B  %5zu B  %5zu B  %5zu B    (wire)\n\n",
                  "", fsz, bsz, csz, asz, ksz, jsz);
   }
}

// ── Summary ──────────────────────────────────────────────────────────────────

void print_summary()
{
   std::printf("\n");
   std::printf("=== Summary ===\n");
   std::printf("  %-45s %12s  %8s  %6s\n", "Benchmark", "Throughput", "Latency", "Bytes");
   std::printf("  %s\n", std::string(45 + 12 + 8 + 6 + 10, '-').c_str());
   for (auto& r : g_results)
   {
      if (r.mean_ns < 10.0)
         std::printf("  %-45s %12" PRIu64 " ops/s  %6.1f ns  %6zu B\n", r.name.c_str(),
                     r.ops_per_sec, r.mean_ns, r.bytes);
      else
         std::printf("  %-45s %12" PRIu64 " ops/s  %6.0f ns  %6zu B\n", r.name.c_str(),
                     r.ops_per_sec, r.mean_ns, r.bytes);
   }

   // Key comparisons
   std::printf("\n=== Key Comparisons ===\n");

   auto find = [](const char* name) -> double {
      for (auto& r : g_results)
         if (r.name == name)
            return r.mean_ns;
      return 0.0;
   };

   // Mutation: in-place vs repack
   auto mut_ip = find("mutate-inplace/UserProfile.id (fixed)");
   auto mut_rp = find("mutate-repack/UserProfile.id (full round-trip)");
   print_comparison("Mutate UserProfile.id", "in-place", mut_ip, "repack", mut_rp);

   auto mut_ip_s = find("mutate-inplace/UserProfile.name (same-len)");
   auto mut_rp_s = find("mutate-repack/UserProfile.name (full round-trip)");
   print_comparison("Mutate UserProfile.name", "in-place", mut_ip_s, "repack", mut_rp_s);

   auto mut_ip_n = find("mutate-inplace/Order.customer.age (nested)");
   auto mut_rp_n = find("mutate-repack/Order.customer.age (full round-trip)");
   print_comparison("Mutate Order.customer.age", "in-place", mut_ip_n, "repack", mut_rp_n);

   // Pack vs JSON
   auto pack_u = find("pack/UserProfile");
   auto json_u = find("json-write/UserProfile");
   print_comparison("Serialize UserProfile", "fracpack", pack_u, "json", json_u);

   auto unpack_u = find("unpack/UserProfile");
   auto jsonr_u  = find("json-read/UserProfile");
   print_comparison("Deserialize UserProfile", "fracpack", unpack_u, "json", jsonr_u);
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main()
{
   std::printf("Fracpack Benchmark Suite\n");
   std::printf("========================\n");

   bench_pack();
   bench_unpack();
   bench_view();
   bench_validate();
   bench_json();
   bench_mutation();
   bench_array_scaling();
   bench_view_vs_unpack();
   bench_view_vs_native();
   bench_pack_vs_json();
   bench_to_key();
   bench_multiformat();
   print_summary();

   return 0;
}
