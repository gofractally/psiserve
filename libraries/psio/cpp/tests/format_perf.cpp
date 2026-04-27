// Quick perf comparison: json (reflection CPO) vs msgpack
// (reflection CPO) on the same struct.  pjson encodes a different
// surface (dynamic pjson_value tree), so it's measured separately
// on its native input.
//
// Build via psio3_format_perf target.  Output reports ns/op for
// encode + decode + round-trip on each format, plus encoded size.
//
// Disclaimer: simple steady-state microbenchmark.  Treat as
// rule-of-thumb relative numbers, not absolute throughput claims.

#include <psio/json.hpp>
#include <psio/msgpack.hpp>
#include <psio/pjson.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace bench
{
   struct Bag
   {
      std::string                 name;
      std::vector<std::uint8_t>   payload;
      std::optional<std::int32_t> count;
      std::int64_t                seq;
      double                      score;
   };
   PSIO_REFLECT(Bag, name, payload, count, seq, score)

   inline Bag make_sample()
   {
      return Bag{
         .name    = std::string{"alice-the-quick-brown-fox"},
         .payload = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
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

   // ── pjson on its native input (dynamic pjson_value) ─────────────
   //
   // pjson is a different shape: it encodes a dynamic value tree
   // rather than a reflected struct.  Apples-to-apples vs json/msgpack
   // would need wiring pjson into the CPO path; for now we measure
   // pjson on its own canonical workflow so the relative cost of
   // dynamic-value encoding is at least visible.
   psio::pjson_object obj;
   obj.emplace_back("name", psio::pjson_value{sample.name});
   psio::pjson_array payload_arr;
   for (auto b : sample.payload)
      payload_arr.emplace_back(
         psio::pjson_value{psio::pjson_number{
            static_cast<std::int64_t>(b), 0}});
   obj.emplace_back("payload", psio::pjson_value{std::move(payload_arr)});
   obj.emplace_back(
      "count",
      psio::pjson_value{psio::pjson_number{
         static_cast<std::int64_t>(*sample.count), 0}});
   obj.emplace_back("seq",
                    psio::pjson_value{psio::pjson_number{sample.seq, 0}});
   obj.emplace_back("score", psio::pjson_value{sample.score});
   psio::pjson_value pj{std::move(obj)};

   auto pj_bytes = psio::pjson::encode(pj);
   std::printf("\n  pjson    : %zu bytes  (%.2fx vs json)\n",
               pj_bytes.size(),
               static_cast<double>(pj_bytes.size()) /
                  static_cast<double>(json_bytes.size()));

   auto enc_pj = bench_ns(
      [&] {
         auto out = psio::pjson::encode(pj);
         asm volatile("" : : "r"(out.data()) : "memory");
      },
      N);
   auto dec_pj = bench_ns(
      [&] {
         auto out = psio::pjson::decode(
            std::span<const std::uint8_t>{pj_bytes.data(), pj_bytes.size()});
         asm volatile("" : : "r"(&out) : "memory");
      },
      N);

   std::printf("  pjson    :  %8.1f   %8.1f   %8.1f   (dynamic value, "
               "different surface)\n",
               enc_pj, dec_pj, enc_pj + dec_pj);

   return 0;
}
