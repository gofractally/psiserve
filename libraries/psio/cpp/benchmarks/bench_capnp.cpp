// Cap'n Proto benchmark — same schemas and data as bench_fracpack.cpp
//
// Measures: pack (build message), unpack (copy to native struct),
//           view (read fields from message without copy), and wire size.

#include "benchmarks/bench_schemas.capnp.h"

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <capnp/serialize-packed.h>
#include <kj/array.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cinttypes>
#include <string>
#include <vector>

// ── Native structs (what a developer would unpack into) ─────────────────────

struct NativePoint
{
   double x, y;
};

struct NativeToken
{
   uint16_t    kind;
   uint32_t    offset;
   uint32_t    length;
   std::string text;
};

struct NativeUserProfile
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

struct NativeLineItem
{
   std::string product;
   uint32_t    qty;
   double      unit_price;
};

struct NativeOrder
{
   uint64_t                      id;
   NativeUserProfile             customer;
   std::vector<NativeLineItem>   items;
   double                        total;
   std::string                   note;
};

struct NativeSensorReading
{
   uint64_t    timestamp;
   std::string device_id;
   double      temp, humidity, pressure;
   double      accel_x, accel_y, accel_z;
   double      gyro_x, gyro_y, gyro_z;
   double      mag_x, mag_y, mag_z;
   float       battery;
   int16_t     signal_dbm;
   uint32_t    error_code;
   std::string firmware;
};

// Helper: read capnp UserProfile::Reader into NativeUserProfile
void unpack_user(UserProfile::Reader r, NativeUserProfile& out)
{
   out.id       = r.getId();
   out.name     = r.getName();
   out.email    = r.getEmail();
   out.bio      = r.getBio();
   out.age      = r.getAge();
   out.score    = r.getScore();
   out.verified = r.getVerified();
   out.tags.clear();
   for (auto t : r.getTags())
      out.tags.emplace_back(t.cStr(), t.size());
}

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

// ── Helpers to build capnp messages ──────────────────────────────────────────

// Serialize a capnp message to a flat byte array (wire format)
kj::Array<capnp::word> serialize_msg(capnp::MallocMessageBuilder& builder)
{
   return capnp::messageToFlatArray(builder);
}

size_t wire_size(capnp::MallocMessageBuilder& builder)
{
   auto flat = serialize_msg(builder);
   return flat.asBytes().size();
}

// Build Point message
void build_point(capnp::MallocMessageBuilder& builder)
{
   auto p = builder.initRoot<Point>();
   p.setX(3.14159265358979);
   p.setY(2.71828182845905);
}

// Build Token message
void build_token(capnp::MallocMessageBuilder& builder)
{
   auto t = builder.initRoot<Token>();
   t.setKind(42);
   t.setOffset(1024);
   t.setLength(15);
   t.setText("identifier_name");
}

// Build UserProfile message
void build_user(capnp::MallocMessageBuilder& builder)
{
   auto u = builder.initRoot<UserProfile>();
   u.setId(123456789ULL);
   u.setName("Alice Johnson");
   u.setEmail("alice@example.com");
   u.setBio("Software engineer interested in distributed systems and WebAssembly");
   u.setAge(32);
   u.setScore(98.5);
   auto tags = u.initTags(4);
   tags.set(0, "developer");
   tags.set(1, "wasm");
   tags.set(2, "c++");
   tags.set(3, "open-source");
   u.setVerified(true);
}

// Build UserProfile sub-object in an existing struct
void build_user_into(UserProfile::Builder u)
{
   u.setId(123456789ULL);
   u.setName("Alice Johnson");
   u.setEmail("alice@example.com");
   u.setBio("Software engineer interested in distributed systems and WebAssembly");
   u.setAge(32);
   u.setScore(98.5);
   auto tags = u.initTags(4);
   tags.set(0, "developer");
   tags.set(1, "wasm");
   tags.set(2, "c++");
   tags.set(3, "open-source");
   u.setVerified(true);
}

// Build Order message
void build_order(capnp::MallocMessageBuilder& builder)
{
   auto o = builder.initRoot<Order>();
   o.setId(987654321ULL);
   build_user_into(o.initCustomer());
   auto items = o.initItems(5);
   for (int i = 0; i < 5; ++i)
   {
      items[i].setProduct(("Product-" + std::to_string(i)).c_str());
      items[i].setQty(static_cast<uint32_t>(i + 1));
      items[i].setUnitPrice(19.99 + i * 5.0);
   }
   o.setTotal(199.95);
   o.setNote("Please ship before Friday");
}

