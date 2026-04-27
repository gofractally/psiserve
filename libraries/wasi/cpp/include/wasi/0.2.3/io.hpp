#pragma once

// wasi:io@0.2.3 — error, poll, and streams interfaces.
//
// Canonical WIT sources:
//   libraries/wasi/wit/0.2.3/io/error.wit
//   libraries/wasi/wit/0.2.3/io/poll.wit
//   libraries/wasi/wit/0.2.3/io/streams.wit
//
// These C++ declarations mirror the WIT through PSIO structural
// metadata. The inline stubs return defaults and are never called
// at runtime — psiserve's Linker wires the imports to host_function
// closures before instantiation.

#include <psio/reflect.hpp>
#include <psio/structural.hpp>
#include <psio/wit_resource.hpp>

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

// =====================================================================
// wasi:io/error — resource error
// =====================================================================

struct io_error : psio::wit_resource {};
PSIO_REFLECT(io_error)

// =====================================================================
// wasi:io/poll — resource pollable, free function poll
// =====================================================================

struct pollable : psio::wit_resource {};
PSIO_REFLECT(pollable)

// =====================================================================
// wasi:io/streams — stream-error variant, input-stream, output-stream
// =====================================================================

struct stream_error
{
   enum tag_t : uint8_t
   {
      last_operation_failed,
      closed
   };
   tag_t tag;
   // Payload valid only when tag == last_operation_failed.
   // Stores the handle of the associated io_error resource.
   uint32_t error_handle = 0;
};
PSIO_REFLECT(stream_error, tag, error_handle)

struct input_stream : psio::wit_resource {};
PSIO_REFLECT(input_stream)

struct output_stream : psio::wit_resource {};
PSIO_REFLECT(output_stream)

// ── WIT result<T, E> as std::variant ────────────────────────────────
// WIT result<ok-type, err-type> maps to std::variant<OkType, ErrType>:
//   index 0 = ok (success payload)
//   index 1 = err (error payload)
// This lets the canonical ABI's variant lowering handle it directly.

template <typename T>
using wasi_result = std::variant<T, stream_error>;

using wasi_result_void = std::variant<std::monostate, stream_error>;

namespace wasi_io_detail {
   template <typename T>
   wasi_result<T> ok(T value) { return wasi_result<T>{std::in_place_index<0>, std::move(value)}; }

   inline wasi_result_void ok() { return wasi_result_void{std::in_place_index<0>}; }

   template <typename T>
   wasi_result<T> err(stream_error e) { return wasi_result<T>{std::in_place_index<1>, e}; }

   inline wasi_result_void err(stream_error e) { return wasi_result_void{std::in_place_index<1>, e}; }
}

// =====================================================================
// Interface: wasi:io/error
// =====================================================================

struct wasi_io_error
{
   // [method] error.to-debug-string: func() -> string
   static inline std::vector<uint8_t> error_to_debug_string(
       psio::borrow<io_error> /*self*/)
   {
      return {};
   }
};

// =====================================================================
// Interface: wasi:io/poll
// =====================================================================

struct wasi_io_poll
{
   // [method] pollable.ready: func() -> bool
   static inline bool pollable_ready(psio::borrow<pollable> /*self*/)
   {
      return false;
   }

   // [method] pollable.block: func()
   static inline void pollable_block(psio::borrow<pollable> /*self*/) {}

   // poll: func(in: list<borrow<pollable>>) -> list<u32>
   static inline std::vector<uint32_t> poll(
       std::vector<psio::borrow<pollable>> /*in*/)
   {
      return {};
   }
};

// =====================================================================
// Interface: wasi:io/streams
// =====================================================================

struct wasi_io_streams
{
   // ── input-stream methods ──────────────────────────────────────────

   static inline wasi_result<std::vector<uint8_t>> input_stream_read(
       psio::borrow<input_stream>, uint64_t)
   {
      return wasi_io_detail::err<std::vector<uint8_t>>({stream_error::closed});
   }

   static inline wasi_result<std::vector<uint8_t>> input_stream_blocking_read(
       psio::borrow<input_stream>, uint64_t)
   {
      return wasi_io_detail::err<std::vector<uint8_t>>({stream_error::closed});
   }

