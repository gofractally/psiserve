// Standalone FlatBuffer benchmark — psio::fb_builder (zero dependency)
// vs official FlatBuffers library
//
// Build:
//   cmake -B build/Release -G Ninja -DCMAKE_BUILD_TYPE=Release \
//         -DPSIO_ENABLE_BENCHMARKS=ON
//   cmake --build build/Release --target psio_bench_standalone_flatbuf
// Run:
//   ./build/Release/bin/psio_bench_standalone_flatbuf

#include <psio/flatbuf.hpp>
#include <psio/reflect.hpp>
#include "benchmarks/bench_schemas_generated.h"

#include <flatbuffers/flatbuffers.h>

#include <array>
#include <cassert>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <flat_map>
#include <flat_set>
#include <optional>
#include <string>
#include <variant>
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

   double   total_ops   = static_cast<double>(target) * static_cast<double>(batch);
   double   mean_ns_f   = static_cast<double>(elapsed_ns) / total_ops;
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

// ── Benchmark types ─────────────────────────────────────────────────────────

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

// ── Types with embedded structs (BPoint is definitionWillNotChange → inline) ─

struct Segment
{
   BPoint      start;
   BPoint      end;
   std::string label;
};
PSIO_REFLECT(Segment, start, end, label)

// Verify BPoint qualifies as a FlatBuffer struct at compile time
static_assert(psio::detail::fbs::is_fb_struct<BPoint>(),
              "BPoint should be a FlatBuffer struct (definitionWillNotChange + all scalar)");
static_assert(!psio::detail::fbs::is_fb_struct<Token>(),
              "Token has a string field — not a struct");

// ── Enum type ───────────────────────────────────────────────────────────────

enum class Color : uint8_t
{
   Red   = 0,
   Green = 1,
   Blue  = 2
};
PSIO_REFLECT_ENUM(Color, Red, Green, Blue)

struct ColoredPoint
{
   double x;
   double y;
   Color  color;
};
PSIO_REFLECT(ColoredPoint, x, y, color)

// ── Fixed-length array type ─────────────────────────────────────────────────

struct Matrix2x2
{
   std::array<float, 4> data;
   std::string          label;
};
PSIO_REFLECT(Matrix2x2, data, label)

// ── Union (variant) type ────────────────────────────────────────────────────

struct Circle
{
   double radius;
};
PSIO_REFLECT(Circle, radius)

struct Rect
{
   double width;
   double height;
};
PSIO_REFLECT(Rect, width, height)

struct Shape
{
   std::string                  name;
   std::variant<Circle, Rect>   geom;
   Color                        color;
};
PSIO_REFLECT(Shape, name, geom, color)

// ── Nested FlatBuffer type ──────────────────────────────────────────────────

struct Envelope
{
   std::string              tag;
   psio::fb_nested<Order>   payload;
};
PSIO_REFLECT(Envelope, tag, payload)

// ── Struct with non-zero defaults ───────────────────────────────────────────

struct Config
{
   int32_t     timeout = 30;
   float       scale   = 1.0f;
   bool        enabled = true;
   std::string label;         // dynamic field, default ""
   uint16_t    retries = 3;
};
PSIO_REFLECT(Config, timeout, scale, enabled, label, retries)

// ── Sorted container types ─────────────────────────────────────────────────

struct Inventory
{
   std::flat_set<int32_t>              item_ids;
   std::flat_map<std::string, int32_t> counts;
   std::flat_map<int32_t, std::string> labels;
};
PSIO_REFLECT(Inventory, item_ids, counts, labels)

// ── Test data ────────────────────────────────────────────────────────────────

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

// ── Official FlatBuffers builders ────────────────────────────────────────────

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

// ── Wire compatibility: standalone output read by official reader ─────────