// Build SensorReading message
void build_sensor(capnp::MallocMessageBuilder& builder)
{
   auto s = builder.initRoot<SensorReading>();
   s.setTimestamp(1700000000000ULL);
   s.setDeviceId("sensor-alpha-42");
   s.setTemp(23.5);
   s.setHumidity(65.2);
   s.setPressure(1013.25);
   s.setAccelX(0.01);
   s.setAccelY(-0.02);
   s.setAccelZ(9.81);
   s.setGyroX(0.001);
   s.setGyroY(-0.003);
   s.setGyroZ(0.002);
   s.setMagX(25.1);
   s.setMagY(-12.3);
   s.setMagZ(42.7);
   s.setBattery(3.7f);
   s.setSignalDbm(-65);
   s.setErrorCode(0);  // 0 = not set
   s.setFirmware("v2.3.1-rc4");
}

// ── Benchmarks ───────────────────────────────────────────────────────────────

void bench_capnp_pack()
{
   print_header("Cap'n Proto: Pack (build + serialize)");

   // Get wire sizes
   {
      capnp::MallocMessageBuilder b;
      build_point(b);
      size_t sz = wire_size(b);
      std::printf("  Point wire size: %zu B\n", sz);
   }
   {
      capnp::MallocMessageBuilder b;
      build_token(b);
      size_t sz = wire_size(b);
      std::printf("  Token wire size: %zu B\n", sz);
   }
   {
      capnp::MallocMessageBuilder b;
      build_user(b);
      size_t sz = wire_size(b);
      std::printf("  UserProfile wire size: %zu B\n", sz);
   }
   {
      capnp::MallocMessageBuilder b;
      build_order(b);
      size_t sz = wire_size(b);
      std::printf("  Order wire size: %zu B\n", sz);
   }
   {
      capnp::MallocMessageBuilder b;
      build_sensor(b);
      size_t sz = wire_size(b);
      std::printf("  SensorReading wire size: %zu B\n", sz);
   }

   // Pack benchmarks: build message + serialize to flat array
   {
      capnp::MallocMessageBuilder b;
      build_point(b);
      size_t sz = wire_size(b);
      bench("capnp-pack/Point", sz, [&] {
         capnp::MallocMessageBuilder builder;
         build_point(builder);
         auto flat = capnp::messageToFlatArray(builder);
         do_not_optimize(flat.begin());
         return flat.size();
      });
   }
   {
      capnp::MallocMessageBuilder b;
      build_token(b);
      size_t sz = wire_size(b);
      bench("capnp-pack/Token", sz, [&] {
         capnp::MallocMessageBuilder builder;
         build_token(builder);
         auto flat = capnp::messageToFlatArray(builder);
         do_not_optimize(flat.begin());
         return flat.size();
      });
   }
   {
      capnp::MallocMessageBuilder b;
      build_user(b);
      size_t sz = wire_size(b);
      bench("capnp-pack/UserProfile", sz, [&] {
         capnp::MallocMessageBuilder builder;
         build_user(builder);
         auto flat = capnp::messageToFlatArray(builder);
         do_not_optimize(flat.begin());
         return flat.size();
      });
   }
   {
      capnp::MallocMessageBuilder b;
      build_order(b);
      size_t sz = wire_size(b);
      bench("capnp-pack/Order", sz, [&] {
         capnp::MallocMessageBuilder builder;
         build_order(builder);
         auto flat = capnp::messageToFlatArray(builder);
         do_not_optimize(flat.begin());
         return flat.size();
      });
   }
   {
      capnp::MallocMessageBuilder b;
      build_sensor(b);
      size_t sz = wire_size(b);
      bench("capnp-pack/SensorReading", sz, [&] {
         capnp::MallocMessageBuilder builder;
         build_sensor(builder);
         auto flat = capnp::messageToFlatArray(builder);
         do_not_optimize(flat.begin());
         return flat.size();
      });
   }
}

