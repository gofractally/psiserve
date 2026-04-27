// Head-to-head: psio::msgpack vs msgpack-cxx.
//
// Two comparisons:
//
//   (1) Typed encode/decode  — psio::encode<T>/decode<T> with
//       PSIO_REFLECT vs msgpack::pack/msgpack::unpack with
//       MSGPACK_DEFINE.  Same struct, identical wire bytes (both
//       libraries default to fixarray / positional record form),
//       so the difference is pure codec quality.
//
//   (2) Schemaless dynamic walk — msgpack::object tagged tree
//       (msgpack-cxx's canonical "I don't know the schema, give me
//       a tree" surface) vs psio::pjson_view (psio's canonical
//       schemaless self-describing surface).  Different wire
//       formats — pjson has a per-object index that gives O(1)
//       lookup; msgpack::object has none — so this is a use-case
//       comparison, not a wire comparison.
//
// Build only when msgpack-cxx is present; see the parent
// CMakeLists.txt.  Run manually:
//
//     ./build/Debug-rename/bin/psio3_format_perf_external

#include <psio/msgpack.hpp>
#include <psio/pjson_typed.hpp>
#include <psio/pjson_view.hpp>

#include <msgpack.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace bench
{
   struct Sub
   {
      std::int32_t id;
      std::string  label;

      MSGPACK_DEFINE(id, label);
   };

   struct Bag
   {
      std::string                 name;
      std::vector<std::uint8_t>   payload;
      std::vector<std::int32_t>   ids;
      std::vector<Sub>            entries;
      // Note: msgpack-cxx represents std::optional via a separate
      // adapter that ships with newer versions; for a clean baseline
      // here we skip the optional field — both libraries see the
      // same shape.
      std::int64_t                seq;
      double                      score;

      MSGPACK_DEFINE(name, payload, ids, entries, seq, score);
   };

   //  Same struct, with PSIO_REFLECT instead of MSGPACK_DEFINE.
   //  msgpack-cxx's MSGPACK_DEFINE injects member functions; psio's
   //  PSIO_REFLECT is an out-of-line ADL hook.  They coexist on the
   //  same C++ class without conflict — but PSIO_REFLECT must live
   //  in the type's enclosing namespace so ADL finds the helper.
   PSIO_REFLECT(Sub, id, label)
   PSIO_REFLECT(Bag, name, payload, ids, entries, seq, score)

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

   //  ── Typed encode/decode ────────────────────────────────────────
   //
   //  Psio path:  psio::encode<T>/decode<T>.
   //  msgpack-cxx path:  msgpack::pack into sbuffer + msgpack::unpack
   //                     + object::convert<T>.

   //  Pre-encode once for size + decode benches.
   auto psio_bytes = psio::encode(psio::msgpack{}, sample);

   msgpack::sbuffer mp_sbuf;
   msgpack::pack(mp_sbuf, sample);
   //  Wire bytes should be identical between the two libraries —
   //  both default to fixarray/positional, both use the same
   //  smallest-fitting integer rules.  Confirm the sizes match
   //  before benching.
   bool wire_match = (mp_sbuf.size() == psio_bytes.size()) &&
                     (std::memcmp(mp_sbuf.data(), psio_bytes.data(),
                                  mp_sbuf.size()) == 0);

   std::printf("== Typed encode/decode  (same wire format, same struct) ==\n");
   std::printf("  wire size — psio       : %zu\n", psio_bytes.size());
   std::printf("  wire size — msgpack-cxx: %zu  (byte-identical: %s)\n",
               mp_sbuf.size(), wire_match ? "YES" : "no");

   auto enc_psio = bench_ns(
      [&] {
         auto out = psio::encode(psio::msgpack{}, sample);
         asm volatile("" : : "r"(out.data()) : "memory");
      },
      N);
   auto dec_psio = bench_ns(
      [&] {
         auto out = psio::decode<bench::Bag>(
            psio::msgpack{}, std::span<const char>{psio_bytes});
         asm volatile("" : : "r"(&out) : "memory");
      },
      N);

   auto enc_mpx = bench_ns(
      [&] {
         msgpack::sbuffer sbuf;
         msgpack::pack(sbuf, sample);
         asm volatile("" : : "r"(sbuf.data()) : "memory");
      },
      N);
   auto dec_mpx = bench_ns(
      [&] {
         msgpack::object_handle oh =
            msgpack::unpack(mp_sbuf.data(), mp_sbuf.size());
         bench::Bag out;
         oh.get().convert(out);
         asm volatile("" : : "r"(&out) : "memory");
      },
      N);

   std::printf("\n  ns/op  (lower is better)  N=%zu\n", N);
   std::printf("                       encode      decode      round-trip\n");
   std::printf("  psio::msgpack    :  %8.1f   %8.1f   %8.1f\n",
               enc_psio, dec_psio, enc_psio + dec_psio);
   std::printf(
      "  msgpack-cxx      :  %8.1f   %8.1f   %8.1f   "
      "(psio is %.2fx encode, %.2fx decode)\n",
      enc_mpx, dec_mpx, enc_mpx + dec_mpx, enc_mpx / enc_psio,
      dec_mpx / dec_psio);

   //  ── Schemaless dynamic walk ────────────────────────────────────
   //
   //  Different wire formats so this is a use-case comparison rather
   //  than a wire-format one.  Both surfaces let you parse a buffer
   //  whose schema you don't know at compile time and walk it by
   //  field name.
   //
   //  msgpack::object: tagged tree, depth-N traversal cost per
   //  access (find by linear scan of the map's key/value pairs;
   //  for arrays, index by [i]).
   //
   //  pjson_view: hash-prefilter + offset-table lookup, O(1) per
   //  named-field access on canonical buffers.

   //  Build pjson bytes from the same logical sample.  pjson is
   //  always self-describing (named); msgpack-cxx's default fixarray
   //  isn't — so we encode the same shape once into pjson and once
   //  into msgpack::object that wraps the existing sbuffer.
   auto pjson_bytes = psio::from_struct(sample);
   psio::pjson_view pj_view{pjson_bytes.data(), pjson_bytes.size()};

   //  Decode msgpack-cxx into msgpack::object once for the access loop.
   msgpack::object_handle oh =
      msgpack::unpack(mp_sbuf.data(), mp_sbuf.size());
   msgpack::object        mp_obj = oh.get();

   //  The access pattern: walk the four entries and extract
   //  entry[2].label — a deep-ish lookup that exercises both
   //  surfaces' walking machinery.
   //
   //  msgpack::object: object is a tagged variant; for a fixarray
   //  the cells are at obj.via.array.ptr[i].  Within Bag (array of
   //  6 fields), entries is at index 3, then [2] is the third Sub,
   //  then label is field 1 of that Sub.
   //
   //  pjson_view: named lookup all the way.  Bag → "entries" →
   //  index [2] → "label".
   auto walk_mpx = bench_ns(
      [&] {
         //  Field index 3 = entries (declaration order).
         msgpack::object const& entries_obj = mp_obj.via.array.ptr[3];
         msgpack::object const& sub_obj = entries_obj.via.array.ptr[2];
         msgpack::object const& label_obj = sub_obj.via.array.ptr[1];
         std::string label;
         label_obj.convert(label);
         asm volatile("" : : "r"(label.data()) : "memory");
      },
      N);
   auto walk_pjs = bench_ns(
      [&] {
         auto entries_v = pj_view.find("entries");
         auto sub_v = entries_v->at(2);
         auto label_v = sub_v.find("label");
         std::string label = std::string(label_v->as_string());
         asm volatile("" : : "r"(label.data()) : "memory");
      },
      N);

   std::printf("\n== Schemaless dynamic walk  (deep field access) ==\n");
   std::printf(
      "  msgpack::object  : %8.1f ns/op (find entries[2].label "
      "via positional index)\n",
      walk_mpx);
   std::printf(
      "  pjson_view       : %8.1f ns/op (find entries[2].label "
      "by name)\n",
      walk_pjs);
   std::printf(
      "  Note: different wire formats so this is a use-case "
      "comparison.\n"
      "  msgpack::object benefits from positional-array index access;\n"
      "  pjson_view benefits from per-object hash+offset table.\n");

   return 0;
}