void verify_wire_compat()
{
   std::printf("\n=== Wire Compatibility ===\n");
   std::printf("  (standalone fb_builder output read by official FlatBuffers reader)\n\n");

   auto verify_roundtrip = [](const char* name, const auto& value, auto check_fn) {
      psio::fb_builder fbb;
      fbb.pack(value);
      check_fn(fbb.data());
      std::printf("  %-16s OK\n", name);
   };

   verify_roundtrip("Point", g_point, [](const uint8_t* buf) {
      auto p = flatbuffers::GetRoot<fb::Point>(buf);
      assert(p->x() == 3.14159265358979);
      assert(p->y() == 2.71828182845905);
   });

   verify_roundtrip("Token", g_token, [](const uint8_t* buf) {
      auto t = flatbuffers::GetRoot<fb::Token>(buf);
      assert(t->kind() == 42);
      assert(t->offset() == 1024);
      assert(t->length() == 15);
      assert(t->text() && std::string(t->text()->c_str()) == "identifier_name");
   });

   verify_roundtrip("UserProfile", g_user, [](const uint8_t* buf) {
      auto u = flatbuffers::GetRoot<fb::UserProfile>(buf);
      assert(u->id() == 123456789ULL);
      assert(u->name() && std::string(u->name()->c_str()) == "Alice Johnson");
      assert(u->email() && std::string(u->email()->c_str()) == "alice@example.com");
      assert(u->bio() != nullptr);
      assert(u->age() == 32);
      assert(u->score() == 98.5);
      assert(u->tags() && u->tags()->size() == 4);
      assert(u->verified() == true);
   });

   verify_roundtrip("Order", g_order, [](const uint8_t* buf) {
      auto o = flatbuffers::GetRoot<fb::Order>(buf);
      assert(o->id() == 987654321ULL);
      assert(o->customer() && o->customer()->name());
      assert(std::string(o->customer()->name()->c_str()) == "Alice Johnson");
      assert(o->items() && o->items()->size() == 5);
      assert(o->total() == 199.95);
      assert(o->note() && std::string(o->note()->c_str()) == "Please ship before Friday");
   });

   verify_roundtrip("SensorReading", g_sensor, [](const uint8_t* buf) {
      auto s = flatbuffers::GetRoot<fb::SensorReading>(buf);
      assert(s->timestamp() == 1700000000000ULL);
      assert(s->device_id() && std::string(s->device_id()->c_str()) == "sensor-alpha-42");
      assert(s->temp() == 23.5);
      assert(s->battery() == 3.7f);
      assert(s->signal_dbm() == -65);
      assert(s->firmware() && std::string(s->firmware()->c_str()) == "v2.3.1-rc4");
   });

   // Cross-verify: official output read by standalone reader
   std::printf("\n  (official FlatBuffers output read by standalone fb_unpack)\n\n");

   {
      flatbuffers::FlatBufferBuilder fbb(1024);
      fbb.Finish(build_point(fbb));
      auto p = psio::fb_unpack<BPoint>(fbb.GetBufferPointer());
      assert(p.x == 3.14159265358979);
      assert(p.y == 2.71828182845905);
      std::printf("  %-16s OK\n", "Point");
   }
   {
      flatbuffers::FlatBufferBuilder fbb(1024);
      fbb.Finish(build_token(fbb));
      auto t = psio::fb_unpack<Token>(fbb.GetBufferPointer());
      assert(t.kind == 42);
      assert(t.offset == 1024);
      assert(t.length == 15);
      assert(t.text == "identifier_name");
      std::printf("  %-16s OK\n", "Token");
   }
   {
      flatbuffers::FlatBufferBuilder fbb(1024);
      fbb.Finish(build_user(fbb));
      auto u = psio::fb_unpack<UserProfile>(fbb.GetBufferPointer());
      assert(u.id == 123456789ULL);
      assert(u.name == "Alice Johnson");
      assert(u.email == "alice@example.com");
      assert(u.age == 32);
      assert(u.score == 98.5);
      assert(u.tags.size() == 4);
      assert(u.verified == true);
      std::printf("  %-16s OK\n", "UserProfile");
   }
   {
      flatbuffers::FlatBufferBuilder fbb(1024);
      fbb.Finish(build_order(fbb));
      auto o = psio::fb_unpack<Order>(fbb.GetBufferPointer());
      assert(o.id == 987654321ULL);
      assert(o.customer.name == "Alice Johnson");
      assert(o.items.size() == 5);
      assert(o.items[0].product == "Product-0");
      assert(o.total == 199.95);
      assert(o.note && *o.note == "Please ship before Friday");
      std::printf("  %-16s OK\n", "Order");
   }
   {
      flatbuffers::FlatBufferBuilder fbb(1024);
      fbb.Finish(build_sensor(fbb));
      auto s = psio::fb_unpack<SensorReading>(fbb.GetBufferPointer());
      assert(s.timestamp == 1700000000000ULL);
      assert(s.device_id == "sensor-alpha-42");
      assert(s.temp == 23.5);
      assert(s.battery == 3.7f);
      assert(s.signal_dbm == -65);
      assert(s.firmware == "v2.3.1-rc4");
      std::printf("  %-16s OK\n", "SensorReading");
   }

   // Struct-as-field round-trip: BPoint embedded inline in Segment
   std::printf("\n  (struct-as-field: BPoint inline in Segment)\n\n");
   {
      Segment seg{{1.0, 2.0}, {3.0, 4.0}, "AB"};
      psio::fb_builder fbb;
      fbb.pack(seg);
      auto s = psio::fb_unpack<Segment>(fbb.data());
      assert(s.start.x == 1.0 && s.start.y == 2.0);
      assert(s.end.x == 3.0 && s.end.y == 4.0);
      assert(s.label == "AB");

      // View access
      auto v = psio::fb_view<Segment>::from_buffer(fbb.data());
      auto sp = v.start();
      auto ep = v.end();
      assert(sp.x == 1.0 && sp.y == 2.0);
      assert(ep.x == 3.0 && ep.y == 4.0);

      // Size check: inline structs save vtable overhead
      // Without struct: 2 sub-tables × (vtable + soffset + 2 offsets) ≈ +40B
      // With struct:    2 × 16B inline = 32B
      std::printf("  %-16s OK  (%zu bytes — BPoint fields are inline)\n",
                  "Segment", fbb.size());
   }

   // ── Enum round-trip ──────────────────────────────────────────────────
   std::printf("\n  (enum, array, union, file-id, size-prefix, verify)\n\n");
   {
      ColoredPoint cp{1.5, 2.5, Color::Blue};
      psio::fb_builder fbb;
      fbb.pack(cp);
      auto cp2 = psio::fb_unpack<ColoredPoint>(fbb.data());
      assert(cp2.x == 1.5);
      assert(cp2.y == 2.5);
      assert(cp2.color == Color::Blue);

      // Test enum reflection
      using R = psio::reflect<Color>;
      static_assert(R::is_enum);
      static_assert(R::count == 3);
      assert(R::to_string(Color::Red) == std::string_view("Red"));
      assert(R::to_string(Color::Green) == std::string_view("Green"));
      assert(R::to_string(Color::Blue) == std::string_view("Blue"));
      assert(R::from_string("Red") == Color::Red);
      assert(R::from_string("Green") == Color::Green);
      assert(R::from_string("Blue") == Color::Blue);
      assert(!R::from_string("Purple").has_value());
      assert(std::string_view(R::name) == "Color");
      std::printf("  %-16s OK  (enum round-trip + reflection, %zu bytes)\n", "Enum", fbb.size());
   }

   // ── Fixed-length array round-trip ──────────────────────────────────
   {
      Matrix2x2 m{{1.0f, 2.0f, 3.0f, 4.0f}, "identity"};
      psio::fb_builder fbb;
      fbb.pack(m);
      auto m2 = psio::fb_unpack<Matrix2x2>(fbb.data());
      assert(m2.data[0] == 1.0f && m2.data[1] == 2.0f);
      assert(m2.data[2] == 3.0f && m2.data[3] == 4.0f);
      assert(m2.label == "identity");
      std::printf("  %-16s OK  (std::array<float,4>, %zu bytes)\n", "Array", fbb.size());
   }

   // ── Union (variant) round-trip ─────────────────────────────────────
   {
      Shape s1{"circle", Circle{5.0}, Color::Red};
      psio::fb_builder fbb;
      fbb.pack(s1);
      auto s1r = psio::fb_unpack<Shape>(fbb.data());
      assert(s1r.name == "circle");
      assert(std::holds_alternative<Circle>(s1r.geom));
      assert(std::get<Circle>(s1r.geom).radius == 5.0);
      assert(s1r.color == Color::Red);
      std::printf("  %-16s OK  (variant<Circle>, %zu bytes)\n", "Union/Circle", fbb.size());

      fbb.clear();
      Shape s2{"rect", Rect{3.0, 4.0}, Color::Green};
      fbb.pack(s2);
      auto s2r = psio::fb_unpack<Shape>(fbb.data());
      assert(s2r.name == "rect");
      assert(std::holds_alternative<Rect>(s2r.geom));
      assert(std::get<Rect>(s2r.geom).width == 3.0);
      assert(std::get<Rect>(s2r.geom).height == 4.0);
      assert(s2r.color == Color::Green);
      std::printf("  %-16s OK  (variant<Rect>, %zu bytes)\n", "Union/Rect", fbb.size());
   }

   // ── File identifier ────────────────────────────────────────────────
   {
      psio::fb_builder fbb;
      fbb.pack(g_point, "BPNT");
      assert(psio::fb_has_identifier(fbb.data(), "BPNT"));
      assert(psio::fb_file_identifier(fbb.data()) == "BPNT");
      auto p2 = psio::fb_unpack<BPoint>(fbb.data());
      assert(p2.x == g_point.x && p2.y == g_point.y);
      std::printf("  %-16s OK  (file_identifier \"BPNT\")\n", "FileId");
   }

   // ── Size-prefixed buffer ───────────────────────────────────────────
   {
      psio::fb_builder fbb;
      fbb.pack_size_prefixed(g_point);
      auto len = psio::fb_size_prefixed_length(fbb.data());
      assert(len + 4 == fbb.size());
      auto p2 = psio::fb_unpack_size_prefixed<BPoint>(fbb.data());
      assert(p2.x == g_point.x && p2.y == g_point.y);
      std::printf("  %-16s OK  (size=%u, total=%zu bytes)\n",
                  "SizePrefix", len, fbb.size());
   }

   // ── Buffer verification ────────────────────────────────────────────
   {
      psio::fb_builder fbb;
      fbb.pack(g_order);
      assert(psio::fb_verify<Order>(fbb.data(), fbb.size()));

      // Truncated buffer should fail
      assert(!psio::fb_verify<Order>(fbb.data(), 3));
      assert(!psio::fb_verify<Order>(fbb.data(), 8));

      // Verify other types
      fbb.clear();
      fbb.pack(g_point);
      assert(psio::fb_verify<BPoint>(fbb.data(), fbb.size()));

      fbb.clear();
      fbb.pack(g_user);
      assert(psio::fb_verify<UserProfile>(fbb.data(), fbb.size()));

      std::printf("  %-16s OK  (valid buffers pass, truncated fail)\n", "Verify");
   }

   // ── Nested FlatBuffer round-trip ──────────────────────────────────
   {
      // Build the inner payload
      psio::fb_builder inner_fbb;
      inner_fbb.pack(g_order);
      std::vector<uint8_t> inner_bytes(inner_fbb.data(),
                                       inner_fbb.data() + inner_fbb.size());

      Envelope env{"order-envelope", psio::fb_nested<Order>(std::move(inner_bytes))};
      psio::fb_builder fbb;
      fbb.pack(env);

      // Unpack round-trip
      auto env2 = psio::fb_unpack<Envelope>(fbb.data());
      assert(env2.tag == "order-envelope");
      assert(env2.payload);
      auto inner_order = env2.payload.unpack();
      assert(inner_order.id == 987654321ULL);
      assert(inner_order.customer.name == "Alice Johnson");
      assert(inner_order.items.size() == 5);

      // View access via proxy (same interface as fb_view)
      auto v = psio::fb_view<Envelope>::from_buffer(fbb.data());
      auto tag_sv = v.tag();
      assert(std::string_view(tag_sv.data(), tag_sv.size()) == "order-envelope");

      // payload returns fb_view<Order> when accessed through view
      auto payload_view = v.payload();
      assert(payload_view.id() == 987654321ULL);
      auto cust_view = payload_view.customer();
      auto cname = cust_view.name();
      assert(std::string_view(cname.data(), cname.size()) == "Alice Johnson");

      // fb_nested proxy access (named fields on the nested object itself)
      assert(env2.payload.id() == 987654321ULL);
      auto cust_nested = env2.payload.customer();
      auto cname_n = cust_nested.name();
      assert(std::string_view(cname_n.data(), cname_n.size()) == "Alice Johnson");

      std::printf("  %-16s OK  (round-trip + view + proxy, %zu bytes)\n",
                  "Nested", fbb.size());
   }

   // ── In-place mutation (fb_mut) ──────────────────────────────────────
   {
      psio::fb_builder fbb;
      fbb.pack(g_user);
      // Get mutable buffer
      std::vector<uint8_t> buf(fbb.data(), fbb.data() + fbb.size());

      auto m = psio::fb_mut<UserProfile>::from_buffer(buf.data());
      // Read current values
      assert(static_cast<uint64_t>(m.id()) == 123456789ULL);
      assert(static_cast<uint32_t>(m.age()) == 32);
      assert(static_cast<double>(m.score()) == 98.5);
      assert(static_cast<bool>(m.verified()) == true);

      // Mutate scalars in-place
      m.id() = 999ULL;
      m.age() = 33;
      m.score() = 100.0;
      m.verified() = false;

      // Verify mutations via unpack
      auto u = psio::fb_unpack<UserProfile>(buf.data());
      assert(u.id == 999ULL);
      assert(u.age == 33);
      assert(u.score == 100.0);
      assert(u.verified == false);
      // Strings unchanged
      assert(u.name == "Alice Johnson");
      assert(u.email == "alice@example.com");

      std::printf("  %-16s OK  (scalar mutation in-place)\n", "fb_mut");
   }

   // ── Nested mutation (fb_mut on sub-table) ────────────────────────
   {
      psio::fb_builder fbb;
      fbb.pack(g_order);
      std::vector<uint8_t> buf(fbb.data(), fbb.data() + fbb.size());

      auto m = psio::fb_mut<Order>::from_buffer(buf.data());
      m.id() = 111ULL;
      m.total() = 42.0;

      // Mutate nested table's scalar
      auto cust = m.customer();
      cust.age() = 99;
      cust.verified() = false;

      auto o = psio::fb_unpack<Order>(buf.data());
      assert(o.id == 111ULL);
      assert(o.total == 42.0);
      assert(o.customer.age == 99);
      assert(o.customer.verified == false);
      assert(o.customer.name == "Alice Johnson");  // unchanged

      std::printf("  %-16s OK  (nested scalar mutation)\n", "fb_mut/nested");
   }

   // ── Enum mutation ─────────────────────────────────────────────────
   {
      ColoredPoint cp{1.5, 2.5, Color::Blue};
      psio::fb_builder fbb;
      fbb.pack(cp);
      std::vector<uint8_t> buf(fbb.data(), fbb.data() + fbb.size());

      auto m = psio::fb_mut<ColoredPoint>::from_buffer(buf.data());
      assert(static_cast<Color>(m.color()) == Color::Blue);
      m.color() = Color::Red;

      auto cp2 = psio::fb_unpack<ColoredPoint>(fbb.data());
      // Verify from mutated buffer
      auto cp3 = psio::fb_unpack<ColoredPoint>(buf.data());
      assert(cp3.color == Color::Red);
      assert(cp3.x == 1.5);

      std::printf("  %-16s OK  (enum mutation in-place)\n", "fb_mut/enum");
   }

   // ── fb_doc: O(1) string mutation + compact ────────────────────────
   {
      auto doc = psio::fb_doc<UserProfile>::from_value(g_user);
      size_t orig_size = doc.size();

      // Read via named accessors
      assert(static_cast<uint64_t>(doc.id()) == 123456789ULL);
      assert(std::string_view(doc.name()) == "Alice Johnson");
      assert(std::string_view(doc.email()) == "alice@example.com");
      assert(static_cast<uint32_t>(doc.age()) == 32);

      // Scalar mutation (in-place, same as fb_mut)
      doc.id() = 999ULL;
      doc.age() = 33;

      // String mutation (O(1) append + offset update)
      doc.name() = "Bob";
      doc.email() = "bob@example.com";

      size_t after_mut = doc.size();
      assert(after_mut > orig_size);  // buffer grew (dead space)

      // Verify mutations
      auto u = doc.unpack();
      assert(u.id == 999ULL);
      assert(u.age == 33);
      assert(u.name == "Bob");
      assert(u.email == "bob@example.com");
      assert(u.score == 98.5);  // unchanged
      assert(u.tags.size() == 4);  // unchanged

      // Compact: rebuild with no dead space
      doc.compact();
      assert(doc.size() <= orig_size);  // back to minimal

      // Verify still correct after compact
      auto u2 = doc.unpack();
      assert(u2.id == 999ULL);
      assert(u2.name == "Bob");
      assert(u2.email == "bob@example.com");

      std::printf("  %-16s OK  (string mutation + compact, %zu → %zu → %zu bytes)\n",
                  "fb_doc", orig_size, after_mut, doc.size());
   }

   // ── fb_doc: nested sub-table string mutation ──────────────────────
   {
      auto doc = psio::fb_doc<Order>::from_value(g_order);

      // Mutate nested table's string
      doc.customer().name() = "Charlie";
      doc.customer().email() = "charlie@example.com";
      doc.total() = 42.0;

      auto o = doc.unpack();
      assert(o.customer.name == "Charlie");
      assert(o.customer.email == "charlie@example.com");
      assert(o.total == 42.0);
      assert(o.id == 987654321ULL);  // unchanged
      assert(o.items.size() == 5);   // unchanged

      doc.compact();
      auto o2 = doc.unpack();
      assert(o2.customer.name == "Charlie");
      assert(o2.customer.email == "charlie@example.com");

      std::printf("  %-16s OK  (nested string mutation + compact)\n", "fb_doc/nested");
   }

   // ── fb_doc: copy and move semantics ───────────────────────────────
   {
      auto doc = psio::fb_doc<UserProfile>::from_value(g_user);
      doc.name() = "Original";

      // Copy
      auto copy = doc;
      copy.name() = "Copy";
      assert(std::string_view(doc.name()) == "Original");  // original unchanged
      assert(std::string_view(copy.name()) == "Copy");

      // Move
      auto moved = std::move(copy);
      assert(std::string_view(moved.name()) == "Copy");

      std::printf("  %-16s OK  (copy/move semantics)\n", "fb_doc/copy");
   }

   // ── Struct defaults: builder omits, reader restores ──────────────────
   {
      // Pack a Config with all-default values
      Config all_defaults{};  // {30, 1.0f, true, "", 3}
      psio::fb_builder fbb_def;
      fbb_def.pack(all_defaults);

      // All scalar fields match defaults → should be omitted from vtable,
      // producing a smaller buffer than if they were all written
      size_t default_sz = fbb_def.size();

      // Pack one with non-default values
      Config custom{.timeout = 60, .scale = 2.5f, .enabled = false, .label = "custom", .retries = 10};
      psio::fb_builder fbb_cust;
      fbb_cust.pack(custom);
      size_t custom_sz = fbb_cust.size();

      // The all-defaults buffer should be smaller (no scalar data written)
      assert(default_sz < custom_sz);

      // Unpack all-defaults: should recover the struct defaults, not zeros
      auto unpacked = psio::fb_unpack<Config>(fbb_def.data());
      assert(unpacked.timeout == 30);
      assert(unpacked.scale == 1.0f);
      assert(unpacked.enabled == true);
      assert(unpacked.label.empty());
      assert(unpacked.retries == 3);

      // View all-defaults: same
      auto v = psio::fb_view<Config>::from_buffer(fbb_def.data());
      assert(v.timeout() == 30);
      assert(v.scale() == 1.0f);
      assert(v.enabled() == true);
      assert(v.retries() == 3);

      // Unpack custom: should get custom values
      auto unpacked2 = psio::fb_unpack<Config>(fbb_cust.data());
      assert(unpacked2.timeout == 60);
      assert(unpacked2.scale == 2.5f);
      assert(unpacked2.enabled == false);
      assert(unpacked2.label == "custom");
      assert(unpacked2.retries == 10);

      // Round-trip: pack default → unpack → pack again → same bytes
      psio::fb_builder fbb_rt;
      fbb_rt.pack(unpacked);
      assert(fbb_rt.size() == default_sz);
      assert(std::memcmp(fbb_rt.data(), fbb_def.data(), default_sz) == 0);

      std::printf("  %-16s OK  (default_sz=%zu, custom_sz=%zu)\n",
                  "defaults", default_sz, custom_sz);
   }

   // ── flat_set and flat_map ─────────────────────────────────────────────
   {
      Inventory inv;
      inv.item_ids.insert(100);
      inv.item_ids.insert(42);
      inv.item_ids.insert(7);
      inv.item_ids.insert(200);
      inv.counts["apples"]  = 10;
      inv.counts["bananas"] = 25;
      inv.counts["cherries"] = 3;
      inv.labels[1] = "warehouse-A";
      inv.labels[2] = "warehouse-B";
      inv.labels[5] = "cold-storage";

      // Pack
      psio::fb_builder fbb;
      fbb.pack(inv);

      // Unpack round-trip
      auto inv2 = psio::fb_unpack<Inventory>(fbb.data());
      assert(inv2.item_ids.size() == 4);
      assert(inv2.item_ids.contains(7));
      assert(inv2.item_ids.contains(42));
      assert(inv2.item_ids.contains(100));
      assert(inv2.item_ids.contains(200));

      assert(inv2.counts.size() == 3);
      assert(inv2.counts["apples"] == 10);
      assert(inv2.counts["bananas"] == 25);
      assert(inv2.counts["cherries"] == 3);

      assert(inv2.labels.size() == 3);
      assert(inv2.labels[1] == "warehouse-A");
      assert(inv2.labels[2] == "warehouse-B");
      assert(inv2.labels[5] == "cold-storage");

      // View: fb_sorted_vec (flat_set)
      auto v = psio::fb_view<Inventory>::from_buffer(fbb.data());
      auto ids = v.item_ids();
      assert(ids.size() == 4);
      assert(ids.contains(42));
      assert(ids.contains(7));
      assert(ids.contains(200));
      assert(!ids.contains(999));
      assert(ids.find(100) < ids.size());
      assert(ids.find(999) == ids.size());
      // Verify sorted order
      assert(ids[0] == 7);
      assert(ids[1] == 42);
      assert(ids[2] == 100);
      assert(ids[3] == 200);

      // View: fb_sorted_map (string→int)
      auto cnts = v.counts();
      assert(cnts.size() == 3);
      assert(cnts.contains("apples"));
      assert(cnts.contains("bananas"));
      assert(!cnts.contains("dates"));
      auto e = cnts.find("cherries");
      assert(e.idx_ < cnts.size());
      assert(e.value() == 3);

      // View: fb_sorted_map (int→string)
      auto lbls = v.labels();
      assert(lbls.size() == 3);
      assert(lbls.contains(1));
      assert(lbls.contains(5));
      assert(!lbls.contains(99));
      auto lbl = lbls.find(2);
      assert(lbl.idx_ < lbls.size());
      assert(lbl.value() == "warehouse-B");

      std::printf("  %-16s OK  (%zu bytes, set=%zu, map1=%zu, map2=%zu)\n",
                  "flat_set/map", fbb.size(), inv2.item_ids.size(),
                  inv2.counts.size(), inv2.labels.size());
   }

   // ── flat_set/map schema export ────────────────────────────────────────
   {
      std::string schema = psio::to_fbs_schema<Inventory>();
      // Verify the schema contains expected patterns
      assert(schema.find("[int]") != std::string::npos);      // flat_set<int32_t>
      assert(schema.find("(key)") != std::string::npos);       // key attribute
      assert(schema.find("_entry") != std::string::npos);      // entry table
      std::printf("  %-16s OK  (schema exports correctly)\n", "flat_set/schema");
   }

   std::printf("\n  All cross-format verifications passed.\n");
}

