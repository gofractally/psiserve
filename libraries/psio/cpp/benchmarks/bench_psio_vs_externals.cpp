// libraries/psio/cpp/benchmarks/bench_psio_vs_externals.cpp
//
// Unified head-to-head: psio's wire formats × canonical external
// libraries × shapes × ops.  Replaces the old v1↔v3 bench_perf_report
// matrix.  Writes one snapshot CSV per run to
//   bench_snapshots/perf_<UTC-ISO>_<commit>.csv
// (gitignored by default; commit a snapshot only when accepting it as
//  a new baseline).
//
// Phase 3 scaffold: psio-only columns first.  Adapter wiring for
// canonical external libs (msgpack-cxx, libprotobuf, libflatbuffers,
// libcapnp, sszpp) lands as separate commits, each gated behind a
// CMake find_package().  See .issues/psio-bench-vs-externals-plan.md.

#include <psio/avro.hpp>
#include <psio/bin.hpp>
#include <psio/bincode.hpp>
#include <psio/borsh.hpp>
#include <psio/capnp.hpp>
#include <psio/flatbuf.hpp>
#include <psio/frac.hpp>
#include <psio/msgpack.hpp>
#include <psio/protobuf.hpp>
#include <psio/pssz.hpp>
#include <psio/reflect.hpp>
#include <psio/ssz.hpp>

#include "harness.hpp"
#include "shapes.hpp"

#ifdef PSIO_HAVE_MSGPACK
#  include "adapters/msgpack_adapter.hpp"
#endif

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

using namespace psio_bench;

namespace {

   //  Per-(format, shape) support gate.  Some psio formats fail to
   //  encode certain shapes (e.g., frac doesn't yet support
   //  vector<variable-element>); the static_assert lives inside the
   //  template body, so SFINAE can't catch it — we have to skip
   //  explicitly here.  Default = supported; specialise to false_type
   //  for known-unsupported pairs.
   template <typename Fmt, typename T>
   struct fmt_supports : std::true_type {};

   //  frac32 / frac16 don't currently support vector<variable> — Order
   //  has vector<LineItem>, OrderBounded has vector<LineItemBounded>.
   template <> struct fmt_supports<psio::frac32, Order>        : std::false_type {};
   template <> struct fmt_supports<psio::frac32, OrderBounded> : std::false_type {};
   template <> struct fmt_supports<psio::frac16, Order>        : std::false_type {};
   template <> struct fmt_supports<psio::frac16, OrderBounded> : std::false_type {};

   //  Per-(shape, format) op timings for the psio side.  Each call
   //  exercises one CPO and pushes one snapshot_row into `out`.
   //
   //  Ops covered here:
   //    size_of          — psio::size_of(fmt, v)
   //    encode_rvalue    — auto bytes = psio::encode(fmt, v)
   //    encode_sink      — psio::encode(fmt, v, sink) into a reused vector
   //    decode           — psio::decode<T>(fmt, bytes)
   //    validate         — psio::validate<T>(fmt, bytes)  (where supported)
   //
   //  view_one / view_all are added in a follow-up commit (task #66).

