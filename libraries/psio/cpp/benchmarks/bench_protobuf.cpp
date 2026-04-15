// Protocol Buffers benchmark — same schemas and data as bench_fracpack.cpp
//
// Measures: pack (serialize), unpack (parse to message object),
//           view (read fields from parsed message), and wire size.
//
// Note: Protobuf always requires a full parse before field access —
// there is no zero-copy view like FlatBuffers/Cap'n Proto/fracpack.

#include "benchmarks/bench_schemas.pb.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cinttypes>
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

// ── Helpers to build protobuf messages ──────────────────────────────────────

void build_point(pb::Point& p)
{
   p.set_x(3.14159265358979);
   p.set_y(2.71828182845905);
}

void build_token(pb::Token& t)
{
   t.set_kind(42);
   t.set_offset(1024);
   t.set_length(15);
   t.set_text("identifier_name");
}

void build_user(pb::UserProfile& u)
{
   u.set_id(123456789ULL);
   u.set_name("Alice Johnson");
   u.set_email("alice@example.com");
   u.set_bio("Software engineer interested in distributed systems and WebAssembly");
   u.set_age(32);
   u.set_score(98.5);
   u.add_tags("developer");
   u.add_tags("wasm");
   u.add_tags("c++");
   u.add_tags("open-source");
   u.set_verified(true);
}

void build_order(pb::Order& o)
{
   o.set_id(987654321ULL);
   build_user(*o.mutable_customer());
   for (int i = 0; i < 5; ++i)
   {
      auto* item = o.add_items();
      item->set_product("Product-" + std::to_string(i));
      item->set_qty(static_cast<uint32_t>(i + 1));
      item->set_unit_price(19.99 + i * 5.0);
   }
   o.set_total(199.95);
   o.set_note("Please ship before Friday");
}

void build_sensor(pb::SensorReading& s)
{
   s.set_timestamp(1700000000000ULL);
   s.set_device_id("sensor-alpha-42");
   s.set_temp(23.5);
   s.set_humidity(65.2);
   s.set_pressure(1013.25);
   s.set_accel_x(0.01);
   s.set_accel_y(-0.02);
   s.set_accel_z(9.81);
   s.set_gyro_x(0.001);
   s.set_gyro_y(-0.003);
   s.set_gyro_z(0.002);
   s.set_mag_x(25.1);
   s.set_mag_y(-12.3);
   s.set_mag_z(42.7);
   s.set_battery(3.7f);
   s.set_signal_dbm(-65);
   s.set_error_code(0);
   s.set_firmware("v2.3.1-rc4");
}

// ── Pack benchmarks ─────────────────────────────────────────────────────────

void bench_protobuf_pack()
{
   print_header("Protocol Buffers: Pack (build + serialize)");

   // Print wire sizes
   {
      pb::Point p; build_point(p);
      std::printf("  Point wire size: %zu B\n", p.ByteSizeLong());
   }
   {
      pb::Token t; build_token(t);
      std::printf("  Token wire size: %zu B\n", t.ByteSizeLong());
   }
   {
      pb::UserProfile u; build_user(u);
      std::printf("  UserProfile wire size: %zu B\n", u.ByteSizeLong());
   }
   {
      pb::Order o; build_order(o);
      std::printf("  Order wire size: %zu B\n", o.ByteSizeLong());
   }
   {
      pb::SensorReading s; build_sensor(s);
      std::printf("  SensorReading wire size: %zu B\n", s.ByteSizeLong());
   }

   // Pack: build message + serialize to string
   {
      pb::Point p; build_point(p);
      size_t sz = p.ByteSizeLong();
      bench("protobuf-pack/Point", sz, [&] {
         pb::Point msg;
         build_point(msg);
         std::string buf;
         msg.SerializeToString(&buf);
         do_not_optimize(buf.data());
         return buf.size();
      });
   }
   {
      pb::Token t; build_token(t);
      size_t sz = t.ByteSizeLong();
      bench("protobuf-pack/Token", sz, [&] {
         pb::Token msg;
         build_token(msg);
         std::string buf;
         msg.SerializeToString(&buf);
         do_not_optimize(buf.data());
         return buf.size();
      });
   }
   {
      pb::UserProfile u; build_user(u);
      size_t sz = u.ByteSizeLong();
      bench("protobuf-pack/UserProfile", sz, [&] {
         pb::UserProfile msg;
         build_user(msg);
         std::string buf;
         msg.SerializeToString(&buf);
         do_not_optimize(buf.data());
         return buf.size();
      });
   }
   {
      pb::Order o; build_order(o);
      size_t sz = o.ByteSizeLong();
      bench("protobuf-pack/Order", sz, [&] {
         pb::Order msg;
         build_order(msg);
         std::string buf;
         msg.SerializeToString(&buf);
         do_not_optimize(buf.data());
         return buf.size();
      });
   }
   {
      pb::SensorReading s; build_sensor(s);
      size_t sz = s.ByteSizeLong();
      bench("protobuf-pack/SensorReading", sz, [&] {
         pb::SensorReading msg;
         build_sensor(msg);
         std::string buf;
         msg.SerializeToString(&buf);
         do_not_optimize(buf.data());
         return buf.size();
      });
   }
}