// ── Wire size comparison ─────────────────────────────────────────────────────

void compare_wire_sizes()
{
   std::printf("\n=== Wire Size Comparison ===\n");
   std::printf("  %-20s %10s %10s %10s\n", "Type", "No-Dedup", "Dedup", "Official");
   std::printf("  %s\n", std::string(54, '-').c_str());

   auto sizes = [](const char* name, auto& value, auto build_fn) {
      psio::fb_builder sb;
      sb.pack(value);
      size_t ssz = sb.size();

      psio::basic_fb_builder<psio::fb_dedup::on> sbd;
      sbd.pack(value);
      size_t dsz = sbd.size();

      flatbuffers::FlatBufferBuilder ofb(1024);
      auto                          root = build_fn(ofb);
      ofb.Finish(root);
      size_t osz = ofb.GetSize();

      std::printf("  %-20s %7zu B  %7zu B  %7zu B  %s\n", name, ssz, dsz, osz,
                  dsz == osz ? "(dedup matches)" : (dsz < osz ? "(dedup smaller)" : ""));
   };

   sizes("Point", g_point, build_point);
   sizes("Token", g_token, build_token);
   sizes("UserProfile", g_user, build_user);
   sizes("Order", g_order, build_order);
   sizes("SensorReading", g_sensor, build_sensor);
}

