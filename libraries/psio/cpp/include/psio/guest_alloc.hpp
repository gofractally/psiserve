#pragma once

// psio/guest_alloc.hpp — provides `cabi_realloc` for a WASM guest using
// a minimal bump allocator seeded at `__heap_base`. The canonical ABI
// gives the callee ownership of every allocation the host lowers into
// the guest's memory; this implementation satisfies the export without
// ever freeing. That is the right trade-off for short-lived request
// handlers (the forked instance is discarded at the end of the request)
// and for reactor modules whose working set fits inside the initial
// linear memory plus any growth we do here.
//
// SINGLE-TU HEADER: include this from exactly one translation unit in
// the guest. `cabi_realloc` is a non-inline extern "C" export, so
// including the header in multiple TUs will trip duplicate-symbol
// errors at link time.
//
// If the module needs a real allocator with free/reuse, don't include
// this header — delegate to libc instead:
//
//    extern "C" [[clang::export_name("cabi_realloc")]]
//    void* cabi_realloc(void* p, size_t, size_t, size_t n) {
//       return realloc(p, n);
//    }
//
// The host's bump-vs-real-malloc classification pass (see
// plans/wit-marshaling-design.md) inspects this export's bytecode to
// decide between single-alloc and per-field lowering, so picking the
// simplest implementation compatible with the module's needs matters.

#ifdef __wasm__

#include <stddef.h>
#include <stdint.h>

extern "C" {

// Linker-provided. `__heap_base` is the first byte past the guest's
// static data in linear memory; wasi-sdk's lld emits it automatically
// for wasm32 reactor and command modules alike.
extern unsigned char __heap_base;

[[clang::export_name("cabi_realloc")]]
void* cabi_realloc(void* old_ptr, size_t old_size, size_t align, size_t new_size)
{
   constexpr uint32_t page_size = 65536;

   // Lazily-initialized bump pointer. Keeping it in a function-local
   // static avoids needing a ctor — important since reactor mode's
   // `_initialize` runs global ctors in arbitrary order.
   static uintptr_t bump = 0;
   if (bump == 0)
      bump = reinterpret_cast<uintptr_t>(&__heap_base);

   if (new_size == 0)
      return nullptr;

   const uintptr_t a       = align ? static_cast<uintptr_t>(align) : 1u;
   const uintptr_t aligned = (bump + a - 1) & ~(a - 1);
   const uintptr_t next    = aligned + new_size;

   // Grow linear memory if the allocation would run past current size.
   const uint32_t  cur_pages = __builtin_wasm_memory_size(0);
   const uintptr_t cur_bytes = static_cast<uintptr_t>(cur_pages) * page_size;
   if (next > cur_bytes) {
      const uintptr_t gap        = next - cur_bytes;
      const uintptr_t gap_pages  = (gap + page_size - 1) / page_size;
      const uint32_t  need_pages = static_cast<uint32_t>(gap_pages);
      if (__builtin_wasm_memory_grow(0, need_pages) == static_cast<uint32_t>(-1))
         __builtin_trap();
   }

   bump = next;
   void* const new_ptr = reinterpret_cast<void*>(aligned);

   // On a grow-realloc the canonical ABI passes the old payload size so
   // the allocator can carry data across. Initial allocations pass
   // old_ptr=0, old_size=0, so this branch is skipped for those.
   if (old_ptr && old_size) {
      const size_t copy = old_size < new_size ? old_size : new_size;
      __builtin_memcpy(new_ptr, old_ptr, copy);
   }
   return new_ptr;
}

} // extern "C"

#endif // __wasm__