void bench_capnp_unpack()
{
   print_header("Cap'n Proto: View-All (zero-copy read all fields via Reader)");

   // Point
   {
      capnp::MallocMessageBuilder b;
      build_point(b);
      auto flat = capnp::messageToFlatArray(b);
      auto bytes = flat.asBytes();
      size_t sz = bytes.size();

      bench("capnp-view-all/Point", sz, [&] {
         auto reader = capnp::FlatArrayMessageReader(
             kj::ArrayPtr<const capnp::word>(
                 reinterpret_cast<const capnp::word*>(bytes.begin()),
                 bytes.size() / sizeof(capnp::word)));
         auto p = reader.getRoot<Point>();
         double x = p.getX();
         double y = p.getY();
         do_not_optimize(x);
         do_not_optimize(y);
         return x + y;
      });
   }

   // Token
   {
      capnp::MallocMessageBuilder b;
      build_token(b);
      auto flat = capnp::messageToFlatArray(b);
      auto bytes = flat.asBytes();
      size_t sz = bytes.size();

      bench("capnp-view-all/Token", sz, [&] {
         auto reader = capnp::FlatArrayMessageReader(
             kj::ArrayPtr<const capnp::word>(
                 reinterpret_cast<const capnp::word*>(bytes.begin()),
                 bytes.size() / sizeof(capnp::word)));
         auto t = reader.getRoot<Token>();
         auto kind = t.getKind();
         auto text = t.getText();
         do_not_optimize(kind);
         do_not_optimize(text.begin());
         return kind;
      });
   }

   // UserProfile
   {
      capnp::MallocMessageBuilder b;
      build_user(b);
      auto flat = capnp::messageToFlatArray(b);
      auto bytes = flat.asBytes();
      size_t sz = bytes.size();

      bench("capnp-view-all/UserProfile (read all fields)", sz, [&] {
         auto reader = capnp::FlatArrayMessageReader(
             kj::ArrayPtr<const capnp::word>(
                 reinterpret_cast<const capnp::word*>(bytes.begin()),
                 bytes.size() / sizeof(capnp::word)));
         auto u = reader.getRoot<UserProfile>();
         auto id = u.getId();
         do_not_optimize(id);
         auto name = u.getName();
         do_not_optimize(name.begin());
         auto email = u.getEmail();
         do_not_optimize(email.begin());
         auto bio = u.getBio();
         do_not_optimize(bio.begin());
         auto age = u.getAge();
         do_not_optimize(age);
         auto score = u.getScore();
         do_not_optimize(score);
         auto tags = u.getTags();
         do_not_optimize(tags.size());
         auto verified = u.getVerified();
         do_not_optimize(verified);
         return id;
      });
   }

   // Order
   {
      capnp::MallocMessageBuilder b;
      build_order(b);
      auto flat = capnp::messageToFlatArray(b);
      auto bytes = flat.asBytes();
      size_t sz = bytes.size();

      bench("capnp-view-all/Order (read all fields)", sz, [&] {
         auto reader = capnp::FlatArrayMessageReader(
             kj::ArrayPtr<const capnp::word>(
                 reinterpret_cast<const capnp::word*>(bytes.begin()),
                 bytes.size() / sizeof(capnp::word)));
         auto o = reader.getRoot<Order>();
         auto id = o.getId();
         do_not_optimize(id);
         auto customer = o.getCustomer();
         auto cname = customer.getName();
         do_not_optimize(cname.begin());
         auto items = o.getItems();
         do_not_optimize(items.size());
         auto total = o.getTotal();
         do_not_optimize(total);
         auto note = o.getNote();
         do_not_optimize(note.begin());
         return id;
      });
   }

   // SensorReading
   {
      capnp::MallocMessageBuilder b;
      build_sensor(b);
      auto flat = capnp::messageToFlatArray(b);
      auto bytes = flat.asBytes();
      size_t sz = bytes.size();

      bench("capnp-view-all/SensorReading (read all fields)", sz, [&] {
         auto reader = capnp::FlatArrayMessageReader(
             kj::ArrayPtr<const capnp::word>(
                 reinterpret_cast<const capnp::word*>(bytes.begin()),
                 bytes.size() / sizeof(capnp::word)));
         auto s = reader.getRoot<SensorReading>();
         auto ts = s.getTimestamp();
         do_not_optimize(ts);
         auto did = s.getDeviceId();
         do_not_optimize(did.begin());
         auto temp = s.getTemp();
         do_not_optimize(temp);
         auto hum = s.getHumidity();
         do_not_optimize(hum);
         auto pres = s.getPressure();
         do_not_optimize(pres);
         auto ax = s.getAccelX();
         do_not_optimize(ax);
         auto ay = s.getAccelY();
         do_not_optimize(ay);
         auto az = s.getAccelZ();
         do_not_optimize(az);
         auto gx = s.getGyroX();
         do_not_optimize(gx);
         auto gy = s.getGyroY();
         do_not_optimize(gy);
         auto gz = s.getGyroZ();
         do_not_optimize(gz);
         auto mx = s.getMagX();
         do_not_optimize(mx);
         auto my = s.getMagY();
         do_not_optimize(my);
         auto mz = s.getMagZ();
         do_not_optimize(mz);
         auto bat = s.getBattery();
         do_not_optimize(bat);
         auto sig = s.getSignalDbm();
         do_not_optimize(sig);
         auto ec = s.getErrorCode();
         do_not_optimize(ec);
         auto fw = s.getFirmware();
         do_not_optimize(fw.begin());
         return ts;
      });
   }
}

