// Compile-time contiguous-bitwise-field run detector for reflected structs.
// Used by pack_bin and fracpack serializers to coalesce consecutive
// bitwise-serializable fields with no struct-internal padding into a single
// stream.write call, instead of one call per field.
//
// Requires the reflected type to be std::is_standard_layout (gated via
// reflect<T>::has_std_layout).  If not standard-layout, run detection is
// disabled and the caller falls back to per-field writes.

#pragma once

#include <psio1/reflect.hpp>
#include <psio1/stream.hpp>

#include <cstddef>
#include <tuple>
#include <type_traits>

namespace psio1::run_detail
{
   template <typename T, std::size_t I>
   consteval std::size_t nth_field_size()
   {
      return sizeof(std::tuple_element_t<I, struct_tuple_t<T>>);
   }

   template <typename T, std::size_t I>
   consteval bool nth_field_is_bitwise()
   {
      return has_bitwise_serialization<std::tuple_element_t<I, struct_tuple_t<T>>>();
   }

   // Is field at index I the start of a new run?
   // (Bitwise + either I==0 or prev field isn't bitwise/adjacent.)
   template <typename T, std::size_t I>
   consteval bool is_run_start()
   {
      if constexpr (!reflect<T>::has_std_layout)
         return false;
      else if constexpr (!nth_field_is_bitwise<T, I>())
         return false;
      else if constexpr (I == 0)
         return true;
      else if constexpr (!nth_field_is_bitwise<T, I - 1>())
         return true;
      else
      {
         constexpr std::size_t prev_off = reflect<T>::data_member_offsets[I - 1];
         constexpr std::size_t prev_sz  = nth_field_size<T, I - 1>();
         constexpr std::size_t my_off   = reflect<T>::data_member_offsets[I];
         return my_off != prev_off + prev_sz;
      }
   }

   // Is field I inside a run started at an earlier index?
   template <typename T, std::size_t I>
   consteval bool is_run_continuation()
   {
      if constexpr (I == 0)
         return false;
      else if constexpr (!nth_field_is_bitwise<T, I>())
         return false;
      else
         return !is_run_start<T, I>();
   }

   // Run length starting at I (recurses with growing K until hitting a break).
   template <typename T, std::size_t I, std::size_t K = 1>
   consteval std::size_t run_length_from()
   {
      constexpr std::size_t N = std::tuple_size_v<struct_tuple_t<T>>;
      if constexpr (I + K >= N)
         return K;
      else if constexpr (!nth_field_is_bitwise<T, I + K>())
         return K;
      else
      {
         constexpr auto&       offsets  = reflect<T>::data_member_offsets;
         constexpr std::size_t prev_off = offsets[I + K - 1];
         constexpr std::size_t prev_sz  = nth_field_size<T, I + K - 1>();
         constexpr std::size_t my_off   = offsets[I + K];
         if constexpr (my_off != prev_off + prev_sz)
            return K;
         else
            return run_length_from<T, I, K + 1>();
      }
   }

   // Bytes covered by the run starting at I with length L.
   template <typename T, std::size_t I, std::size_t L>
   consteval std::size_t run_total_bytes()
   {
      constexpr std::size_t start = reflect<T>::data_member_offsets[I];
      constexpr std::size_t end_ =
          reflect<T>::data_member_offsets[I + L - 1] + nth_field_size<T, I + L - 1>();
      return end_ - start;
   }

   // Does T have at least one run of length >= 2? When false the walker
   // offers no benefit — callers should stick with their per-field path to
   // avoid the walker's compile-time abstraction overhead.
   template <typename T, std::size_t... Is>
   consteval bool has_batchable_run_impl(std::index_sequence<Is...>)
   {
      return (... || (is_run_start<T, Is>() && run_length_from<T, Is>() >= 2));
   }

   template <typename T>
   consteval bool has_batchable_run()
   {
      if constexpr (!Reflected<T>)
         return false;
      else
      {
         constexpr std::size_t N = std::tuple_size_v<struct_tuple_t<T>>;
         if constexpr (N == 0)
            return false;
         else
            return has_batchable_run_impl<T>(std::make_index_sequence<N>{});
      }
   }

   // ── Generic walker ───────────────────────────────────────────────────
   //
   // walk_with_runs<T>(obj, stream, n_limit, non_bitwise_op)
   //
   // Walks T's reflected members in declaration order. For each run of
   // contiguous bitwise fields, emits one `stream.write(&obj+offset, bytes)`.
   // For non-bitwise fields, invokes `non_bitwise_op(obj.*member_ptr)` so the
   // caller controls per-format semantics (pack_bin's to_bin, fracpack's
   // embedded_fixed_pack, etc.). Fields at index >= n_limit are skipped
   // (used by extensible pack paths that trim trailing optionals).

   template <typename T, std::size_t I, typename S, typename Op>
   inline void visit_field(const T& obj, S& stream, int n_limit, Op& op)
   {
      if (static_cast<int>(I) >= n_limit)
         return;
      if constexpr (is_run_start<T, I>())
      {
         constexpr auto L      = run_length_from<T, I>();
         constexpr auto bytes  = run_total_bytes<T, I, L>();
         constexpr auto offset = reflect<T>::data_member_offsets[I];
         stream.write(reinterpret_cast<const char*>(&obj) + offset, bytes);
      }
      else if constexpr (is_run_continuation<T, I>())
      {
         // Part of a preceding run — already written.
      }
      else
      {
         constexpr auto mp = std::get<I>(reflect<T>::member_pointers());
         op(obj.*mp);
      }
   }

   template <typename T, typename S, typename Op, std::size_t... Is>
   inline void walk_with_runs_impl(const T& obj, S& stream, int n_limit, Op& op,
                                    std::index_sequence<Is...>)
   {
      (visit_field<T, Is>(obj, stream, n_limit, op), ...);
   }

   template <typename T, typename S, typename Op>
   inline void walk_with_runs(const T& obj, S& stream, int n_limit, Op&& op)
   {
      constexpr auto N = std::tuple_size_v<struct_tuple_t<T>>;
      walk_with_runs_impl<T>(obj, stream, n_limit, op, std::make_index_sequence<N>{});
   }
}  // namespace psio1::run_detail
