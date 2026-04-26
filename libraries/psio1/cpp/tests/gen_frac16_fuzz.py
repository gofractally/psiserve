#!/usr/bin/env python3
"""Generate random struct types + fuzz test cases for frac16 / frac32 round-trip.

Output is a self-contained .cpp file with:
  * N random struct types, each declared with PSIO1_REFLECT and `= default` ==.
  * A `make_<Struct>(rng)` factory per type that builds a random instance.
  * A Catch2 TEST_CASE per type that runs M iterations of
        pack -> validate -> unpack -> compare
    at both frac_format_32 and frac_format_16, plus a cross-format
    consistency check (decode(frac16_pack(v)) == decode(frac32_pack(v))).

The file is regenerated deterministically from --seed; commit the
output so the build has no Python dependency.

Usage:
    python3 gen_frac16_fuzz.py --output path/to/out.cpp \
        [--count 50] [--seed 42] [--iterations 30]
"""

from __future__ import annotations

import argparse
import random
from pathlib import Path

# ── Primitive leaf types ─────────────────────────────────────────────────────

PRIMITIVES = [
    "std::uint8_t",
    "std::uint16_t",
    "std::uint32_t",
    "std::uint64_t",
    "std::int8_t",
    "std::int16_t",
    "std::int32_t",
    "std::int64_t",
    "float",
    "double",
    "bool",
]

# ── Random type generator ────────────────────────────────────────────────────

def gen_type(rng: random.Random, depth: int = 0, max_depth: int = 2) -> str:
    """Pick a random packable type. `depth` caps nesting."""
    if depth >= max_depth:
        return rng.choice(PRIMITIVES)

    # Weight toward primitives/strings so field counts stay reasonable.
    kind = rng.choices(
        ["primitive", "string", "vector", "optional", "array"],
        weights=[45, 20, 15, 15, 5],
    )[0]

    # Exclude `bool` from container elements — std::vector<bool> uses proxy
    # iterators and does not satisfy fracpack's PackableSequenceContainer
    # contract (takes bool* in unpack).
    non_bool = [t for t in PRIMITIVES if t != "bool"]

    def inner_type() -> str:
        ty = gen_type(rng, depth + 1, max_depth)
        # Rewrite primitive `bool` → pick a different primitive so containers
        # stay vector<bool>-free. Compound types returned from recursion have
        # their own guard via this same function.
        if ty == "bool":
            ty = rng.choice(non_bool)
        return ty

    if kind == "primitive":
        return rng.choice(PRIMITIVES)
    if kind == "string":
        return "std::string"
    if kind == "vector":
        return f"std::vector<{inner_type()}>"
    if kind == "optional":
        return f"std::optional<{inner_type()}>"
    if kind == "array":
        n = rng.randint(1, 4)
        return f"std::array<{inner_type()}, {n}>"
    raise RuntimeError("unreachable")


# ── Struct generation ────────────────────────────────────────────────────────

def gen_struct(rng: random.Random, idx: int) -> dict:
    """Return a dict describing one random struct."""
    n_fields = rng.randint(1, 6)
    fields = [(f"f{i}", gen_type(rng)) for i in range(n_fields)]
    def_will_not_change = rng.random() < 0.15  # 15% are fixed/DWNC
    # DWNC structs can't have variable-size fields. Re-roll until all fixed.
    if def_will_not_change:
        for fi, (name, ty) in enumerate(fields):
            while _is_variable(ty):
                fields[fi] = (name, rng.choice(PRIMITIVES))
                ty = fields[fi][1]
    return {
        "name": f"FuzzS{idx:03d}",
        "fields": fields,
        "dwnc": def_will_not_change,
    }


def _is_variable(ty: str) -> bool:
    return (
        ty.startswith("std::string")
        or ty.startswith("std::vector")
        or ty.startswith("std::optional")
        or ("std::array<" in ty and ("std::string" in ty or "std::vector" in ty or "std::optional" in ty))
    )


