// Reflect-based FlatBuffers vs Official FlatBuffers comparison benchmark
//
// Proves that PSIO_REFLECT (code-as-schema, no IDL, no flatc codegen) can
// serialize to wire-compatible FlatBuffer format at competitive speed.
//
// Build:
//   cmake -B build/Release -G Ninja -DCMAKE_BUILD_TYPE=Release \
//         -DPSIO_ENABLE_BENCHMARKS=ON
//   cmake --build build/Release --target psio_bench_reflect_flatbuf
// Run:
//   ./build/Release/bin/psio_bench_reflect_flatbuf

#include <psio/to_flatbuf.hpp>
#include <psio/reflect.hpp>
#include "benchmarks/bench_schemas_generated.h"

#include <flatbuffers/flatbuffers.h>

#include <cassert>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
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

// ── Benchmark harness ────────────────────────────────────────────────────────

struct BenchResult
{
   std::string name;
   uint64_t    ops_per_sec;
   double      mean_ns;
   size_t      bytes;
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
   auto elapsed_ns =
       std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count();

   double   total_ops  = static_cast<double>(target) * static_cast<double>(batch);
   double   mean_ns_f  = static_cast<double>(elapsed_ns) / total_ops;
   uint64_t ops_per_sec =
       mean_ns_f > 0.001 ? static_cast<uint64_t>(1'000'000'000.0 / mean_ns_f) : 0;

   g_results.push_back({name, ops_per_sec, mean_ns_f, data_bytes});

   if (mean_ns_f < 10.0)
      std::printf("  %-45s %12" PRIu64 " ops/s  %6.1f ns  %4zu B\n", name, ops_per_sec,
                  mean_ns_f, data_bytes);
   else
      std::printf("  %-45s %12" PRIu64 " ops/s  %6.0f ns  %4zu B\n", name, ops_per_sec,
                  mean_ns_f, data_bytes);
}

void print_header(const char* group)
{
   std::printf("\n=== %s ===\n", group);
   std::printf("  %-45s %12s  %8s  %4s\n", "Benchmark", "Throughput", "Latency", "Size");
   std::printf("  %s\n", std::string(45 + 12 + 8 + 4 + 10, '-').c_str());
}

// ── Benchmark types (same layout + field order as .fbs schema) ───────────────

struct BPoint
{
   double x;
   double y;
};
PSIO_REFLECT(BPoint, definitionWillNotChange(), x, y)

struct Token
{
   uint16_t    kind;
   uint32_t    offset;
   uint32_t    length;
   std::string text;
};
PSIO_REFLECT(Token, kind, offset, length, text)

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
PSIO_REFLECT(UserProfile, id, name, email, bio, age, score, tags, verified)

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

// ── Test data (identical values to bench_flatbuf.cpp) ────────────────────────

static const BPoint g_point{3.14159265358979, 2.71828182845905};

static const Token g_token{42, 1024, 15, "identifier_name"};

static const UserProfile g_user{
    123456789ULL,
    "Alice Johnson",
    "alice@example.com",
    "Software engineer interested in distributed systems and WebAssembly",
    32,
    98.5,
    {"developer", "wasm", "c++", "open-source"},
    true};

static Order make_order()
{
   Order o;
   o.id       = 987654321ULL;
   o.customer = g_user;
   for (int i = 0; i < 5; ++i)
      o.items.push_back(
          {"Product-" + std::to_string(i), static_cast<uint32_t>(i + 1), 19.99 + i * 5.0});
   o.total = 199.95;
   o.note  = "Please ship before Friday";
   return o;
}

static const Order g_order = make_order();

static const SensorReading g_sensor{1700000000000ULL,
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
                                    std::optional<uint32_t>{0},
                                    "v2.3.1-rc4"};

// ── Official FlatBuffers builders (for comparison) ───────────────────────────

flatbuffers::Offset<fb::Point> build_point(flatbuffers::FlatBufferBuilder& fbb)
{
   return fb::CreatePoint(fbb, 3.14159265358979, 2.71828182845905);
}

flatbuffers::Offset<fb::Token> build_token(flatbuffers::FlatBufferBuilder& fbb)
{
   auto text = fbb.CreateString("identifier_name");
   return fb::CreateToken(fbb, 42, 1024, 15, text);
}

flatbuffers::Offset<fb::UserProfile> build_user(flatbuffers::FlatBufferBuilder& fbb)
{
   auto name  = fbb.CreateString("Alice Johnson");
   auto email = fbb.CreateString("alice@example.com");
   auto bio =
       fbb.CreateString("Software engineer interested in distributed systems and WebAssembly");
   auto tags = fbb.CreateVectorOfStrings({"developer", "wasm", "c++", "open-source"});
   return fb::CreateUserProfile(fbb, 123456789ULL, name, email, bio, 32, 98.5, tags, true);
}

flatbuffers::Offset<fb::Order> build_order(flatbuffers::FlatBufferBuilder& fbb)
{
   auto customer = build_user(fbb);

   std::vector<flatbuffers::Offset<fb::LineItem>> items_vec;
   for (int i = 0; i < 5; ++i)
   {
      auto product = fbb.CreateString("Product-" + std::to_string(i));
      items_vec.push_back(
          fb::CreateLineItem(fbb, product, static_cast<uint32_t>(i + 1), 19.99 + i * 5.0));
   }
   auto items = fbb.CreateVector(items_vec);
   auto note  = fbb.CreateString("Please ship before Friday");
   return fb::CreateOrder(fbb, 987654321ULL, customer, items, 199.95, note);
}

flatbuffers::Offset<fb::SensorReading> build_sensor(flatbuffers::FlatBufferBuilder& fbb)
{
   auto device_id = fbb.CreateString("sensor-alpha-42");
   auto firmware  = fbb.CreateString("v2.3.1-rc4");
   return fb::CreateSensorReading(fbb, 1700000000000ULL, device_id, 23.5, 65.2, 1013.25, 0.01,
                                  -0.02, 9.81, 0.001, -0.003, 0.002, 25.1, -12.3, 42.7, 3.7f,
                                  -65, 0, firmware);
}

// ── Wire compatibility verification ──────────────────────────────────────────

void verify_wire_compat()
{
   std::printf("\n=== Wire Compatibility Verification ===\n");
   std::printf("  (reflect-based output read by official FlatBuffers reader)\n\n");

   // Point
   {
      flatbuffers::FlatBufferBuilder fbb(1024);
      psio::to_flatbuf_finish(fbb, g_point);
      auto p = flatbuffers::GetRoot<fb::Point>(fbb.GetBufferPointer());
      assert(p->x() == g_point.x);
      assert(p->y() == g_point.y);
      std::printf("  Point:         OK  x=%.5f y=%.5f\n", p->x(), p->y());
   }

   // Token
   {
      flatbuffers::FlatBufferBuilder fbb(1024);
      psio::to_flatbuf_finish(fbb, g_token);
      auto t = flatbuffers::GetRoot<fb::Token>(fbb.GetBufferPointer());
      assert(t->kind() == 42);
      assert(t->offset() == 1024);
      assert(t->length() == 15);
      assert(t->text() && std::string(t->text()->c_str()) == "identifier_name");
      std::printf("  Token:         OK  kind=%u text=%s\n", t->kind(), t->text()->c_str());
   }

   // UserProfile
   {
      flatbuffers::FlatBufferBuilder fbb(1024);
      psio::to_flatbuf_finish(fbb, g_user);
      auto u = flatbuffers::GetRoot<fb::UserProfile>(fbb.GetBufferPointer());
      assert(u->id() == 123456789ULL);
      assert(u->name() && std::string(u->name()->c_str()) == "Alice Johnson");
      assert(u->email() && std::string(u->email()->c_str()) == "alice@example.com");
      assert(u->bio() != nullptr);
      assert(u->age() == 32);
      assert(u->score() == 98.5);
      assert(u->tags() && u->tags()->size() == 4);
      assert(u->verified() == true);
      std::printf("  UserProfile:   OK  id=%" PRIu64 " name=%s tags=%u verified=%d\n", u->id(),
                  u->name()->c_str(), u->tags()->size(), u->verified());
   }

   // Order (nested: UserProfile + vector<LineItem>)
   {
      flatbuffers::FlatBufferBuilder fbb(1024);
      psio::to_flatbuf_finish(fbb, g_order);
      auto o = flatbuffers::GetRoot<fb::Order>(fbb.GetBufferPointer());
      assert(o->id() == 987654321ULL);
      assert(o->customer() && o->customer()->name());
      assert(std::string(o->customer()->name()->c_str()) == "Alice Johnson");
      assert(o->items() && o->items()->size() == 5);
      auto item0 = o->items()->Get(0);
      assert(item0->product() && std::string(item0->product()->c_str()) == "Product-0");
      assert(item0->qty() == 1);
      assert(o->total() == 199.95);
      assert(o->note() && std::string(o->note()->c_str()) == "Please ship before Friday");
      std::printf("  Order:         OK  id=%" PRIu64 " customer=%s items=%u total=%.2f\n",
                  o->id(), o->customer()->name()->c_str(), o->items()->size(), o->total());
   }

   // SensorReading
   {
      flatbuffers::FlatBufferBuilder fbb(1024);
      psio::to_flatbuf_finish(fbb, g_sensor);
      auto s = flatbuffers::GetRoot<fb::SensorReading>(fbb.GetBufferPointer());
      assert(s->timestamp() == 1700000000000ULL);
      assert(s->device_id() && std::string(s->device_id()->c_str()) == "sensor-alpha-42");
      assert(s->temp() == 23.5);
      assert(s->humidity() == 65.2);
      assert(s->battery() == 3.7f);
      assert(s->signal_dbm() == -65);
      assert(s->firmware() && std::string(s->firmware()->c_str()) == "v2.3.1-rc4");
      std::printf("  SensorReading: OK  ts=%" PRIu64 " dev=%s temp=%.1f fw=%s\n",
                  s->timestamp(), s->device_id()->c_str(), s->temp(),
                  s->firmware()->c_str());
   }

   std::printf("\n  All 5 types verified: reflect output is wire-compatible.\n");
}

// ── Wire size comparison ─────────────────────────────────────────────────────

void compare_wire_sizes()
{
   std::printf("\n=== Wire Size Comparison ===\n");
   std::printf("  %-20s %8s %8s\n", "Type", "Reflect", "Official");
   std::printf("  %s\n", std::string(38, '-').c_str());

   auto sizes = [](const char* name, auto reflect_fn, auto official_fn) {
      flatbuffers::FlatBufferBuilder r(1024);
      reflect_fn(r);
      size_t rsz = r.GetSize();

      flatbuffers::FlatBufferBuilder o(1024);
      auto root = official_fn(o);
      o.Finish(root);
      size_t osz = o.GetSize();

      std::printf("  %-20s %6zu B  %6zu B  %s\n", name, rsz, osz,
                  rsz == osz ? "" : "(diff)");
   };

   sizes(
       "Point", [](auto& fbb) { psio::to_flatbuf_finish(fbb, g_point); }, build_point);
   sizes(
       "Token", [](auto& fbb) { psio::to_flatbuf_finish(fbb, g_token); }, build_token);
   sizes(
       "UserProfile", [](auto& fbb) { psio::to_flatbuf_finish(fbb, g_user); }, build_user);
   sizes(
       "Order", [](auto& fbb) { psio::to_flatbuf_finish(fbb, g_order); }, build_order);
   sizes(
       "SensorReading", [](auto& fbb) { psio::to_flatbuf_finish(fbb, g_sensor); },
       build_sensor);
}

// ── Pack benchmarks ──────────────────────────────────────────────────────────

void bench_pack()
{
   print_header("Pack: Reflect-based (psio::to_flatbuf)");

   auto reflect_pack = [](const char* name, const auto& value) {
      flatbuffers::FlatBufferBuilder tmp(1024);
      psio::to_flatbuf_finish(tmp, value);
      size_t sz = tmp.GetSize();

      std::string bname = std::string("reflect-flatbuf/") + name;
      bench(bname.c_str(), sz, [&] {
         flatbuffers::FlatBufferBuilder fbb(1024);
         psio::to_flatbuf_finish(fbb, value);
         do_not_optimize(fbb.GetBufferPointer());
      });
   };

   reflect_pack("Point", g_point);
   reflect_pack("Token", g_token);
   reflect_pack("UserProfile", g_user);
   reflect_pack("Order", g_order);
   reflect_pack("SensorReading", g_sensor);

   print_header("Pack: Official FlatBuffers (flatc codegen)");

   auto official_pack = [](const char* name, auto build_fn) {
      flatbuffers::FlatBufferBuilder tmp(1024);
      auto                          root = build_fn(tmp);
      tmp.Finish(root);
      size_t sz = tmp.GetSize();

      std::string bname = std::string("official-flatbuf/") + name;
      bench(bname.c_str(), sz, [&] {
         flatbuffers::FlatBufferBuilder fbb(1024);
         auto                          r = build_fn(fbb);
         fbb.Finish(r);
         do_not_optimize(fbb.GetBufferPointer());
      });
   };

   official_pack("Point", build_point);
   official_pack("Token", build_token);
   official_pack("UserProfile", build_user);
   official_pack("Order", build_order);
   official_pack("SensorReading", build_sensor);
}

// ── Unpack benchmarks ────────────────────────────────────────────────────────

// Pre-build FlatBuffer buffers for unpack/view benchmarks
struct PreBuilt
{
   std::vector<uint8_t> point, token, user, order, sensor;

