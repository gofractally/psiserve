#pragma once

#include <psiber/types.hpp>

#include <ucc/typed_int.hpp>

#include <chrono>
#include <cstdint>

namespace psiserve
{
   // ── File descriptor types ─────────────────────────────────────────────────
   //
   // psiserve maintains two distinct fd namespaces:
   //
   //   VirtualFd — the fd number visible to WASM code (0..255).
   //               Indexes into the per-process FdTable.
   //
   //   RealFd    — the OS kernel fd returned by socket()/accept()/etc.
   //               Defined in psiber (shared with the fiber scheduler).
   //
   // Using typed ints makes it a compile error to pass one where the other
   // is expected — you can't accidentally hand a VirtualFd to ::read() or
   // a RealFd to FdTable::get().

   using VirtualFd = ucc::typed_int<int, struct virtual_fd_tag>;

   // Re-export from psiber
   using psiber::RealFd;
   using psiber::invalid_real_fd;

   inline constexpr VirtualFd invalid_virtual_fd{-1};

   // ── WASM memory types ─────────────────────────────────────────────────────
   //
   //   WasmPtr  — byte offset into linear memory (not a host pointer)
   //   WasmSize — byte count within linear memory
   //
   // These prevent accidentally mixing host pointers, fd numbers, and
   // linear memory offsets at compile time.

   using WasmPtr  = ucc::typed_int<uint32_t, struct wasm_ptr_tag>;
   using WasmSize = ucc::typed_int<uint32_t, struct wasm_size_tag>;

   // ── PSI error codes ───────────────────────────────────────────────────────
   //
   // Platform-independent error codes returned to WASM by the psi.* API.
   // Values are stable across platforms (unlike POSIX errno).
   //
   // WASM sees: result >= 0 → success, result < 0 → -PsiError.

   enum class PsiError : int32_t
   {
      none         = 0,
      bad_fd       = 1,
      not_socket   = 2,
      too_many_fds = 3,
      io_failure   = 4,
      conn_refused = 5,
      conn_reset   = 6,
      broken_pipe  = 7,
      timed_out    = 8,
      would_block  = 9,
      unknown      = 10,
      count_               // not an error — marks the array size
   };

   // ── PsiResult ─────────────────────────────────────────────────────────────
   //
   // Return type for psi.* host functions.  Encodes either a non-negative
   // success value (fd number, byte count) or a PsiError.
   // Maps to a single i32 at the WASM boundary via wasm_type_traits.

   struct PsiResult
   {
      int32_t raw;

      /// Success: wrap a non-negative value (fd, byte count, etc.)
      static constexpr PsiResult ok(int32_t v) { return {v}; }

      /// Error: wrap a PsiError as a negative i32.
      static constexpr PsiResult err(PsiError e) { return {-static_cast<int32_t>(e)}; }

      /// Map a failed POSIX syscall (reads errno) to a PsiResult.
      static PsiResult fromErrno();

      constexpr bool     isOk() const { return raw >= 0; }
      constexpr bool     isErr() const { return raw < 0; }
      constexpr int32_t  value() const { return raw; }
      constexpr PsiError error() const
      {
         return isErr() ? static_cast<PsiError>(-raw) : PsiError::none;
      }

      /// Human-readable error string, or "ok" on success.
      const char* errorString() const;
   };

   // ── Network types ──────────────────────────────────────────────────────────

   using Port = ucc::typed_int<uint16_t, struct port_tag>;

   // ── Time types ────────────────────────────────────────────────────────────

   using SteadyTime = std::chrono::steady_clock::time_point;

}  // namespace psiserve

// ── wasm_type_traits registrations ───────────────────────────────────────────
//
// Teach psizam's fast trampoline to pass psiserve types as their underlying
// WASM-native scalars.  Must come after the type definitions above but before
// any host function that uses them.

#include <psizam/host_function.hpp>

namespace psizam
{
   // typed_int<T, Tag> → T
   template<typename T, typename Tag>
   struct wasm_type_traits<ucc::typed_int<T, Tag>> {
      static constexpr bool is_wasm_type = true;
      using wasm_type = T;
      static constexpr T                       unwrap(ucc::typed_int<T, Tag> v) { return *v; }
      static constexpr ucc::typed_int<T, Tag>  wrap(T v) { return ucc::typed_int<T, Tag>{v}; }
   };

   // PsiResult → int32_t
   template<>
   struct wasm_type_traits<psiserve::PsiResult> {
      static constexpr bool is_wasm_type = true;
      using wasm_type = int32_t;
      static constexpr int32_t              unwrap(psiserve::PsiResult r) { return r.raw; }
      static constexpr psiserve::PsiResult  wrap(int32_t v) { return {v}; }
   };
}