void bench_capnp_unpack_native()
{
   print_header("Cap'n Proto: Unpack (Reader -> native struct, real allocations)");

   // This is what a developer actually has to do with capnp if they need
   // a native struct: read every field through the Reader API, copy strings
   // and vectors into std:: containers.

   auto make_reader = [](auto& bytes) {
      return capnp::FlatArrayMessageReader(
          kj::ArrayPtr<const capnp::word>(
              reinterpret_cast<const capnp::word*>(bytes.begin()),
              bytes.size() / sizeof(capnp::word)));
   };

   // Point
   {
      capnp::MallocMessageBuilder b;
      build_point(b);
      auto flat = capnp::messageToFlatArray(b);
      auto bytes = flat.asBytes();

      bench("capnp-unpack/Point", bytes.size(), [&] {
         auto reader = make_reader(bytes);
         auto p = reader.getRoot<Point>();
         NativePoint out;
         out.x = p.getX();
         out.y = p.getY();
         do_not_optimize(out.x);
         do_not_optimize(out.y);
      });
   }

   // Token
   {
      capnp::MallocMessageBuilder b;
      build_token(b);
      auto flat = capnp::messageToFlatArray(b);
      auto bytes = flat.asBytes();

      bench("capnp-unpack/Token", bytes.size(), [&] {
         auto reader = make_reader(bytes);
         auto t = reader.getRoot<Token>();
         NativeToken out;
         out.kind   = t.getKind();
         out.offset = t.getOffset();
         out.length = t.getLength();
         out.text   = t.getText();
         do_not_optimize(out.kind);
         do_not_optimize(out.text.data());
      });
   }

   // UserProfile
   {
      capnp::MallocMessageBuilder b;
      build_user(b);
      auto flat = capnp::messageToFlatArray(b);
      auto bytes = flat.asBytes();

      bench("capnp-unpack/UserProfile", bytes.size(), [&] {
         auto reader = make_reader(bytes);
         NativeUserProfile out;
         unpack_user(reader.getRoot<UserProfile>(), out);
         do_not_optimize(out.id);
         do_not_optimize(out.name.data());
      });
   }

   // Order
   {
      capnp::MallocMessageBuilder b;
      build_order(b);
      auto flat = capnp::messageToFlatArray(b);
      auto bytes = flat.asBytes();

      bench("capnp-unpack/Order", bytes.size(), [&] {
         auto reader = make_reader(bytes);
         auto o = reader.getRoot<Order>();
         NativeOrder out;
         out.id    = o.getId();
         unpack_user(o.getCustomer(), out.customer);
         out.total = o.getTotal();
         out.note  = o.getNote();
         out.items.clear();
         for (auto item : o.getItems())
         {
            NativeLineItem li;
            li.product    = item.getProduct();
            li.qty        = item.getQty();
            li.unit_price = item.getUnitPrice();
            out.items.push_back(std::move(li));
         }
         do_not_optimize(out.id);
         do_not_optimize(out.items.data());
      });
   }

   // SensorReading
   {
      capnp::MallocMessageBuilder b;
      build_sensor(b);
      auto flat = capnp::messageToFlatArray(b);
      auto bytes = flat.asBytes();

      bench("capnp-unpack/SensorReading", bytes.size(), [&] {
         auto reader = make_reader(bytes);
         auto s = reader.getRoot<SensorReading>();
         NativeSensorReading out;
         out.timestamp  = s.getTimestamp();
         out.device_id  = s.getDeviceId();
         out.temp       = s.getTemp();
         out.humidity   = s.getHumidity();
         out.pressure   = s.getPressure();
         out.accel_x    = s.getAccelX();
         out.accel_y    = s.getAccelY();
         out.accel_z    = s.getAccelZ();
         out.gyro_x     = s.getGyroX();
         out.gyro_y     = s.getGyroY();
         out.gyro_z     = s.getGyroZ();
         out.mag_x      = s.getMagX();
         out.mag_y      = s.getMagY();
         out.mag_z      = s.getMagZ();
         out.battery    = s.getBattery();
         out.signal_dbm = s.getSignalDbm();
         out.error_code = s.getErrorCode();
         out.firmware   = s.getFirmware();
         do_not_optimize(out.timestamp);
         do_not_optimize(out.firmware.data());
      });
   }
}