   template <typename Fmt, typename T>
   void bench_psio_cell(std::vector<snapshot_row>& out,
                        const std::string&         shape_name,
                        const std::string&         fmt_name,
                        Fmt                        fmt,
                        const T&                   v,
                        const std::string&         mode = "static")
   {
      auto bytes = psio::encode(fmt, v);
      const std::size_t wire = bytes.size();
      const std::span<const char> span_bytes{bytes.data(), wire};

      auto record = [&](std::string op, double ns, double ns_med,
                         double cv, std::size_t wire_b, std::size_t iters,
                         int trials, std::string notes = "") {
         out.push_back(snapshot_row{
            .shape      = shape_name,
            .format     = fmt_name,
            .library    = "psio",
            .mode       = mode,
            .op         = std::move(op),
            .ns_min     = ns,
            .ns_median  = ns_med,
            .cv_pct     = cv,
            .wire_bytes = wire_b,
            .iters      = iters,
            .trials     = trials,
            .notes      = std::move(notes),
         });
      };

      auto cv = [](const timing& t) {
         return t.min_ns > 0.0 ? t.stddev_ns / t.min_ns * 100.0 : 0.0;
      };

      // size_of
      auto t_size = ns_per_iter(0u, [&](std::size_t) {
         std::size_t n = psio::size_of(fmt, v);
         asm volatile("" : "+r"(n) : : "memory");
      });
      record("size_of", t_size.min_ns, t_size.median_ns, cv(t_size),
             wire, t_size.iters, t_size.trials);

      // encode_rvalue
      auto t_enc = ns_per_iter(0u, [&](std::size_t) {
         auto b = psio::encode(fmt, v);
         asm volatile("" : : "r"(b.data()) : "memory");
      });
      record("encode_rvalue", t_enc.min_ns, t_enc.median_ns, cv(t_enc),
             wire, t_enc.iters, t_enc.trials);

      // encode_sink (reused buffer) — gated; capnp/flatbuf only have
      // the rvalue path.
      if constexpr (requires(std::vector<char>& s) {
                       psio::encode(fmt, v, s);
                    })
      {
         std::vector<char> sink;
         sink.reserve(wire * 2);
         auto t_sink = ns_per_iter(0u, [&](std::size_t) {
            sink.clear();
            psio::encode(fmt, v, sink);
         });
         record("encode_sink", t_sink.min_ns, t_sink.median_ns,
                cv(t_sink), wire, t_sink.iters, t_sink.trials);
      }

      // decode
      auto t_dec = ns_per_iter(0u, [&](std::size_t) {
         auto p = psio::decode<T>(fmt, span_bytes);
         asm volatile("" : : "r"(&p) : "memory");
      });
      record("decode", t_dec.min_ns, t_dec.median_ns, cv(t_dec),
             wire, t_dec.iters, t_dec.trials);

      // validate (some formats / shapes don't expose this; skip if it
      // would force compile-time errors via SFINAE-bypassing static
      // asserts).  For now, run unconditionally and let the linker /
      // compiler tell us where it isn't supported — wrap the call in
      // a try/catch so a missing validate doesn't crash the run.
      try
      {
         auto t_val = ns_per_iter(0u, [&](std::size_t) {
            auto st = psio::validate<T>(fmt, span_bytes);
            asm volatile("" : : "r"(&st) : "memory");
         });
         record("validate", t_val.min_ns, t_val.median_ns, cv(t_val),
                wire, t_val.iters, t_val.trials);
      }
      catch (...)
      {
         // No-op — some shapes/formats throw via codec_status::or_throw
         // semantics; the bench shouldn't abort.
      }
   }

#ifdef PSIO_HAVE_MSGPACK
   //  Time msgpack-cxx (the canonical lib) on a single shape, with
   //  the same op surface as bench_psio_cell.  library = "msgpack-cxx",
   //  format = "msgpack" (wire-compatible with psio::msgpack).
   template <typename T>
   void bench_msgpack_cxx_cell(std::vector<snapshot_row>& out,
                                const std::string& shape_name, const T& v)
   {
      auto bytes = mp_bench::encode(v);
      const std::size_t wire = bytes.size();

      auto record = [&](std::string op, double ns, double ns_med,
                         double cv, std::size_t wire_b, std::size_t iters,
                         int trials, std::string notes = "") {
         out.push_back(snapshot_row{
            .shape      = shape_name,
            .format     = "msgpack",
            .library    = "msgpack-cxx",
            .mode       = "static",
            .op         = std::move(op),
            .ns_min     = ns,
            .ns_median  = ns_med,
            .cv_pct     = cv,
            .wire_bytes = wire_b,
            .iters      = iters,
            .trials     = trials,
            .notes      = std::move(notes),
         });
      };

      auto cv = [](const timing& t) {
         return t.min_ns > 0.0 ? t.stddev_ns / t.min_ns * 100.0 : 0.0;
      };

      // size_of  — msgpack-cxx has no two-pass sizer; the only honest
      // measurement is "pack to a throwaway sbuffer, take .size()".
      auto t_size = ns_per_iter(0u, [&](std::size_t) {
         std::size_t n = mp_bench::size_of(v);
         asm volatile("" : "+r"(n) : : "memory");
      });
      record("size_of", t_size.min_ns, t_size.median_ns, cv(t_size),
             wire, t_size.iters, t_size.trials,
             "pack-to-throwaway-sbuffer (no native sizer)");

      // encode_rvalue — fresh sbuffer + std::vector<char> copy.
      auto t_enc = ns_per_iter(0u, [&](std::size_t) {
         auto b = mp_bench::encode(v);
         asm volatile("" : : "r"(b.data()) : "memory");
      });
      record("encode_rvalue", t_enc.min_ns, t_enc.median_ns, cv(t_enc),
             wire, t_enc.iters, t_enc.trials);

      // encode_sink — reused sbuffer (msgpack-cxx's native buffer).
      msgpack::sbuffer sbuf;
      auto t_sink = ns_per_iter(0u, [&](std::size_t) {
         mp_bench::encode_into(v, sbuf);
      });
      record("encode_sink", t_sink.min_ns, t_sink.median_ns, cv(t_sink),
             wire, t_sink.iters, t_sink.trials,
             "reused msgpack::sbuffer (not std::vector<char>)");

      // decode  — unpack + convert.  msgpack-cxx allocates internally
      // for each unpack call; this is the canonical pattern.
      auto t_dec = ns_per_iter(0u, [&](std::size_t) {
         auto p = mp_bench::decode<T>(bytes.data(), bytes.size());
         asm volatile("" : : "r"(&p) : "memory");
      });
      record("decode", t_dec.min_ns, t_dec.median_ns, cv(t_dec),
             wire, t_dec.iters, t_dec.trials);

      // No native validate — `unpack` IS the validation; report skipped.
   }
#endif

