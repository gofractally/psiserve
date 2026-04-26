// FlatBuffers benchmark — same schemas and data as bench_fracpack.cpp
//
// Measures: pack (build + finish), view (read fields zero-copy), and wire size.
// FlatBuffers doesn't have a traditional "unpack" — its API is zero-copy like Cap'n Proto.

#include "benchmarks/bench_schemas_generated.h"

#include <flatbuffers/flatbuffers.h>

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

   g_results.push_back(
       {name, ops_per_sec, mean_ns_f, data_bytes, static_cast<size_t>(total_ops)});

   if (mean_ns_f < 10.0)
      std::printf("  %-50s %12" PRIu64 " ops/s  %6.1f ns  %6zu B  (%.0f ops)\n", name,
                  ops_per_sec, mean_ns_f, data_bytes, total_ops);
   else
      std::printf("  %-50s %12" PRIu64 " ops/s  %6.0f ns  %6zu B  (%.0f ops)\n", name,
                  ops_per_sec, mean_ns_f, data_bytes, total_ops);
}

void print_header(const char* group)
{
   std::printf("\n=== %s ===\n", group);
   std::printf("  %-50s %12s  %8s  %6s\n", "Benchmark", "Throughput", "Latency", "Bytes");
   std::printf("  %s\n", std::string(50 + 12 + 8 + 6 + 10, '-').c_str());
}

// ── Helpers to build FlatBuffers messages ───────────────────────────────────

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
   return fb::CreateSensorReading(fbb, 1700000000000ULL, device_id, 23.5, 65.2, 1013.25,
                                     0.01, -0.02, 9.81, 0.001, -0.003, 0.002, 25.1, -12.3,
                                     42.7, 3.7f, -65, 0, firmware);
}

// ── Pack benchmarks ─────────────────────────────────────────────────────────

void bench_flatbuf_pack()
{
   print_header("FlatBuffers: Pack (build + finish)");

   // Print wire sizes
   auto print_size = [](const char* name, auto build_fn) {
      flatbuffers::FlatBufferBuilder fbb(1024);
      auto                          root = build_fn(fbb);
      fbb.Finish(root);
      std::printf("  %s wire size: %zu B\n", name, static_cast<size_t>(fbb.GetSize()));
   };
   print_size("Point", build_point);
   print_size("Token", build_token);
   print_size("UserProfile", build_user);
   print_size("Order", build_order);
   print_size("SensorReading", build_sensor);

   // Pack: build + finish (FlatBufferBuilder manages the buffer)
   auto bench_pack = [](const char* name, auto build_fn) {
      // Get size for reporting
      flatbuffers::FlatBufferBuilder tmp(1024);
      auto                          root = build_fn(tmp);
      tmp.Finish(root);
      size_t sz = tmp.GetSize();

      std::string bname = std::string("flatbuf-pack/") + name;
      bench(bname.c_str(), sz, [&] {
         flatbuffers::FlatBufferBuilder fbb(1024);
         auto                          r = build_fn(fbb);
         fbb.Finish(r);
         do_not_optimize(fbb.GetBufferPointer());
         return fbb.GetSize();
      });
   };

   bench_pack("Point", build_point);
   bench_pack("Token", build_token);
   bench_pack("UserProfile", build_user);
   bench_pack("Order", build_order);
   bench_pack("SensorReading", build_sensor);
}

// ── Unpack benchmarks (deserialize to native structs) ───────────────────────

void bench_flatbuf_unpack()
{
   print_header("FlatBuffers: Unpack (deserialize to native T types)");

   // Point
   {
      flatbuffers::FlatBufferBuilder fbb(1024);
      fbb.Finish(build_point(fbb));
      auto   buf = fbb.GetBufferPointer();
      size_t sz  = fbb.GetSize();
      bench("flatbuf-unpack/Point", sz, [&] {
         auto p = flatbuffers::GetRoot<fb::Point>(buf)->UnPack();
         do_not_optimize(p->x);
         do_not_optimize(p->y);
         delete p;
      });
   }

   // Token
   {
      flatbuffers::FlatBufferBuilder fbb(1024);
      fbb.Finish(build_token(fbb));
      auto   buf = fbb.GetBufferPointer();
      size_t sz  = fbb.GetSize();
      bench("flatbuf-unpack/Token", sz, [&] {
         auto t = flatbuffers::GetRoot<fb::Token>(buf)->UnPack();
         do_not_optimize(t->kind);
         do_not_optimize(t->text.data());
         delete t;
      });
   }

   // UserProfile
   {
      flatbuffers::FlatBufferBuilder fbb(1024);
      fbb.Finish(build_user(fbb));
      auto   buf = fbb.GetBufferPointer();
      size_t sz  = fbb.GetSize();
      bench("flatbuf-unpack/UserProfile", sz, [&] {
         auto u = flatbuffers::GetRoot<fb::UserProfile>(buf)->UnPack();
         do_not_optimize(u->id);
         do_not_optimize(u->name.data());
         do_not_optimize(u->tags.size());
         delete u;
      });
   }

   // Order
   {
      flatbuffers::FlatBufferBuilder fbb(1024);
      fbb.Finish(build_order(fbb));
      auto   buf = fbb.GetBufferPointer();
      size_t sz  = fbb.GetSize();
      bench("flatbuf-unpack/Order", sz, [&] {
         auto o = flatbuffers::GetRoot<fb::Order>(buf)->UnPack();
         do_not_optimize(o->id);
         do_not_optimize(o->customer->name.data());
         do_not_optimize(o->items.size());
         delete o;
      });
   }

   // SensorReading
   {
      flatbuffers::FlatBufferBuilder fbb(1024);
      fbb.Finish(build_sensor(fbb));
      auto   buf = fbb.GetBufferPointer();
      size_t sz  = fbb.GetSize();
      bench("flatbuf-unpack/SensorReading", sz, [&] {
         auto s = flatbuffers::GetRoot<fb::SensorReading>(buf)->UnPack();
         do_not_optimize(s->timestamp);
         do_not_optimize(s->device_id.data());
         do_not_optimize(s->firmware.data());
         delete s;
      });
   }
}