void bench_capnp_view_warm()
{
   print_header("Cap'n Proto: View-All WARM (Reader pre-constructed, only field reads)");

   // Same as view-all but the FlatArrayMessageReader is constructed ONCE
   // outside the loop.  This isolates pure field-read cost from reader setup.

   auto make_reader = [](auto& bytes) {
      return capnp::FlatArrayMessageReader(
          kj::ArrayPtr<const capnp::word>(
              reinterpret_cast<const capnp::word*>(bytes.begin()),
              bytes.size() / sizeof(capnp::word)),
          capnp::ReaderOptions{/*.traversalLimitInWords=*/UINT64_MAX,
                               /*.nestingLimit=*/64});
   };

   // Point
   {
      capnp::MallocMessageBuilder b;
      build_point(b);
      auto flat = capnp::messageToFlatArray(b);
      auto bytes = flat.asBytes();
      auto reader = make_reader(bytes);

      bench("capnp-view-warm/Point", bytes.size(), [&] {
         auto p = reader.getRoot<Point>();
         do_not_optimize(p.getX());
         do_not_optimize(p.getY());
      });
   }

   // Token
   {
      capnp::MallocMessageBuilder b;
      build_token(b);
      auto flat = capnp::messageToFlatArray(b);
      auto bytes = flat.asBytes();
      auto reader = make_reader(bytes);

      bench("capnp-view-warm/Token", bytes.size(), [&] {
         auto t = reader.getRoot<Token>();
         do_not_optimize(t.getKind());
         do_not_optimize(t.getOffset());
         do_not_optimize(t.getLength());
         auto text = t.getText();
         do_not_optimize(text.begin());
      });
   }

   // UserProfile
   {
      capnp::MallocMessageBuilder b;
      build_user(b);
      auto flat = capnp::messageToFlatArray(b);
      auto bytes = flat.asBytes();
      auto reader = make_reader(bytes);

      bench("capnp-view-warm/UserProfile (all fields)", bytes.size(), [&] {
         auto u = reader.getRoot<UserProfile>();
         do_not_optimize(u.getId());
         auto n = u.getName(); do_not_optimize(n.begin());
         auto e = u.getEmail(); do_not_optimize(e.begin());
         auto b = u.getBio(); do_not_optimize(b.begin());
         do_not_optimize(u.getAge());
         do_not_optimize(u.getScore());
         auto tg = u.getTags(); do_not_optimize(tg.size());
         do_not_optimize(u.getVerified());
      });
   }

   // Order
   {
      capnp::MallocMessageBuilder b;
      build_order(b);
      auto flat = capnp::messageToFlatArray(b);
      auto bytes = flat.asBytes();
      auto reader = make_reader(bytes);

      bench("capnp-view-warm/Order (all fields)", bytes.size(), [&] {
         auto o = reader.getRoot<Order>();
         do_not_optimize(o.getId());
         auto c = o.getCustomer();
         auto cn = c.getName(); do_not_optimize(cn.begin());
         auto items = o.getItems(); do_not_optimize(items.size());
         do_not_optimize(o.getTotal());
         auto n = o.getNote(); do_not_optimize(n.begin());
      });
   }

   // SensorReading
   {
      capnp::MallocMessageBuilder b;
      build_sensor(b);
      auto flat = capnp::messageToFlatArray(b);
      auto bytes = flat.asBytes();
      auto reader = make_reader(bytes);

      bench("capnp-view-warm/SensorReading (all fields)", bytes.size(), [&] {
         auto s = reader.getRoot<SensorReading>();
         do_not_optimize(s.getTimestamp());
         auto d = s.getDeviceId(); do_not_optimize(d.begin());
         do_not_optimize(s.getTemp());
         do_not_optimize(s.getHumidity());
         do_not_optimize(s.getPressure());
         do_not_optimize(s.getAccelX());
         do_not_optimize(s.getAccelY());
         do_not_optimize(s.getAccelZ());
         do_not_optimize(s.getGyroX());
         do_not_optimize(s.getGyroY());
         do_not_optimize(s.getGyroZ());
         do_not_optimize(s.getMagX());
         do_not_optimize(s.getMagY());
         do_not_optimize(s.getMagZ());
         do_not_optimize(s.getBattery());
         do_not_optimize(s.getSignalDbm());
         do_not_optimize(s.getErrorCode());
         auto fw = s.getFirmware(); do_not_optimize(fw.begin());
      });
   }

   // View-one warm variants
   {
      capnp::MallocMessageBuilder b;
      build_user(b);
      auto flat = capnp::messageToFlatArray(b);
      auto bytes = flat.asBytes();
      auto reader = make_reader(bytes);

      bench("capnp-view-warm/UserProfile.id", bytes.size(), [&] {
         auto u = reader.getRoot<UserProfile>();
         do_not_optimize(u.getId());
      });

      bench("capnp-view-warm/UserProfile.name", bytes.size(), [&] {
         auto u = reader.getRoot<UserProfile>();
         auto n = u.getName();
         do_not_optimize(n.begin());
      });
   }

   {
      capnp::MallocMessageBuilder b;
      build_order(b);
      auto flat = capnp::messageToFlatArray(b);
      auto bytes = flat.asBytes();
      auto reader = make_reader(bytes);

      bench("capnp-view-warm/Order.customer.name", bytes.size(), [&] {
         auto o = reader.getRoot<Order>();
         auto n = o.getCustomer().getName();
         do_not_optimize(n.begin());
      });
   }
}