   static inline wasi_result<uint64_t> input_stream_skip(
       psio::borrow<input_stream>, uint64_t)
   {
      return wasi_io_detail::err<uint64_t>({stream_error::closed});
   }

   static inline wasi_result<uint64_t> input_stream_blocking_skip(
       psio::borrow<input_stream>, uint64_t)
   {
      return wasi_io_detail::err<uint64_t>({stream_error::closed});
   }

   static inline psio::own<pollable> input_stream_subscribe(
       psio::borrow<input_stream>)
   {
      return psio::own<pollable>{0};
   }

   // ── output-stream methods ─────────────────────────────────────────

   static inline wasi_result<uint64_t> output_stream_check_write(
       psio::borrow<output_stream>)
   {
      return wasi_io_detail::err<uint64_t>({stream_error::closed});
   }

   static inline wasi_result_void output_stream_write(
       psio::borrow<output_stream>, std::vector<uint8_t>)
   {
      return wasi_io_detail::err({stream_error::closed});
   }

   static inline wasi_result_void output_stream_blocking_write_and_flush(
       psio::borrow<output_stream>, std::vector<uint8_t>)
   {
      return wasi_io_detail::err({stream_error::closed});
   }

   static inline wasi_result_void output_stream_flush(
       psio::borrow<output_stream>)
   {
      return wasi_io_detail::err({stream_error::closed});
   }

   static inline wasi_result_void output_stream_blocking_flush(
       psio::borrow<output_stream>)
   {
      return wasi_io_detail::err({stream_error::closed});
   }

   static inline psio::own<pollable> output_stream_subscribe(
       psio::borrow<output_stream>)
   {
      return psio::own<pollable>{0};
   }

   static inline wasi_result_void output_stream_write_zeroes(
       psio::borrow<output_stream>, uint64_t)
   {
      return wasi_io_detail::err({stream_error::closed});
   }

   static inline wasi_result_void output_stream_blocking_write_zeroes_and_flush(
       psio::borrow<output_stream>, uint64_t)
   {
      return wasi_io_detail::err({stream_error::closed});
   }

   static inline wasi_result<uint64_t> output_stream_splice(
       psio::borrow<output_stream>, psio::borrow<input_stream>, uint64_t)
   {
      return wasi_io_detail::err<uint64_t>({stream_error::closed});
   }

   static inline wasi_result<uint64_t> output_stream_blocking_splice(
       psio::borrow<output_stream>, psio::borrow<input_stream>, uint64_t)
   {
      return wasi_io_detail::err<uint64_t>({stream_error::closed});
   }
};

// =====================================================================
// Package and interface registration
// =====================================================================

PSIO_PACKAGE(wasi_io, "0.2.3");
#undef  PSIO_CURRENT_PACKAGE_
#define PSIO_CURRENT_PACKAGE_ PSIO_PACKAGE_TYPE_(wasi_io)

PSIO_INTERFACE(wasi_io_error,
               types(io_error),
               funcs(func(error_to_debug_string, self)))

PSIO_INTERFACE(wasi_io_poll,
               types(pollable),
               funcs(func(pollable_ready, self),
                     func(pollable_block, self),
                     func(poll, in)))

PSIO_INTERFACE(wasi_io_streams,
               types(stream_error, input_stream, output_stream),
               funcs(func(input_stream_read, self, len),
                     func(input_stream_blocking_read, self, len),
                     func(input_stream_skip, self, len),
                     func(input_stream_blocking_skip, self, len),
                     func(input_stream_subscribe, self),
                     func(output_stream_check_write, self),
                     func(output_stream_write, self, contents),
                     func(output_stream_blocking_write_and_flush, self, contents),
                     func(output_stream_flush, self),
                     func(output_stream_blocking_flush, self),
                     func(output_stream_subscribe, self),
                     func(output_stream_write_zeroes, self, len),
                     func(output_stream_blocking_write_zeroes_and_flush, self, len),
                     func(output_stream_splice, self, src, len),
                     func(output_stream_blocking_splice, self, src, len)))