   //  Run all psio formats against one shape value.  Each cell is
   //  gated by fmt_supports<Fmt, T> so unsupported pairs are skipped
   //  rather than triggering a static_assert at compile time.
   template <typename T>
   void run_shape(std::vector<snapshot_row>& out,
                  const std::string&         shape_name,
                  const T&                   v)
   {
      auto cell = [&]<typename Fmt>(const std::string& fmt_name, Fmt fmt) {
         if constexpr (fmt_supports<Fmt, T>::value)
            bench_psio_cell(out, shape_name, fmt_name, fmt, v);
      };
      cell("ssz",      psio::ssz{});
      cell("pssz",     psio::pssz{});
      cell("frac32",   psio::frac32{});
      cell("bin",      psio::bin{});
      cell("borsh",    psio::borsh{});
      cell("bincode",  psio::bincode{});
      cell("avro",     psio::avro{});
      cell("protobuf", psio::protobuf{});
      cell("msgpack",  psio::msgpack{});
      cell("capnp",    psio::capnp{});
      cell("flatbuf",  psio::flatbuf{});

#ifdef PSIO_HAVE_MSGPACK
      bench_msgpack_cxx_cell(out, shape_name, v);
#endif
   }

}  // namespace

int main(int argc, char** argv)
{
   const auto plat = detect_platform_info();

   std::printf("# psio vs externals — perf snapshot\n\n");
   std::printf("commit: %s, os: %s, arch: %s, compiler: %s, "
               "build: %s\n\n",
               plat.commit_short.c_str(), plat.os.c_str(),
               plat.arch.c_str(), plat.compiler.c_str(),
               plat.build_type.c_str());

   std::vector<snapshot_row> rows;

   // Tier 1-7 + bounded
   run_shape(rows, "Point",                psio_bench::point());
   run_shape(rows, "NameRecord",           psio_bench::namerec());
   run_shape(rows, "FlatRecord",           psio_bench::flatrec());
   run_shape(rows, "Record",               psio_bench::record());
   run_shape(rows, "Validator",            psio_bench::validator());
   run_shape(rows, "Order",                psio_bench::order());
   run_shape(rows, "ValidatorList(100)",   psio_bench::vlist(100));
   run_shape(rows, "FlatRecordBounded",    psio_bench::flatrec_bounded());
   run_shape(rows, "RecordBounded",        psio_bench::record_bounded());
   run_shape(rows, "OrderBounded",         psio_bench::order_bounded());
   run_shape(rows, "ValidatorListBounded(100)",
             psio_bench::vlist_bounded(100));

   // Where to write the snapshot.  Default: bench_snapshots/.  Caller
   // can override via PSIO_BENCH_SNAPSHOT_DIR or argv[1].
   std::string snap_dir = "bench_snapshots";
   if (const char* env = std::getenv("PSIO_BENCH_SNAPSHOT_DIR");
       env && *env)
      snap_dir = env;
   if (argc > 1)
      snap_dir = argv[1];

   std::filesystem::create_directories(snap_dir);
   std::string snap_name = "perf_" + plat.timestamp_utc + "_" +
                            plat.commit_short + ".csv";
   std::filesystem::path snap_path =
      std::filesystem::path{snap_dir} / snap_name;

   std::ofstream snap{snap_path};
   if (!snap)
   {
      std::fprintf(stderr, "failed to open %s for writing\n",
                    snap_path.c_str());
      return 1;
   }
   write_snapshot_csv(snap, plat, rows);
   snap.close();

   std::printf("wrote %zu rows to %s\n", rows.size(),
               snap_path.c_str());
   return 0;
}