// ── View benchmarks (zero-copy read) ────────────────────────────────────────

void bench_flatbuf_view()
{
   print_header("FlatBuffers: View-All (read all fields zero-copy)");

   // Point
   {
      flatbuffers::FlatBufferBuilder fbb(1024);
      fbb.Finish(build_point(fbb));
      auto   buf = fbb.GetBufferPointer();
      size_t sz  = fbb.GetSize();

      bench("flatbuf-view-all/Point", sz, [&] {
         auto p = flatbuffers::GetRoot<fb::Point>(buf);
         auto x = p->x();
         auto y = p->y();
         do_not_optimize(x);
         do_not_optimize(y);
         return x + y;
      });
   }

   // Token
   {
      flatbuffers::FlatBufferBuilder fbb(1024);
      fbb.Finish(build_token(fbb));
      auto   buf = fbb.GetBufferPointer();
      size_t sz  = fbb.GetSize();

      bench("flatbuf-view-all/Token", sz, [&] {
         auto t    = flatbuffers::GetRoot<fb::Token>(buf);
         auto kind = t->kind();
         auto off  = t->offset();
         auto len  = t->length();
         auto text = t->text();
         do_not_optimize(kind);
         do_not_optimize(off);
         do_not_optimize(len);
         do_not_optimize(text->data());
         return kind;
      });
   }

   // UserProfile
   {
      flatbuffers::FlatBufferBuilder fbb(1024);
      fbb.Finish(build_user(fbb));
      auto   buf = fbb.GetBufferPointer();
      size_t sz  = fbb.GetSize();

      bench("flatbuf-view-all/UserProfile", sz, [&] {
         auto u        = flatbuffers::GetRoot<fb::UserProfile>(buf);
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
         return id;
      });
   }

   // Order
   {
      flatbuffers::FlatBufferBuilder fbb(1024);
      fbb.Finish(build_order(fbb));
      auto   buf = fbb.GetBufferPointer();
      size_t sz  = fbb.GetSize();

      bench("flatbuf-view-all/Order", sz, [&] {
         auto o     = flatbuffers::GetRoot<fb::Order>(buf);
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
         return id;
      });
   }

   // SensorReading
   {
      flatbuffers::FlatBufferBuilder fbb(1024);
      fbb.Finish(build_sensor(fbb));
      auto   buf = fbb.GetBufferPointer();
      size_t sz  = fbb.GetSize();

      bench("flatbuf-view-all/SensorReading", sz, [&] {
         auto s   = flatbuffers::GetRoot<fb::SensorReading>(buf);
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
         return ts;
      });
   }
}

void bench_flatbuf_view_one()
{
   print_header("FlatBuffers: View-One (single field from serialized)");

   // UserProfile.name
   {
      flatbuffers::FlatBufferBuilder fbb(1024);
      fbb.Finish(build_user(fbb));
      auto   buf = fbb.GetBufferPointer();
      size_t sz  = fbb.GetSize();

      bench("flatbuf-view-one/UserProfile.name", sz, [&] {
         auto u    = flatbuffers::GetRoot<fb::UserProfile>(buf);
         auto name = u->name();
         do_not_optimize(name->data());
         return name->size();
      });
   }

   // UserProfile.id
   {
      flatbuffers::FlatBufferBuilder fbb(1024);
      fbb.Finish(build_user(fbb));
      auto   buf = fbb.GetBufferPointer();
      size_t sz  = fbb.GetSize();

      bench("flatbuf-view-one/UserProfile.id", sz, [&] {
         auto u  = flatbuffers::GetRoot<fb::UserProfile>(buf);
         auto id = u->id();
         do_not_optimize(id);
         return id;
      });
   }

   // Order.customer.name
   {
      flatbuffers::FlatBufferBuilder fbb(1024);
      fbb.Finish(build_order(fbb));
      auto   buf = fbb.GetBufferPointer();
      size_t sz  = fbb.GetSize();

      bench("flatbuf-view-one/Order.customer.name", sz, [&] {
         auto o    = flatbuffers::GetRoot<fb::Order>(buf);
         auto name = o->customer()->name();
         do_not_optimize(name->data());
         return name->size();
      });
   }
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main()
{
   std::printf("FlatBuffers Benchmark (comparison with psio formats)\n");
   std::printf("=====================================================\n");

   bench_flatbuf_pack();
   bench_flatbuf_unpack();
   bench_flatbuf_view();
   bench_flatbuf_view_one();

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