# ── C++ emission: struct declarations ────────────────────────────────────────

def emit_struct(s: dict) -> str:
    fields_decl = "\n".join(f"   {ty:40s} {name};" for name, ty in s["fields"])
    field_names = ", ".join(name for name, _ in s["fields"])
    dwnc_arg = "definitionWillNotChange(), " if s["dwnc"] else ""
    return (
        f"struct {s['name']}\n"
        f"{{\n"
        f"{fields_decl}\n"
        f"   bool operator==(const {s['name']}&) const = default;\n"
        f"}};\n"
        f"PSIO1_REFLECT({s['name']}, {dwnc_arg}{field_names})\n"
    )


# ── C++ emission: factory functions ──────────────────────────────────────────

def emit_factory(s: dict) -> str:
    init_lines = []
    for name, ty in s["fields"]:
        init_lines.append(f"   out.{name} = fuzz::rand_val<{ty}>(rng);")
    body = "\n".join(init_lines)
    return (
        f"inline {s['name']} make_{s['name']}(std::mt19937_64& rng)\n"
        f"{{\n"
        f"   {s['name']} out{{}};\n"
        f"{body}\n"
        f"   return out;\n"
        f"}}\n"
    )


# ── C++ emission: test cases ─────────────────────────────────────────────────

def emit_test_case(s: dict, iterations: int) -> str:
    return f"""TEST_CASE("frac16 fuzz: {s['name']}", "[fracpack16][fuzz]")
{{
   std::mt19937_64 rng{{0xF16ull ^ std::hash<std::string>{{}}("{s['name']}")}};
   for (int i = 0; i < {iterations}; ++i)
   {{
      auto v = make_{s['name']}(rng);
      auto b32 = psio1::to_frac(v);
      auto b16 = psio1::to_frac16(v);
      REQUIRE(psio1::validate_frac<{s['name']}>(b32) != psio1::validation_t::invalid);
      REQUIRE(psio1::validate_frac16<{s['name']}>(b16) != psio1::validation_t::invalid);
      {s['name']} r32{{}};
      {s['name']} r16{{}};
      REQUIRE(psio1::from_frac(r32, b32));
      REQUIRE(psio1::from_frac16(r16, b16));
      REQUIRE(r32 == v);
      REQUIRE(r16 == v);
      REQUIRE(r32 == r16);  // cross-format consistency
      REQUIRE(b16.size() <= b32.size());
   }}
}}
"""


# ── File emission ────────────────────────────────────────────────────────────