// ── Unpack benchmarks (parse from wire) ─────────────────────────────────────

void bench_protobuf_unpack()
{
   print_header("Protocol Buffers: Unpack (parse from wire)");

   // Point
   {
      pb::Point p; build_point(p);
      std::string wire;
      p.SerializeToString(&wire);
      size_t sz = wire.size();
      bench("protobuf-unpack/Point", sz, [&] {
         pb::Point msg;
         msg.ParseFromString(wire);
         do_not_optimize(msg.x());
         do_not_optimize(msg.y());
         return msg.x();
      });
   }
   // Token
   {
      pb::Token t; build_token(t);
      std::string wire;
      t.SerializeToString(&wire);
      size_t sz = wire.size();
      bench("protobuf-unpack/Token", sz, [&] {
         pb::Token msg;
         msg.ParseFromString(wire);
         do_not_optimize(msg.kind());
         do_not_optimize(msg.text().data());
         return msg.kind();
      });
   }
   // UserProfile
   {
      pb::UserProfile u; build_user(u);
      std::string wire;
      u.SerializeToString(&wire);
      size_t sz = wire.size();
      bench("protobuf-unpack/UserProfile", sz, [&] {
         pb::UserProfile msg;
         msg.ParseFromString(wire);
         do_not_optimize(msg.id());
         do_not_optimize(msg.name().data());
         do_not_optimize(msg.tags_size());
         return msg.id();
      });
   }
   // Order
   {
      pb::Order o; build_order(o);
      std::string wire;
      o.SerializeToString(&wire);
      size_t sz = wire.size();
      bench("protobuf-unpack/Order", sz, [&] {
         pb::Order msg;
         msg.ParseFromString(wire);
         do_not_optimize(msg.id());
         do_not_optimize(msg.customer().name().data());
         do_not_optimize(msg.items_size());
         return msg.id();
      });
   }
   // SensorReading
   {
      pb::SensorReading s; build_sensor(s);
      std::string wire;
      s.SerializeToString(&wire);
      size_t sz = wire.size();
      bench("protobuf-unpack/SensorReading", sz, [&] {
         pb::SensorReading msg;
         msg.ParseFromString(wire);
         do_not_optimize(msg.timestamp());
         do_not_optimize(msg.device_id().data());
         do_not_optimize(msg.firmware().data());
         return msg.timestamp();
      });
   }
}

// ── View benchmarks ─────────────────────────────────────────────────────────
// Protobuf has no zero-copy view — must parse first, then read fields.
// We measure parse + read-all-fields as "view-all" and parse + read-one as "view-one"
// to be comparable with FlatBuffers/Cap'n Proto/fracpack view benchmarks.