   PreBuilt()
   {
      auto build = [](auto build_fn) {
         flatbuffers::FlatBufferBuilder fbb(1024);
         auto                          root = build_fn(fbb);
         fbb.Finish(root);
         auto* p = fbb.GetBufferPointer();
         return std::vector<uint8_t>(p, p + fbb.GetSize());
      };
      point  = build(build_point);
      token  = build(build_token);
      user   = build(build_user);
      order  = build(build_order);
      sensor = build(build_sensor);
   }
};

static PreBuilt g_pre;

void bench_unpack()
{
   print_header("Unpack: Reflect-based (psio::from_flatbuf)");

   bench("reflect-unpack/Point", g_pre.point.size(), [&] {
      auto p = psio::from_flatbuf<BPoint>(g_pre.point.data());
      do_not_optimize(p.x);
      do_not_optimize(p.y);
   });
   bench("reflect-unpack/Token", g_pre.token.size(), [&] {
      auto t = psio::from_flatbuf<Token>(g_pre.token.data());
      do_not_optimize(t.kind);
      do_not_optimize(t.text.data());
   });
   bench("reflect-unpack/UserProfile", g_pre.user.size(), [&] {
      auto u = psio::from_flatbuf<UserProfile>(g_pre.user.data());
      do_not_optimize(u.id);
      do_not_optimize(u.name.data());
      do_not_optimize(u.tags.size());
   });
   bench("reflect-unpack/Order", g_pre.order.size(), [&] {
      auto o = psio::from_flatbuf<Order>(g_pre.order.data());
      do_not_optimize(o.id);
      do_not_optimize(o.customer.name.data());
      do_not_optimize(o.items.size());
   });
   bench("reflect-unpack/SensorReading", g_pre.sensor.size(), [&] {
      auto s = psio::from_flatbuf<SensorReading>(g_pre.sensor.data());
      do_not_optimize(s.timestamp);
      do_not_optimize(s.device_id.data());
      do_not_optimize(s.firmware.data());
   });

   print_header("Unpack: Official FlatBuffers (UnPack)");

   bench("official-unpack/Point", g_pre.point.size(), [&] {
      auto p = flatbuffers::GetRoot<fb::Point>(g_pre.point.data())->UnPack();
      do_not_optimize(p->x);
      do_not_optimize(p->y);
      delete p;
   });
   bench("official-unpack/Token", g_pre.token.size(), [&] {
      auto t = flatbuffers::GetRoot<fb::Token>(g_pre.token.data())->UnPack();
      do_not_optimize(t->kind);
      do_not_optimize(t->text.data());
      delete t;
   });
   bench("official-unpack/UserProfile", g_pre.user.size(), [&] {
      auto u = flatbuffers::GetRoot<fb::UserProfile>(g_pre.user.data())->UnPack();
      do_not_optimize(u->id);
      do_not_optimize(u->name.data());
      do_not_optimize(u->tags.size());
      delete u;
   });
   bench("official-unpack/Order", g_pre.order.size(), [&] {
      auto o = flatbuffers::GetRoot<fb::Order>(g_pre.order.data())->UnPack();
      do_not_optimize(o->id);
      do_not_optimize(o->customer->name.data());
      do_not_optimize(o->items.size());
      delete o;
   });
   bench("official-unpack/SensorReading", g_pre.sensor.size(), [&] {
      auto s = flatbuffers::GetRoot<fb::SensorReading>(g_pre.sensor.data())->UnPack();
      do_not_optimize(s->timestamp);
      do_not_optimize(s->device_id.data());
      do_not_optimize(s->firmware.data());
      delete s;
   });
}

// ── View benchmarks ──────────────────────────────────────────────────────────

void bench_view()
{
   print_header("View-All: Reflect-based (psio::flatbuf_view)");

   bench("reflect-view/Point", g_pre.point.size(), [&] {
      auto v = psio::flatbuf_view<BPoint>::from_buffer(g_pre.point.data());
      auto x = v.get<0>();
      auto y = v.get<1>();
      do_not_optimize(x);
      do_not_optimize(y);
   });
   bench("reflect-view/Token", g_pre.token.size(), [&] {
      auto v    = psio::flatbuf_view<Token>::from_buffer(g_pre.token.data());
      auto kind = v.get<0>();
      auto off  = v.get<1>();
      auto len  = v.get<2>();
      auto text = v.get<3>();
      do_not_optimize(kind);
      do_not_optimize(off);
      do_not_optimize(len);
      do_not_optimize(text->data());
   });
   bench("reflect-view/UserProfile", g_pre.user.size(), [&] {
      auto v        = psio::flatbuf_view<UserProfile>::from_buffer(g_pre.user.data());
      auto id       = v.get<0>();
      auto name     = v.get<1>();
      auto email    = v.get<2>();
      auto bio      = v.get<3>();
      auto age      = v.get<4>();
      auto score    = v.get<5>();
      auto tags     = v.get<6>();
      auto verified = v.get<7>();
      do_not_optimize(id);
      do_not_optimize(name->data());
      do_not_optimize(email->data());
      do_not_optimize(bio->data());
      do_not_optimize(age);
      do_not_optimize(score);
      do_not_optimize(tags->size());
      do_not_optimize(verified);
   });
   bench("reflect-view/Order", g_pre.order.size(), [&] {
      auto v     = psio::flatbuf_view<Order>::from_buffer(g_pre.order.data());
      auto id    = v.get<0>();
      auto cust  = v.get<1>();
      auto cname = cust.get<1>();
      auto items = v.get<2>();
      auto total = v.get<3>();
      auto note  = v.get<4>();
      do_not_optimize(id);
      do_not_optimize(cname->data());
      do_not_optimize(items->size());
      do_not_optimize(total);
      do_not_optimize(note->data());
   });
   bench("reflect-view/SensorReading", g_pre.sensor.size(), [&] {
      auto v   = psio::flatbuf_view<SensorReading>::from_buffer(g_pre.sensor.data());
      auto ts  = v.get<0>();
      auto did = v.get<1>();
      auto tmp = v.get<2>();
      auto hum = v.get<3>();
      auto prs = v.get<4>();
      auto ax  = v.get<5>();
      auto ay  = v.get<6>();
      auto az  = v.get<7>();
      auto gx  = v.get<8>();
      auto gy  = v.get<9>();
      auto gz  = v.get<10>();
      auto mx  = v.get<11>();
      auto my  = v.get<12>();
      auto mz  = v.get<13>();
      auto bat = v.get<14>();
      auto sig = v.get<15>();
      auto ec  = v.get<16>();
      auto fw  = v.get<17>();
      do_not_optimize(ts);
      do_not_optimize(did->data());
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
      do_not_optimize(fw->data());
   });

   print_header("View-All: Official FlatBuffers (generated accessors)");

   bench("official-view/Point", g_pre.point.size(), [&] {
      auto p = flatbuffers::GetRoot<fb::Point>(g_pre.point.data());
      auto x = p->x();
      auto y = p->y();
      do_not_optimize(x);
      do_not_optimize(y);
   });
   bench("official-view/Token", g_pre.token.size(), [&] {
      auto t    = flatbuffers::GetRoot<fb::Token>(g_pre.token.data());
      auto kind = t->kind();
      auto off  = t->offset();
      auto len  = t->length();
      auto text = t->text();
      do_not_optimize(kind);
      do_not_optimize(off);
      do_not_optimize(len);
      do_not_optimize(text->data());
   });
   bench("official-view/UserProfile", g_pre.user.size(), [&] {
      auto u        = flatbuffers::GetRoot<fb::UserProfile>(g_pre.user.data());
      auto id       = u->id();
      auto name     = u->name();
      auto email    = u->email();
      auto bio      = u->bio();
      auto age      = u->age();
      auto score    = u->score();
      auto tags     = u->tags();
      auto verified = u->verified();
      do_not_optimize(id);
      do_not_optimize(name->data());
      do_not_optimize(email->data());
      do_not_optimize(bio->data());
      do_not_optimize(age);
      do_not_optimize(score);
      do_not_optimize(tags->size());
      do_not_optimize(verified);
   });
   bench("official-view/Order", g_pre.order.size(), [&] {
      auto o     = flatbuffers::GetRoot<fb::Order>(g_pre.order.data());
      auto id    = o->id();
      auto cust  = o->customer();
      auto cname = cust->name();
      auto items = o->items();
      auto total = o->total();
      auto note  = o->note();
      do_not_optimize(id);
      do_not_optimize(cname->data());
      do_not_optimize(items->size());
      do_not_optimize(total);
      do_not_optimize(note->data());
   });
   bench("official-view/SensorReading", g_pre.sensor.size(), [&] {
      auto s   = flatbuffers::GetRoot<fb::SensorReading>(g_pre.sensor.data());
      auto ts  = s->timestamp();
      auto did = s->device_id();
      auto tmp = s->temp();
      auto hum = s->humidity();
      auto prs = s->pressure();
      auto ax  = s->accel_x();
      auto ay  = s->accel_y();
      auto az  = s->accel_z();
      auto gx  = s->gyro_x();
      auto gy  = s->gyro_y();
      auto gz  = s->gyro_z();
      auto mx  = s->mag_x();
      auto my  = s->mag_y();
      auto mz  = s->mag_z();
      auto bat = s->battery();
      auto sig = s->signal_dbm();
      auto ec  = s->error_code();
      auto fw  = s->firmware();
      do_not_optimize(ts);
      do_not_optimize(did->data());
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
      do_not_optimize(fw->data());
   });
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main()
{
   std::printf("Reflect-based FlatBuffers Benchmark\n");
   std::printf("====================================\n");
   std::printf("psio::to_flatbuf / from_flatbuf / flatbuf_view\n");
   std::printf("code-as-schema (no IDL, no flatc) vs official FlatBuffers\n");

   verify_wire_compat();
   compare_wire_sizes();
   bench_pack();
   bench_unpack();
   bench_view();

   // Summary
   std::printf("\n=== Full Head-to-Head Summary ===\n");
   std::printf("  %-20s %10s %10s %10s\n", "", "Pack", "Unpack", "View");
   std::printf("  %s\n", std::string(52, '-').c_str());

   // Results are in order: pack-reflect(5), pack-official(5),
   //   unpack-reflect(5+1 dummy), unpack-official(5), view-reflect(5), view-official(5)
   // Find results by name prefix
   auto find = [](const char* prefix) -> std::vector<BenchResult*> {
      std::vector<BenchResult*> out;
      for (auto& r : g_results)
         if (r.name.substr(0, strlen(prefix)) == prefix)
            out.push_back(&r);
      return out;
   };

   auto rp = find("reflect-flatbuf/");
   auto op = find("official-flatbuf/");
   auto ru = find("reflect-unpack/");
   auto ou = find("official-unpack/");
   auto rv = find("reflect-view/");
   auto ov = find("official-view/");

   const char* names[] = {"Point", "Token", "UserProfile", "Order", "SensorReading"};
   for (int i = 0; i < 5; ++i)
   {
      double pr = (i < (int)rp.size() && i < (int)op.size()) ? rp[i]->mean_ns / op[i]->mean_ns
                                                              : 0;
      double ur = (i < (int)ru.size() && i < (int)ou.size()) ? ru[i]->mean_ns / ou[i]->mean_ns
                                                              : 0;
      double vr = (i < (int)rv.size() && i < (int)ov.size()) ? rv[i]->mean_ns / ov[i]->mean_ns
                                                              : 0;
      std::printf("  %-20s %8.2fx   %8.2fx   %8.2fx\n", names[i], pr, ur, vr);
   }
   std::printf("\n  Ratio: <1.0 = reflect faster, >1.0 = official faster\n");

   return 0;
}