PROLOGUE = '''\
// AUTO-GENERATED by gen_frac16_fuzz.py — DO NOT EDIT BY HAND.
//
// Regenerate with:  python3 libraries/psio1/cpp/tests/gen_frac16_fuzz.py \\
//                        --output libraries/psio1/cpp/tests/frac16_fuzz_generated.cpp
//
// Each TEST_CASE exercises one random struct type over N random values;
// asserts pack/unpack round-trips at both frac32 and frac16, that
// validate_frac* accepts the output, and that decoded values match
// cross-format.

#include <catch2/catch.hpp>

#include <psio1/fracpack.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

namespace fuzz
{
   // Type traits for dispatch
   template <typename T>
   struct is_vector : std::false_type {};
   template <typename U>
   struct is_vector<std::vector<U>> : std::true_type { using value_type = U; };

   template <typename T>
   struct is_optional : std::false_type {};
   template <typename U>
   struct is_optional<std::optional<U>> : std::true_type { using value_type = U; };

   template <typename T>
   struct is_array : std::false_type {};
   template <typename U, std::size_t N>
   struct is_array<std::array<U, N>> : std::true_type
   {
      using value_type = U;
      static constexpr std::size_t size = N;
   };

   // Single rand_val<T> entry point; uses `if constexpr` to dispatch so
   // recursion between container kinds is a plain self-call (no overload
   // lookup ordering problems).
   template <typename T>
   T rand_val(std::mt19937_64& rng)
   {
      if constexpr (std::is_same_v<T, bool>)
      {
         return (rng() & 1u) != 0;
      }
      else if constexpr (std::is_integral_v<T>)
      {
         std::uniform_int_distribution<
             std::conditional_t<(sizeof(T) < 2), int, T>> d(std::numeric_limits<T>::min(),
                                                            std::numeric_limits<T>::max());
         return static_cast<T>(d(rng));
      }
      else if constexpr (std::is_floating_point_v<T>)
      {
         std::uniform_real_distribution<T> d(-1e6, 1e6);
         return d(rng);
      }
      else if constexpr (std::is_same_v<T, std::string>)
      {
         std::uniform_int_distribution<int> len_d(0, 32);
         std::uniform_int_distribution<int> ch_d(32, 126);
         int                                n = len_d(rng);
         std::string                        s;
         s.reserve(n);
         for (int i = 0; i < n; ++i)
            s.push_back(static_cast<char>(ch_d(rng)));
         return s;
      }
      else if constexpr (is_vector<T>::value)
      {
         using E = typename is_vector<T>::value_type;
         std::uniform_int_distribution<int> len_d(0, 6);
         int                                n = len_d(rng);
         T                                  out;
         out.reserve(n);
         for (int i = 0; i < n; ++i)
            out.push_back(rand_val<E>(rng));
         return out;
      }
      else if constexpr (is_optional<T>::value)
      {
         using Inner = typename is_optional<T>::value_type;
         if ((rng() & 1u) == 0)
            return std::nullopt;
         return std::optional<Inner>(rand_val<Inner>(rng));
      }
      else if constexpr (is_array<T>::value)
      {
         using E = typename is_array<T>::value_type;
         T out;
         for (auto& e : out)
            e = rand_val<E>(rng);
         return out;
      }
      else
      {
         static_assert(!sizeof(T*), "rand_val: unsupported type");
      }
   }
}  // namespace fuzz

'''

EPILOGUE = ''


BENCH_PROLOGUE = '''\
// AUTO-GENERATED by gen_frac16_fuzz.py --bench-output — DO NOT EDIT BY HAND.
//
// Benchmark harness emitted from the same Python generator that produces
// the fuzz test corpus. Includes N random struct types, factories, and a
// `run_all_bench()` function that returns per-op aggregate timings across
// every type.
//
// Regenerate with:
//   python3 libraries/psio1/cpp/tests/gen_frac16_fuzz.py --bench-output \\
//       libraries/psio1/cpp/benchmarks/bench_fuzz_generated.cpp

#include <psio1/fracpack.hpp>
#include <psio1/frac_ref.hpp>
#include <psio1/from_bin.hpp>
#include <psio1/to_bin.hpp>

#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

template <typename T>
inline void do_not_optimize(T const& val)
{
   asm volatile("" : : "r,m"(val) : "memory");
}
inline void clobber_memory()
{
   asm volatile("" ::: "memory");
}

namespace fuzz
{
   // Type traits for dispatch
   template <typename T>
   struct is_vector : std::false_type {};
   template <typename U>
   struct is_vector<std::vector<U>> : std::true_type { using value_type = U; };

   template <typename T>
   struct is_optional : std::false_type {};
   template <typename U>
   struct is_optional<std::optional<U>> : std::true_type { using value_type = U; };

   template <typename T>
   struct is_array : std::false_type {};
   template <typename U, std::size_t N>
   struct is_array<std::array<U, N>> : std::true_type
   {
      using value_type = U;
      static constexpr std::size_t size = N;
   };

   // Single rand_val<T> entry point; uses `if constexpr` to dispatch so
   // recursion between container kinds is a plain self-call.
   template <typename T>
   T rand_val(std::mt19937_64& rng)
   {
      if constexpr (std::is_same_v<T, bool>)
      {
         return (rng() & 1u) != 0;
      }
      else if constexpr (std::is_integral_v<T>)
      {
         std::uniform_int_distribution<
             std::conditional_t<(sizeof(T) < 2), int, T>> d(std::numeric_limits<T>::min(),
                                                            std::numeric_limits<T>::max());
         return static_cast<T>(d(rng));
      }
      else if constexpr (std::is_floating_point_v<T>)
      {
         std::uniform_real_distribution<T> d(-1e6, 1e6);
         return d(rng);
      }
      else if constexpr (std::is_same_v<T, std::string>)
      {
         std::uniform_int_distribution<int> len_d(0, 32);
         std::uniform_int_distribution<int> ch_d(32, 126);
         int                                n = len_d(rng);
         std::string                        s;
         s.reserve(n);
         for (int i = 0; i < n; ++i)
            s.push_back(static_cast<char>(ch_d(rng)));
         return s;
      }
      else if constexpr (is_vector<T>::value)
      {
         using E = typename is_vector<T>::value_type;
         std::uniform_int_distribution<int> len_d(0, 6);
         int                                n = len_d(rng);
         T                                  out;
         out.reserve(n);
         for (int i = 0; i < n; ++i)
            out.push_back(rand_val<E>(rng));
         return out;
      }
      else if constexpr (is_optional<T>::value)
      {
         using Inner = typename is_optional<T>::value_type;
         if ((rng() & 1u) == 0)
            return std::nullopt;
         return std::optional<Inner>(rand_val<Inner>(rng));
      }
      else if constexpr (is_array<T>::value)
      {
         using E = typename is_array<T>::value_type;
         T out;
         for (auto& e : out)
            e = rand_val<E>(rng);
         return out;
      }
      else
      {
         static_assert(!sizeof(T*), "rand_val: unsupported type");
      }
   }
}  // namespace fuzz

'''