// ── Pre-built buffers ────────────────────────────────────────────────────────

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

// ── Pack benchmarks ──────────────────────────────────────────────────────────

void bench_pack()
{
   print_header("Pack: Standalone (psio::fb_builder)");

   auto standalone_pack = [](const char* name, const auto& value) {
      psio::fb_builder tmp;
      tmp.pack(value);
      size_t sz = tmp.size();

      std::string bname = std::string("standalone/pack/") + name;
      bench(bname.c_str(), sz, [&] {
         psio::fb_builder fbb(1024);
         fbb.pack(value);
         do_not_optimize(fbb.data());
      });
   };

   standalone_pack("Point", g_point);
   standalone_pack("Token", g_token);
   standalone_pack("UserProfile", g_user);
   standalone_pack("Order", g_order);
   standalone_pack("SensorReading", g_sensor);

   print_header("Pack: Standalone + Dedup (basic_fb_builder<fb_dedup::on>)");

   auto dedup_pack = [](const char* name, const auto& value) {
      psio::basic_fb_builder<psio::fb_dedup::on> tmp;
      tmp.pack(value);
      size_t sz = tmp.size();

      std::string bname = std::string("standalone-dedup/pack/") + name;
      bench(bname.c_str(), sz, [&] {
         psio::basic_fb_builder<psio::fb_dedup::on> fbb(1024);
         fbb.pack(value);
         do_not_optimize(fbb.data());
      });
   };

   dedup_pack("Point", g_point);
   dedup_pack("Token", g_token);
   dedup_pack("UserProfile", g_user);
   dedup_pack("Order", g_order);
   dedup_pack("SensorReading", g_sensor);

   print_header("Pack: Official FlatBuffers");

   auto official_pack = [](const char* name, auto build_fn) {
      flatbuffers::FlatBufferBuilder tmp(1024);
      auto                          root = build_fn(tmp);
      tmp.Finish(root);
      size_t sz = tmp.GetSize();

      std::string bname = std::string("official/pack/") + name;
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

void bench_unpack()
{
   print_header("Unpack: Standalone (psio::fb_unpack)");

   bench("standalone/unpack/Point", g_pre.point.size(), [&] {
      auto p = psio::fb_unpack<BPoint>(g_pre.point.data());
      do_not_optimize(p.x);
      do_not_optimize(p.y);
   });
   bench("standalone/unpack/Token", g_pre.token.size(), [&] {
      auto t = psio::fb_unpack<Token>(g_pre.token.data());
      do_not_optimize(t.kind);
      do_not_optimize(t.text.data());
   });
   bench("standalone/unpack/UserProfile", g_pre.user.size(), [&] {
      auto u = psio::fb_unpack<UserProfile>(g_pre.user.data());
      do_not_optimize(u.id);
      do_not_optimize(u.name.data());
      do_not_optimize(u.tags.size());
   });
   bench("standalone/unpack/Order", g_pre.order.size(), [&] {
      auto o = psio::fb_unpack<Order>(g_pre.order.data());
      do_not_optimize(o.id);
      do_not_optimize(o.customer.name.data());
      do_not_optimize(o.items.size());
   });
   bench("standalone/unpack/SensorReading", g_pre.sensor.size(), [&] {
      auto s = psio::fb_unpack<SensorReading>(g_pre.sensor.data());
      do_not_optimize(s.timestamp);
      do_not_optimize(s.device_id.data());
      do_not_optimize(s.firmware.data());
   });

   print_header("Unpack: Official FlatBuffers (UnPack)");

   bench("official/unpack/Point", g_pre.point.size(), [&] {
      auto p = flatbuffers::GetRoot<fb::Point>(g_pre.point.data())->UnPack();
      do_not_optimize(p->x);
      do_not_optimize(p->y);
      delete p;
   });
   bench("official/unpack/Token", g_pre.token.size(), [&] {
      auto t = flatbuffers::GetRoot<fb::Token>(g_pre.token.data())->UnPack();
      do_not_optimize(t->kind);
      do_not_optimize(t->text.data());
      delete t;
   });
   bench("official/unpack/UserProfile", g_pre.user.size(), [&] {
      auto u = flatbuffers::GetRoot<fb::UserProfile>(g_pre.user.data())->UnPack();
      do_not_optimize(u->id);
      do_not_optimize(u->name.data());
      do_not_optimize(u->tags.size());
      delete u;
   });
   bench("official/unpack/Order", g_pre.order.size(), [&] {
      auto o = flatbuffers::GetRoot<fb::Order>(g_pre.order.data())->UnPack();
      do_not_optimize(o->id);
      do_not_optimize(o->customer->name.data());
      do_not_optimize(o->items.size());
      delete o;
   });
   bench("official/unpack/SensorReading", g_pre.sensor.size(), [&] {
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
   print_header("View: Standalone (psio::fb_view)");

   bench("standalone/view/Point", g_pre.point.size(), [&] {
      auto v = psio::fb_view<BPoint>::from_buffer(g_pre.point.data());
      do_not_optimize(v.x());
      do_not_optimize(v.y());
   });
   bench("standalone/view/Token", g_pre.token.size(), [&] {
      auto v    = psio::fb_view<Token>::from_buffer(g_pre.token.data());
      auto kind = v.kind();
      auto off  = v.offset();
      auto len  = v.length();
      auto text = v.text();
      do_not_optimize(kind);
      do_not_optimize(off);
      do_not_optimize(len);
      do_not_optimize(text.data());
   });
   bench("standalone/view/UserProfile", g_pre.user.size(), [&] {
      auto v        = psio::fb_view<UserProfile>::from_buffer(g_pre.user.data());
      auto id       = v.id();
      auto name     = v.name();
      auto email    = v.email();
      auto bio      = v.bio();
      auto age      = v.age();
      auto score    = v.score();
      auto tags     = v.tags();
      auto verified = v.verified();
      do_not_optimize(id);
      do_not_optimize(name.data());
      do_not_optimize(email.data());
      do_not_optimize(bio.data());
      do_not_optimize(age);
      do_not_optimize(score);
      do_not_optimize(tags.size());
      do_not_optimize(verified);
   });
   bench("standalone/view/Order", g_pre.order.size(), [&] {
      auto v     = psio::fb_view<Order>::from_buffer(g_pre.order.data());
      auto id    = v.id();
      auto cust  = v.customer();
      auto cname = cust.name();
      auto items = v.items();
      auto total = v.total();
      auto note  = v.note();
      do_not_optimize(id);
      do_not_optimize(cname.data());
      do_not_optimize(items.size());
      do_not_optimize(total);
      do_not_optimize(note.data());
   });
   bench("standalone/view/SensorReading", g_pre.sensor.size(), [&] {
      auto v   = psio::fb_view<SensorReading>::from_buffer(g_pre.sensor.data());
      auto ts  = v.timestamp();
      auto did = v.device_id();
      auto tmp = v.temp();
      auto hum = v.humidity();
      auto prs = v.pressure();
      auto ax  = v.accel_x();
      auto ay  = v.accel_y();
      auto az  = v.accel_z();
      auto gx  = v.gyro_x();
      auto gy  = v.gyro_y();
      auto gz  = v.gyro_z();
      auto mx  = v.mag_x();
      auto my  = v.mag_y();
      auto mz  = v.mag_z();
      auto bat = v.battery();
      auto sig = v.signal_dbm();
      auto ec  = v.error_code();
      auto fw  = v.firmware();
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
   });

   print_header("View: Official FlatBuffers");

   bench("official/view/Point", g_pre.point.size(), [&] {
      auto p = flatbuffers::GetRoot<fb::Point>(g_pre.point.data());
      do_not_optimize(p->x());
      do_not_optimize(p->y());
   });
   bench("official/view/Token", g_pre.token.size(), [&] {
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
   bench("official/view/UserProfile", g_pre.user.size(), [&] {
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
   bench("official/view/Order", g_pre.order.size(), [&] {
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
   bench("official/view/SensorReading", g_pre.sensor.size(), [&] {
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
   std::printf("Standalone FlatBuffer Benchmark\n");
   std::printf("================================\n");
   std::printf("psio::fb_builder / fb_unpack / fb_view  (zero dependency)\n");
   std::printf("vs official FlatBuffers library\n");

   verify_wire_compat();
   compare_wire_sizes();
   bench_pack();
   bench_unpack();
   bench_view();

   // Schema export demo
   std::printf("\n=== Generated FlatBuffer Schema (Order) ===\n");
   std::printf("%s", psio::to_fbs_schema<Order>().c_str());

   std::printf("\n=== Generated FlatBuffer Schema (Shape — union) ===\n");
   std::printf("%s", psio::to_fbs_schema<Shape>().c_str());

   std::printf("\n=== Generated FlatBuffer Schema (Segment — struct) ===\n");
   std::printf("%s", psio::to_fbs_schema<Segment>().c_str());

   std::printf("\n=== Generated FlatBuffer Schema (Envelope — nested) ===\n");
   std::printf("%s", psio::to_fbs_schema<Envelope>().c_str());

   std::printf("\n=== Generated FlatBuffer Schema (Inventory — flat_set/map) ===\n");
   std::printf("%s", psio::to_fbs_schema<Inventory>().c_str());

   // Summary
   std::printf("\n=== Head-to-Head Summary ===\n");
   std::printf("  %-20s %10s %10s %10s\n", "", "Pack", "Unpack", "View");
   std::printf("  %s\n", std::string(52, '-').c_str());

   auto find = [](const char* prefix) -> std::vector<BenchResult*> {
      std::vector<BenchResult*> out;
      for (auto& r : g_results)
         if (r.name.substr(0, strlen(prefix)) == prefix)
            out.push_back(&r);
      return out;
   };

   auto sp = find("standalone/pack/");
   auto op = find("official/pack/");
   auto su = find("standalone/unpack/");
   auto ou = find("official/unpack/");
   auto sv = find("standalone/view/");
   auto ov = find("official/view/");

   const char* names[] = {"Point", "Token", "UserProfile", "Order", "SensorReading"};
   for (int i = 0; i < 5; ++i)
   {
      double pr = (i < (int)sp.size() && i < (int)op.size()) ? sp[i]->mean_ns / op[i]->mean_ns
                                                              : 0;
      double ur = (i < (int)su.size() && i < (int)ou.size()) ? su[i]->mean_ns / ou[i]->mean_ns
                                                              : 0;
      double vr = (i < (int)sv.size() && i < (int)ov.size()) ? sv[i]->mean_ns / ov[i]->mean_ns
                                                              : 0;
      std::printf("  %-20s %8.2fx   %8.2fx   %8.2fx\n", names[i], pr, ur, vr);
   }
   std::printf("\n  Ratio: <1.0 = standalone faster, >1.0 = official faster\n");

   return 0;
}
