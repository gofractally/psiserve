// Apples-to-apples format perf comparison: json, msgpack, and pjson
// all driven by the same reflected struct via their typed CPO /
// from_struct entry points.  pjson now supports vector + optional
// fields in pjson_typed (typed-array fast path for primitive
// element types, t_null tag for absent optionals), so the same Bag
// shape exercises every format.
//
// Build via psio3_format_perf target.  Output reports ns/op for
// encode + decode + round-trip on each format, plus encoded size.
//
// Disclaimer: simple steady-state microbenchmark.  Treat as
// rule-of-thumb relative numbers, not absolute throughput claims.

#include <psio/json.hpp>
#include <psio/msgpack.hpp>
#include <psio/pjson_typed.hpp>
#include <psio/pjson_view.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace bench
{
   //  A small reflected sub-record so we can exercise vector<substruct>.
   struct Sub
   {
      std::int32_t id;
      std::string  label;
   };
   PSIO_REFLECT(Sub, id, label)

   struct Bag
   {
      std::string                 name;
      std::vector<std::uint8_t>   payload;     // bytes-blob path
      std::vector<std::int32_t>   ids;         // typed-array (msgpack:
                                               // array-of-i32; pjson:
                                               // typed-array fast path)
      std::vector<Sub>            entries;     // generic array of records
      std::optional<std::int32_t> count;
      std::int64_t                seq;
      double                      score;
   };
   PSIO_REFLECT(Bag, name, payload, ids, entries, count, seq, score)

   inline Bag make_sample()
   {
      return Bag{
         .name    = std::string{"alice-the-quick-brown-fox"},
         .payload = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
         .ids     = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
         .entries = {Sub{1, std::string{"alpha"}},
                     Sub{2, std::string{"beta"}},
                     Sub{3, std::string{"gamma"}},
                     Sub{4, std::string{"delta"}}},
         .count   = 42,
         .seq     = 123456789012LL,
         .score   = 3.14159265358979};
   }
}  // namespace bench

using clk = std::chrono::steady_clock;

template <typename F>
double bench_ns(F&& f, std::size_t iters)
{
   auto t0 = clk::now();
   for (std::size_t i = 0; i < iters; ++i)
      f();
   auto t1 = clk::now();
   auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                .count();
   return static_cast<double>(ns) / static_cast<double>(iters);
}

int main()
{
   constexpr std::size_t N = 200'000;

   bench::Bag sample = bench::make_sample();

   // Pre-encode once for size reporting + decode benches.
   auto json_bytes    = psio::encode(psio::json{}, sample);
   auto msgpack_bytes = psio::encode(psio::msgpack{}, sample);

   std::printf("== Format wire size for sample ==========================\n");
   std::printf("  json     : %zu bytes\n", json_bytes.size());
   std::printf("  msgpack  : %zu bytes  (%.2fx vs json)\n",
               msgpack_bytes.size(),
               static_cast<double>(msgpack_bytes.size()) /
                  static_cast<double>(json_bytes.size()));

   // ── encode ──────────────────────────────────────────────────────
   auto enc_json = bench_ns(
      [&] {
         auto out = psio::encode(psio::json{}, sample);
         asm volatile("" : : "r"(out.data()) : "memory");
      },
      N);
   auto enc_mp = bench_ns(
      [&] {
         auto out = psio::encode(psio::msgpack{}, sample);
         asm volatile("" : : "r"(out.data()) : "memory");
      },
      N);

   // ── decode ──────────────────────────────────────────────────────
   auto dec_json = bench_ns(
      [&] {
         auto out = psio::decode<bench::Bag>(
            psio::json{}, std::span<const char>{json_bytes});
         asm volatile("" : : "r"(&out) : "memory");
      },
      N);
   auto dec_mp = bench_ns(
      [&] {
         auto out = psio::decode<bench::Bag>(
            psio::msgpack{}, std::span<const char>{msgpack_bytes});
         asm volatile("" : : "r"(&out) : "memory");
      },
      N);

   std::printf(
      "\n== ns/op  (lower is better)  N=%zu, struct = "
      "{string, vector<u8>, optional<int>, int64, double} ==\n",
      N);
   std::printf("                  encode     decode     round-trip\n");
   std::printf("  json     :  %8.1f   %8.1f   %8.1f\n", enc_json, dec_json,
               enc_json + dec_json);
   std::printf("  msgpack  :  %8.1f   %8.1f   %8.1f   (%.2fx encode, "
               "%.2fx decode)\n",
               enc_mp, dec_mp, enc_mp + dec_mp, enc_json / enc_mp,
               dec_json / dec_mp);

   // ── pjson via typed from_struct + typed_pjson_view ──────────────
   //
   // Same reflected Bag, encoded and decoded through the typed pjson
   // entry points.  Vector/optional support landed in pjson_typed
   // alongside this benchmark — the typed-array fast path handles
   // the vector<u8>, t_null handles the absent optional<int32>.
   auto pjson_bytes = psio::from_struct(sample);
   std::printf("  pjson    : %zu bytes  (%.2fx vs json)\n",
               pjson_bytes.size(),
               static_cast<double>(pjson_bytes.size()) /
                  static_cast<double>(json_bytes.size()));

   auto enc_pj = bench_ns(
      [&] {
         std::vector<std::uint8_t> out;
         psio::to_pjson(sample, out);
         asm volatile("" : : "r"(out.data()) : "memory");
      },
      N);
   auto dec_pj = bench_ns(
      [&] {
         psio::pjson_view raw{pjson_bytes.data(), pjson_bytes.size()};
         auto v = psio::typed_pjson_view<bench::Bag>::from_pjson(raw);
         auto out = v.to_struct();
         asm volatile("" : : "r"(&out) : "memory");
      },
      N);

   std::printf("  pjson    :  %8.1f   %8.1f   %8.1f   (%.2fx encode, "
               "%.2fx decode vs json)\n",
               enc_pj, dec_pj, enc_pj + dec_pj, enc_json / enc_pj,
               dec_json / dec_pj);

   return 0;
}