void bench_protobuf_view()
{
   print_header("Protocol Buffers: View-All (parse + read all fields)");

   // Point
   {
      pb::Point p; build_point(p);
      std::string wire; p.SerializeToString(&wire);
      size_t sz = wire.size();
      bench("protobuf-view-all/Point", sz, [&] {
         pb::Point msg;
         msg.ParseFromString(wire);
         auto x = msg.x();
         auto y = msg.y();
         do_not_optimize(x);
         do_not_optimize(y);
         return x + y;
      });
   }
   // Token
   {
      pb::Token t; build_token(t);
      std::string wire; t.SerializeToString(&wire);
      size_t sz = wire.size();
      bench("protobuf-view-all/Token", sz, [&] {
         pb::Token msg;
         msg.ParseFromString(wire);
         auto kind = msg.kind();
         auto off  = msg.offset();
         auto len  = msg.length();
         auto& text = msg.text();
         do_not_optimize(kind);
         do_not_optimize(off);
         do_not_optimize(len);
         do_not_optimize(text.data());
         return kind;
      });
   }
   // UserProfile
   {
      pb::UserProfile u; build_user(u);
      std::string wire; u.SerializeToString(&wire);
      size_t sz = wire.size();
      bench("protobuf-view-all/UserProfile", sz, [&] {
         pb::UserProfile msg;
         msg.ParseFromString(wire);
         auto id       = msg.id();
         auto& name    = msg.name();
         auto& email   = msg.email();
         auto& bio     = msg.bio();
         auto age      = msg.age();
         auto score    = msg.score();
         auto& tags    = msg.tags();
         auto verified = msg.verified();
         do_not_optimize(id);
         do_not_optimize(name.data());
         do_not_optimize(email.data());
         do_not_optimize(bio.data());
         do_not_optimize(age);
         do_not_optimize(score);
         do_not_optimize(tags.size());
         do_not_optimize(verified);
         return id;
      });
   }
   // Order
   {
      pb::Order o; build_order(o);
      std::string wire; o.SerializeToString(&wire);
      size_t sz = wire.size();
      bench("protobuf-view-all/Order", sz, [&] {
         pb::Order msg;
         msg.ParseFromString(wire);
         auto id     = msg.id();
         auto& cust  = msg.customer();
         auto& cname = cust.name();
         auto& items = msg.items();
         auto total  = msg.total();
         auto& note  = msg.note();
         do_not_optimize(id);
         do_not_optimize(cname.data());
         do_not_optimize(items.size());
         do_not_optimize(total);
         do_not_optimize(note.data());
         return id;
      });
   }
   // SensorReading
   {
      pb::SensorReading s; build_sensor(s);
      std::string wire; s.SerializeToString(&wire);
      size_t sz = wire.size();
      bench("protobuf-view-all/SensorReading", sz, [&] {
         pb::SensorReading msg;
         msg.ParseFromString(wire);
         auto ts  = msg.timestamp();
         auto& did = msg.device_id();
         auto tmp = msg.temp();
         auto hum = msg.humidity();
         auto prs = msg.pressure();
         auto ax  = msg.accel_x();
         auto ay  = msg.accel_y();
         auto az  = msg.accel_z();
         auto gx  = msg.gyro_x();
         auto gy  = msg.gyro_y();
         auto gz  = msg.gyro_z();
         auto mx  = msg.mag_x();
         auto my  = msg.mag_y();
         auto mz  = msg.mag_z();
         auto bat = msg.battery();
         auto sig = msg.signal_dbm();
         auto ec  = msg.error_code();
         auto& fw = msg.firmware();
         do_not_optimize(ts);
         do_not_optimize(did.data());
         do_not_optimize(tmp);
         do_not_optimize(hum);
         do_not_optimize(prs);
         do_not_optimize(ax);
         do_not_optimize(ay);
         do_not_optimize(az);
         do_not_optimize(gx);
         do_not_optimize(gy);
         do_not_optimize(gz);
         do_not_optimize(mx);
         do_not_optimize(my);
         do_not_optimize(mz);
         do_not_optimize(bat);
         do_not_optimize(sig);
         do_not_optimize(ec);
         do_not_optimize(fw.data());
         return ts;
      });
   }
}

void bench_protobuf_view_one()
{
   print_header("Protocol Buffers: View-One (parse + single field read)");

   // UserProfile.name
   {
      pb::UserProfile u; build_user(u);
      std::string wire; u.SerializeToString(&wire);
      size_t sz = wire.size();
      bench("protobuf-view-one/UserProfile.name", sz, [&] {
         pb::UserProfile msg;
         msg.ParseFromString(wire);
         auto& name = msg.name();
         do_not_optimize(name.data());
         return name.size();
      });
   }
   // UserProfile.id
   {
      pb::UserProfile u; build_user(u);
      std::string wire; u.SerializeToString(&wire);
      size_t sz = wire.size();
      bench("protobuf-view-one/UserProfile.id", sz, [&] {
         pb::UserProfile msg;
         msg.ParseFromString(wire);
         auto id = msg.id();
         do_not_optimize(id);
         return id;
      });
   }
   // Order.customer.name
   {
      pb::Order o; build_order(o);
      std::string wire; o.SerializeToString(&wire);
      size_t sz = wire.size();
      bench("protobuf-view-one/Order.customer.name", sz, [&] {
         pb::Order msg;
         msg.ParseFromString(wire);
         auto& name = msg.customer().name();
         do_not_optimize(name.data());
         return name.size();
      });
   }
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main()
{
   std::printf("Protocol Buffers Benchmark (comparison with psio formats)\n");
   std::printf("==========================================================\n");

   bench_protobuf_pack();
   bench_protobuf_unpack();
   bench_protobuf_view();
   bench_protobuf_view_one();

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
