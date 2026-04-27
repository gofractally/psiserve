// libraries/psio/cpp/benchmarks/bench_nested_encode.cpp
//
// Baseline: encode-pass timing for every psio format on a balanced
// binary-tree shape at depths 2..7.  Designed to expose the recursive
// size-walk cost on length-prefixed length-delimited formats — the
// fractal O(D × N) pattern we already eliminated for protobuf via the
// stack-cached size pipeline.
//
// Each level doubles the leaf count; depth-D tree has 2^(D-1) leaves
// and 2^D - 1 nodes total.  Leaf carries an int64 + a short string so
// every level has both a primitive and a length-delimited member,
// giving the size walk something to do at every recursion step.
//
// Output: a markdown table per format, ns-per-encode (min of 7 trials)
// at each depth, plus the nested-cost ratio (depth-7 / depth-2 per
// node) — a tight ratio means O(N), a growing ratio means fractal.

#include <psio/avro.hpp>
#include <psio/bin.hpp>
#include <psio/bincode.hpp>
#include <psio/borsh.hpp>
#include <psio/frac.hpp>
#include <psio/protobuf.hpp>
#include <psio/pssz.hpp>
#include <psio/reflect.hpp>
#include <psio/ssz.hpp>

#include "harness.hpp"

#include <cstdio>
#include <string>
#include <vector>

// ── balanced binary tree, depth fixed by type ────────────────────────
struct NLeaf
{
   std::int64_t id;
   std::string  label;
};
PSIO_REFLECT(NLeaf, id, label)

struct N1 { NLeaf a; NLeaf b; };
PSIO_REFLECT(N1, a, b)
struct N2 { N1 a; N1 b; };
PSIO_REFLECT(N2, a, b)
struct N3 { N2 a; N2 b; };
PSIO_REFLECT(N3, a, b)
struct N4 { N3 a; N3 b; };  // depth 5, 16 leaves
PSIO_REFLECT(N4, a, b)
struct N5 { N4 a; N4 b; };  // depth 6, 32 leaves
PSIO_REFLECT(N5, a, b)
struct N6 { N5 a; N5 b; };  // depth 7, 64 leaves
PSIO_REFLECT(N6, a, b)

template <typename T>
struct depth_of;
template <> struct depth_of<NLeaf> { static constexpr int v = 1; };
template <> struct depth_of<N1>    { static constexpr int v = 2; };
template <> struct depth_of<N2>    { static constexpr int v = 3; };
template <> struct depth_of<N3>    { static constexpr int v = 4; };
template <> struct depth_of<N4>    { static constexpr int v = 5; };
template <> struct depth_of<N5>    { static constexpr int v = 6; };
template <> struct depth_of<N6>    { static constexpr int v = 7; };

template <typename T>
constexpr int leaves_of()
{
   return 1 << (depth_of<T>::v - 1);
}

// ── one cell: encode T via Fmt, returns ns-per-encode (min of 7) ──
template <typename Fmt, typename T>
double bench_encode(const T& v)
{
   // Auto-tuned timing harness (50 ms target per trial, 7 trials).
   auto t = psio3_bench::ns_per_iter(0u, [&](std::size_t /*i*/) {
      auto bytes = psio::encode(Fmt{}, v);
      asm volatile("" : : "r"(bytes.data()) : "memory");
   });
   return t.min_ns;
}

template <typename T>
T make_tree();

template <>
NLeaf make_tree<NLeaf>() { return {42, "leaf"}; }

template <>
N1 make_tree<N1>() { return {make_tree<NLeaf>(), make_tree<NLeaf>()}; }

template <>
N2 make_tree<N2>() { return {make_tree<N1>(), make_tree<N1>()}; }

template <>
N3 make_tree<N3>() { return {make_tree<N2>(), make_tree<N2>()}; }

template <>
N4 make_tree<N4>() { return {make_tree<N3>(), make_tree<N3>()}; }

template <>
N5 make_tree<N5>() { return {make_tree<N4>(), make_tree<N4>()}; }

template <>
N6 make_tree<N6>() { return {make_tree<N5>(), make_tree<N5>()}; }

// ── per-format runner ────────────────────────────────────────────────
template <typename Fmt>
struct fmt_traits;

template <>
struct fmt_traits<psio::frac32>
{
   static constexpr const char* name = "frac32";
};
template <>
struct fmt_traits<psio::frac16>
{
   static constexpr const char* name = "frac16";
};
template <>
struct fmt_traits<psio::pssz>
{
   static constexpr const char* name = "pssz";
};
template <>
struct fmt_traits<psio::ssz>
{
   static constexpr const char* name = "ssz";
};
template <>
struct fmt_traits<psio::bin>
{
   static constexpr const char* name = "bin";
};
template <>
struct fmt_traits<psio::borsh>
{
   static constexpr const char* name = "borsh";
};
template <>
struct fmt_traits<psio::bincode>
{
   static constexpr const char* name = "bincode";
};
template <>
struct fmt_traits<psio::avro>
{
   static constexpr const char* name = "avro";
};
template <>
struct fmt_traits<psio::protobuf>
{
   static constexpr const char* name = "protobuf";
};

template <typename Fmt>
void run_one_format()
{
   std::printf("\n## %s — encode ns by depth\n\n", fmt_traits<Fmt>::name);
   std::printf("| depth | leaves | wire bytes | ns/encode | ns/leaf |\n");
   std::printf("|------:|-------:|-----------:|----------:|--------:|\n");

   auto row = [&](auto v) {
      using T   = std::remove_cvref_t<decltype(v)>;
      auto  pre = psio::encode(Fmt{}, v);
      double ns = bench_encode<Fmt>(v);
      const int    d  = depth_of<T>::v;
      const int    L  = leaves_of<T>();
      std::printf("| %5d | %6d | %10zu | %9.1f | %7.2f |\n",
                  d, L, pre.size(), ns, ns / static_cast<double>(L));
   };

   row(make_tree<N1>());
   row(make_tree<N2>());
   row(make_tree<N3>());
   row(make_tree<N4>());
   row(make_tree<N5>());
   row(make_tree<N6>());
}

int main()
{
   std::printf("# Nested-encode baseline\n");
   std::printf("\nBalanced binary tree of `Leaf { i64 id; string label; }`,\n");
   std::printf("depths 2..7. ns/leaf below should stay constant if the\n");
   std::printf("encoder is O(N); growing ns/leaf means fractal cost.\n");

   run_one_format<psio::protobuf>();
   run_one_format<psio::frac32>();
   run_one_format<psio::frac16>();
   run_one_format<psio::pssz>();
   run_one_format<psio::ssz>();
   run_one_format<psio::bin>();
   run_one_format<psio::borsh>();
   run_one_format<psio::bincode>();
   run_one_format<psio::avro>();

   return 0;
}
