# `wasi_compat_cxx/` — libc++ threading shims

These headers let LLVM's source tree compile against wasi-sdk's **no-threads**
libc++ (`wasm32-wasip1`) without patching LLVM.

## The problem

LLVM's `<Support/*>` and `<DebugInfo/*>` headers unconditionally include
`<mutex>`, `<condition_variable>`, and `<shared_mutex>`, and a handful of them
declare `std::mutex` / `std::condition_variable` members in struct bodies that
aren't themselves guarded by `LLVM_ENABLE_THREADS`.

Under `wasm32-wasip1`, wasi-sdk's libc++ is configured with no thread support.
In that mode:

| Header | Behavior |
|---|---|
| `<mutex>` | Loads; provides `lock_guard`, `unique_lock`, `once_flag`; **omits** `std::mutex`. |
| `<condition_variable>` | Emits `#error "...not supported..."`. |
| `<shared_mutex>` | Emits `#error "...not supported..."`. |

So compilation fails even though `LLVM_ENABLE_THREADS=OFF` is set at the CMake
layer — the failure is in type lookup / `#error`, not in code that actually
executes.

## The shim

This directory ships transparent replacements for the three headers. They are
put ahead of the wasi-sysroot's libc++ include path via `-isystem` in
`wasi-llvm-toolchain.cmake`. Each shim uses `#include_next` to pass through to
the real libc++ header when the real one works, and substitutes no-op stubs
only when libc++ is configured without threads.

Concretely:

- **Threads enabled** (e.g. host builds, `wasm32-wasip1-threads`): the shim is
  a pure passthrough. `#include_next <mutex>` delivers the real `std::mutex`,
  and no stub types are defined. Zero runtime cost.
- **Threads disabled** (`wasm32-wasip1`): the shim supplies minimal no-op
  `std::mutex`, `std::condition_variable`, `std::shared_mutex`, and
  `std::shared_lock` types that satisfy the compiler's type-system
  requirements. These stubs are only used as struct fields and inside code
  paths that `LLVM_ENABLE_THREADS=OFF` already ensures never execute. If one
  of them is ever called at runtime, the behavior is a no-op (not undefined),
  so the program stays well-formed.

## When to remove

Delete this directory (and the `-isystem` line in the toolchain file) once
either:

1. LLVM upstream properly guards all `<mutex>` / `<condition_variable>` /
   `<shared_mutex>` usage behind `LLVM_ENABLE_THREADS`, *or*
2. This project switches to the `wasm32-wasip1-threads` sysroot.