void bench_capnp_view_one()
{
   print_header("Cap'n Proto: View-One (single field from serialized)");

   // UserProfile.name — single field read from wire
   {
      capnp::MallocMessageBuilder b;
      build_user(b);
      auto flat = capnp::messageToFlatArray(b);
      auto bytes = flat.asBytes();
      size_t sz = bytes.size();

      bench("capnp-view-one/UserProfile.name", sz, [&] {
         auto reader = capnp::FlatArrayMessageReader(
             kj::ArrayPtr<const capnp::word>(
                 reinterpret_cast<const capnp::word*>(bytes.begin()),
                 bytes.size() / sizeof(capnp::word)));
         auto u = reader.getRoot<UserProfile>();
         auto name = u.getName();
         do_not_optimize(name.begin());
         return name.size();
      });
   }

   // UserProfile.id — scalar field
   {
      capnp::MallocMessageBuilder b;
      build_user(b);
      auto flat = capnp::messageToFlatArray(b);
      auto bytes = flat.asBytes();
      size_t sz = bytes.size();

      bench("capnp-view-one/UserProfile.id", sz, [&] {
         auto reader = capnp::FlatArrayMessageReader(
             kj::ArrayPtr<const capnp::word>(
                 reinterpret_cast<const capnp::word*>(bytes.begin()),
                 bytes.size() / sizeof(capnp::word)));
         auto u = reader.getRoot<UserProfile>();
         auto id = u.getId();
         do_not_optimize(id);
         return id;
      });
   }

   // Order.customer.name — nested field
   {
      capnp::MallocMessageBuilder b;
      build_order(b);
      auto flat = capnp::messageToFlatArray(b);
      auto bytes = flat.asBytes();
      size_t sz = bytes.size();

      bench("capnp-view-one/Order.customer.name", sz, [&] {
         auto reader = capnp::FlatArrayMessageReader(
             kj::ArrayPtr<const capnp::word>(
                 reinterpret_cast<const capnp::word*>(bytes.begin()),
                 bytes.size() / sizeof(capnp::word)));
         auto o = reader.getRoot<Order>();
         auto name = o.getCustomer().getName();
         do_not_optimize(name.begin());
         return name.size();
      });
   }
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main()
{
   std::printf("Cap'n Proto Benchmark (comparison with psio formats)\n");
   std::printf("====================================================\n");

   bench_capnp_pack();
   bench_capnp_unpack();
   bench_capnp_unpack_native();
   bench_capnp_view_warm();
   bench_capnp_view_one();

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