BENCH_RUN_HARNESS = '''
// ── Per-type op timings ─────────────────────────────────────────────────

template <typename Fn>
static double bench_ns(Fn fn)
{
   using clock = std::chrono::high_resolution_clock;
   for (int i = 0; i < 50; ++i) { fn(); clobber_memory(); }
   std::size_t iters = 0;
   auto start = clock::now();
   while (std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - start).count() < 10'000)
   { fn(); clobber_memory(); ++iters; }
   auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count();
   return iters > 0 ? static_cast<double>(ns) / iters : 0.0;
}

// Instances are constructed by emitted per-type factories (make_FuzzSnnn).

struct OpTotals
{
   double packsize_frac = 0;
   double packsize_bin  = 0;
   double pack_frac     = 0;
   double pack_bin      = 0;
   double validate_frac = 0;
   double unpack_frac   = 0;
   double unpack_bin    = 0;
   int    type_count    = 0;
};

template <typename T>
void bench_type(OpTotals& totals, const T& v)
{
   auto bin_bytes  = psio1::convert_to_bin(v);
   auto frac_bytes = psio1::to_frac(v);

   totals.packsize_frac += bench_ns([&] {
      auto s = psio1::fracpack_size(v); do_not_optimize(s);
   });
   totals.packsize_bin += bench_ns([&] {
      psio1::bin_detail::bin_size_cache c;
      auto s = psio1::compute_bin_size(v, c);
      do_not_optimize(s);
   });
   totals.pack_frac += bench_ns([&] {
      auto b = psio1::to_frac(v); do_not_optimize(b.data());
   });
   totals.pack_bin += bench_ns([&] {
      auto b = psio1::convert_to_bin(v); do_not_optimize(b.data());
   });
   totals.validate_frac += bench_ns([&] {
      auto r = psio1::validate_frac<T>({frac_bytes.data(), frac_bytes.size()});
      do_not_optimize(r);
   });
   totals.unpack_frac += bench_ns([&] {
      T u{};
      (void)psio1::from_frac(u, std::span<const char>(frac_bytes.data(), frac_bytes.size()));
      do_not_optimize(u);
   });
   totals.unpack_bin += bench_ns([&] {
      T u{};
      psio1::input_stream s{bin_bytes.data(), bin_bytes.size()};
      psio1::from_bin(u, s);
      do_not_optimize(u);
   });
   ++totals.type_count;
}
'''

