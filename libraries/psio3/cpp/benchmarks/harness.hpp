#pragma once
//
// libraries/psio3/cpp/benchmarks/harness.hpp — timing + reporting helpers.
//
// Primitives:
//   - ns_per_iter(iters, F) — amortize a body's cost over N runs, best of
//     M trials. Returns ns/op for the fastest trial (least noise).
//   - result_row holds one (shape, format, library) measurement: encode
//     ns, decode ns, validate ns, size_of ns, wire bytes.
//   - report_table / report_csv emit a clean markdown + csv from a
//     vector<result_row>.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace psio3_bench {

   struct timing
   {
      double min_ns    = 0.0;
      double median_ns = 0.0;
   };

   // Run `body` `iters` times, repeat `trials` times, return the trial
   // with the lowest per-iter cost.
   template <typename F>
   timing ns_per_iter(std::size_t iters, F&& body, int trials = 5)
   {
      double best   = 0.0;
      double medval = 0.0;
      std::vector<double> samples;
      samples.reserve(trials);
      for (int t = 0; t < trials; ++t)
      {
         auto t0 = std::chrono::steady_clock::now();
         for (std::size_t i = 0; i < iters; ++i)
            body(i);
         auto   t1 = std::chrono::steady_clock::now();
         double per =
            std::chrono::duration<double, std::nano>(t1 - t0).count() /
            static_cast<double>(iters);
         samples.push_back(per);
         if (t == 0 || per < best)
            best = per;
      }
      std::sort(samples.begin(), samples.end());
      medval = samples[samples.size() / 2];
      return {best, medval};
   }

   // One measurement cell. `library` is "v1" / "v3" / competitor name.
   struct result_row
   {
      std::string shape;
      std::string format;
      std::string library;
      double      enc_ns_min = 0.0;
      double      dec_ns_min = 0.0;
      double      val_ns_min = 0.0;
      double      size_ns_min = 0.0;
      std::size_t wire_bytes  = 0;
   };

   // Find the paired v1/v3 rows (same shape + format) and compute the
   // v3/v1 ratio for each op. Returns 0.0 where the counterpart row is
   // missing (e.g., a format v1 doesn't support).
   struct pair_ratios
   {
      double enc = 0.0;
      double dec = 0.0;
      double val = 0.0;
      double sz  = 0.0;
      double wire = 0.0;
   };

   inline pair_ratios ratios(const result_row& a, const result_row& b) noexcept
   {
      auto div = [](double n, double d) { return d == 0.0 ? 0.0 : n / d; };
      return {div(a.enc_ns_min, b.enc_ns_min),
              div(a.dec_ns_min, b.dec_ns_min),
              div(a.val_ns_min, b.val_ns_min),
              div(a.size_ns_min, b.size_ns_min),
              (b.wire_bytes == 0 ? 0.0
                                 : static_cast<double>(a.wire_bytes) /
                                      static_cast<double>(b.wire_bytes))};
   }

   // ── Report writers ────────────────────────────────────────────────

   inline std::string format_ns(double ns)
   {
      char buf[32];
      if (ns == 0.0)
         std::snprintf(buf, sizeof(buf), "   n/a");
      else if (ns < 1000.0)
         std::snprintf(buf, sizeof(buf), "%6.1f", ns);
      else if (ns < 1e6)
         std::snprintf(buf, sizeof(buf), "%5.1fk", ns / 1000.0);
      else
         std::snprintf(buf, sizeof(buf), "%5.1fM", ns / 1e6);
      return buf;
   }

   inline std::string format_bytes(std::size_t n)
   {
      char buf[32];
      if (n < 1024)
         std::snprintf(buf, sizeof(buf), "%zuB", n);
      else if (n < 1024 * 1024)
         std::snprintf(buf, sizeof(buf), "%.1fK", n / 1024.0);
      else
         std::snprintf(buf, sizeof(buf), "%.2fM", n / (1024.0 * 1024.0));
      return buf;
   }

   inline std::string format_ratio(double r)
   {
      char buf[16];
      if (r == 0.0)
         std::snprintf(buf, sizeof(buf), "  —  ");
      else
         std::snprintf(buf, sizeof(buf), "%.2fx", r);
      return buf;
   }

   // Emit a markdown table grouped by (shape, format), with one row per
   // library and ratio rows. Writes to `out` (e.g., std::cout or a file
   // stream).
   template <typename Out>
   void report_table(Out& out, const std::vector<result_row>& rows)
   {
      // Gather unique (shape, format) pairs in insertion order.
      std::vector<std::pair<std::string, std::string>> pairs;
      for (const auto& r : rows)
      {
         auto key = std::make_pair(r.shape, r.format);
         if (std::find(pairs.begin(), pairs.end(), key) == pairs.end())
            pairs.push_back(key);
      }

      auto fmt = [&](std::string_view s) {
         out << s;
      };
      fmt("| shape | format | library | enc ns | dec ns | val ns | "
          "size ns | wire |\n");
      fmt("|---|---|---|---:|---:|---:|---:|---:|\n");

      for (const auto& [shape, format] : pairs)
      {
         for (const auto& r : rows)
         {
            if (r.shape == shape && r.format == format)
            {
               out << "| " << shape << " | " << format << " | "
                   << r.library << " | " << format_ns(r.enc_ns_min)
                   << " | " << format_ns(r.dec_ns_min) << " | "
                   << format_ns(r.val_ns_min) << " | "
                   << format_ns(r.size_ns_min) << " | "
                   << format_bytes(r.wire_bytes) << " |\n";
            }
         }
         // Ratio row for v3 vs v1 when both present.
         const result_row* v1 = nullptr;
         const result_row* v3 = nullptr;
         for (const auto& r : rows)
         {
            if (r.shape == shape && r.format == format)
            {
               if (r.library == "v1") v1 = &r;
               else if (r.library == "v3") v3 = &r;
            }
         }
         if (v1 && v3)
         {
            auto rt = ratios(*v3, *v1);
            out << "| " << shape << " | " << format
                << " | **v3/v1** | " << format_ratio(rt.enc) << " | "
                << format_ratio(rt.dec) << " | " << format_ratio(rt.val)
                << " | " << format_ratio(rt.sz) << " | "
                << format_ratio(rt.wire) << " |\n";
         }
      }
   }

   template <typename Out>
   void report_csv(Out& out, const std::vector<result_row>& rows)
   {
      out << "shape,format,library,enc_ns,dec_ns,val_ns,size_ns,wire_bytes\n";
      for (const auto& r : rows)
      {
         out << r.shape << "," << r.format << "," << r.library << ","
             << r.enc_ns_min << "," << r.dec_ns_min << ","
             << r.val_ns_min << "," << r.size_ns_min << ","
             << r.wire_bytes << "\n";
      }
   }

}  // namespace psio3_bench
