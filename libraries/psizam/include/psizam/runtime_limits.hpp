#pragma once

// psizam/runtime_limits.hpp — typed-int aliases for runtime configuration
// and per-instance bounds.
//
// Every quantity that flows through `runtime_config`, `compile_policy`,
// and `instance_policy` is wrapped in a `ucc::typed_int<T, Tag>` so that
// callers cannot accidentally pass a page count where a stack-byte limit
// is expected, or mix a wall-clock duration with an instruction-count
// budget.
//
// Scope: WASM-execution bounds (pages, stack bytes, call depth) and host-
// side resource sizing (durations, byte counts). Gas costs / counters
// live in `psizam/gas.hpp` and reuse the same `ucc::typed_int` primitive
// via the `gas_units` alias once `psizam-gas-state-redesign` Phase B
// lands them.
//
// User-defined literals (`_pages`, `_sbytes`, `_calls`, `_ms`, `_bytes`,
// `_kb`, `_mb`, `_gb`) keep call sites readable:
//
//    instance_policy{
//       .memory = { .max_pages = 256_pages },
//       .stack  = { .limit     = 64_kb_sbytes },
//       .timeout = { .wall     = 200_ms },
//    };
//
// All operators are constexpr; aliases are zero-cost over the underlying
// integer (typed_int is a packed wrapper, no virtual dispatch, no extra
// storage).

#include <ucc/typed_int.hpp>

#include <cstddef>
#include <cstdint>

namespace psizam {

// ═════════════════════════════════════════════════════════════════════
// WASM-side bounds
// ═════════════════════════════════════════════════════════════════════

// Linear-memory size in WASM pages (64 KiB each). Used by
// instance_policy::memory.max_pages and compile_policy::memory.max_pages_hint.
struct wasm_page_tag;
using wasm_pages = ucc::typed_int<uint32_t, wasm_page_tag>;

// Operand-stack ceiling in bytes. Used when compile_policy::stack.kind is
// `stack_kind::bytes`.
struct stack_byte_tag;
using stack_bytes = ucc::typed_int<uint32_t, stack_byte_tag>;

// Maximum call-stack depth (frames). Used when compile_policy::stack.kind
// is `stack_kind::call_count`.
struct call_depth_tag;
using call_depth = ucc::typed_int<uint32_t, call_depth_tag>;

// Function-table entry count ceiling.
struct table_entry_tag;
using table_entries = ucc::typed_int<uint32_t, table_entry_tag>;

// ═════════════════════════════════════════════════════════════════════
// Host-side time durations
// ═════════════════════════════════════════════════════════════════════

// Millisecond duration — wall-clock or runtime budgets, compile timeout,
// fiber yield deadlines.
struct ms_tag;
using ms_duration = ucc::typed_int<uint32_t, ms_tag>;

// Microsecond duration — for finer-grained timing queries that need more
// precision than ms_duration affords (e.g. instance::elapsed for short
// requests).
struct us_tag;
using us_duration = ucc::typed_int<uint64_t, us_tag>;

// ═════════════════════════════════════════════════════════════════════
// Host-side memory sizing
// ═════════════════════════════════════════════════════════════════════

// Generic byte count, used for things like JIT code-cache sizing and
// per-library size bounds.
struct host_byte_tag;
using host_bytes = ucc::typed_int<std::size_t, host_byte_tag>;

// Gigabyte count — coarse arena sizing for the runtime's reserved
// virtual-memory region (mem_safety::checked / unchecked).
struct host_gb_tag;
using host_gb = ucc::typed_int<std::size_t, host_gb_tag>;

// ═════════════════════════════════════════════════════════════════════
// User-defined literals
// ═════════════════════════════════════════════════════════════════════
//
// All UDLs are defined inside the `literals` inline namespace so callers
// can pull them in granularly with `using namespace psizam::literals;`
// or rely on ADL when literal types appear in psizam-namespace contexts.

inline namespace literals {

   constexpr wasm_pages    operator""_pages (unsigned long long v) noexcept {
      return wasm_pages{static_cast<uint32_t>(v)};
   }

   constexpr stack_bytes   operator""_sbytes(unsigned long long v) noexcept {
      return stack_bytes{static_cast<uint32_t>(v)};
   }

   constexpr call_depth    operator""_calls (unsigned long long v) noexcept {
      return call_depth{static_cast<uint32_t>(v)};
   }

   constexpr table_entries operator""_entries(unsigned long long v) noexcept {
      return table_entries{static_cast<uint32_t>(v)};
   }

   constexpr ms_duration   operator""_ms    (unsigned long long v) noexcept {
      return ms_duration{static_cast<uint32_t>(v)};
   }

   constexpr us_duration   operator""_us    (unsigned long long v) noexcept {
      return us_duration{v};
   }

   constexpr host_bytes    operator""_bytes (unsigned long long v) noexcept {
      return host_bytes{static_cast<std::size_t>(v)};
   }

   constexpr host_bytes    operator""_kb    (unsigned long long v) noexcept {
      return host_bytes{static_cast<std::size_t>(v) * 1024u};
   }

   constexpr host_bytes    operator""_mb    (unsigned long long v) noexcept {
      return host_bytes{static_cast<std::size_t>(v) * 1024u * 1024u};
   }

   constexpr host_gb       operator""_gb    (unsigned long long v) noexcept {
      return host_gb{static_cast<std::size_t>(v)};
   }

} // namespace literals

} // namespace psizam