BENCH_EPILOGUE = '''
int main()
{
   std::mt19937_64 rng{0xB00Bull};
   OpTotals t{};
'''

BENCH_MAIN_TAIL = '''
   auto mean = [&](double sum) { return t.type_count ? sum / t.type_count : 0.0; };
   std::printf("\\n=== Fuzz-type aggregate perf (mean ns/op across %d types) ===\\n\\n",
               t.type_count);
   std::printf("  %-12s  %8.1f ns  (fracpack_size)\\n",   "packsize_frac", mean(t.packsize_frac));
   std::printf("  %-12s  %8.1f ns  (compute_bin_size)\\n", "packsize_bin",  mean(t.packsize_bin));
   std::printf("  %-12s  %8.1f ns  (to_frac)\\n",          "pack_frac",     mean(t.pack_frac));
   std::printf("  %-12s  %8.1f ns  (convert_to_bin)\\n",   "pack_bin",      mean(t.pack_bin));
   std::printf("  %-12s  %8.1f ns  (validate_frac)\\n",    "validate_frac", mean(t.validate_frac));
   std::printf("  %-12s  %8.1f ns  (from_frac)\\n",        "unpack_frac",   mean(t.unpack_frac));
   std::printf("  %-12s  %8.1f ns  (from_bin)\\n",         "unpack_bin",    mean(t.unpack_bin));
   std::printf("\\n  (no --reporter means: mean across %d generated struct types)\\n", t.type_count);
   return 0;
}
'''


def emit_bench_main_line(s: dict) -> str:
    return f"   {{ auto v = make_{s['name']}(rng); bench_type(t, v); }}\n"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--output", type=Path,
                    help="Test-mode output (Catch2 TEST_CASE per type)")
    ap.add_argument("--bench-output", type=Path,
                    help="Benchmark-mode output (main() + mean ns/op per op)")
    ap.add_argument("--count", type=int, default=50)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--iterations", type=int, default=30)
    args = ap.parse_args()

    if not args.output and not args.bench_output:
        ap.error("must supply --output or --bench-output (or both)")

    rng = random.Random(args.seed)
    structs = [gen_struct(rng, i) for i in range(args.count)]

    if args.output:
        parts = [PROLOGUE]
        parts.append("\n// ── Struct definitions ───────────────────────────────────────────────────\n")
        for s in structs:
            parts.append(emit_struct(s)); parts.append("\n")
        parts.append("\n// ── Factories ────────────────────────────────────────────────────────────\n")
        for s in structs:
            parts.append(emit_factory(s)); parts.append("\n")
        parts.append("\n// ── Test cases ───────────────────────────────────────────────────────────\n")
        for s in structs:
            parts.append(emit_test_case(s, args.iterations)); parts.append("\n")
        parts.append(EPILOGUE)
        args.output.write_text("".join(parts))
        print(f"Wrote {args.output}: {args.count} structs (test mode), seed={args.seed}")

    if args.bench_output:
        parts = [BENCH_PROLOGUE]
        parts.append("\n// ── Struct definitions ───────────────────────────────────────────────────\n")
        for s in structs:
            parts.append(emit_struct(s)); parts.append("\n")
        parts.append("\n// ── Factories ────────────────────────────────────────────────────────────\n")
        for s in structs:
            parts.append(emit_factory(s)); parts.append("\n")
        parts.append(BENCH_RUN_HARNESS)
        parts.append(BENCH_EPILOGUE)
        for s in structs:
            parts.append(emit_bench_main_line(s))
        parts.append(BENCH_MAIN_TAIL)
        args.bench_output.write_text("".join(parts))
        print(f"Wrote {args.bench_output}: {args.count} structs (bench mode), seed={args.seed}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
