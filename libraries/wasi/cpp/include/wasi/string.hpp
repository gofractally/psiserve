#pragma once

// wasi::string — owning byte buffer whose memory layout is the WIT
// canonical ABI `string` representation: {const char* ptr, size_t len}.
// Because the layout *is* canonical, a guest export thunk can adopt the
// two flat-ABI slots directly (no lift, no copy) via `adopt(ptr, len)`;
// similarly, a return value can be handed to the host through the
// canonical ABI by calling `release()` to surrender the buffer.
//
// Design notes:
//   • Composition over inheritance. Deriving from std::string_view is
//     tempting for the implicit conversions, but string_view has a
//     trivial copy-ctor and non-virtual destructor — shallow copies
//     would silently double-free. A plain struct gives us guaranteed
//     layout and lets us own copy/move correctly.
//   • Borrowed form is `std::string_view`. No parallel `wasi::view`
//     type; string_view already has the right layout and is what
//     authors reach for.
//   • operator std::string_view() is free (reads the two members).
//
// Allocator split:
//   • __wasm__ guest — buffer lives in the guest's linear memory and
//     is produced/freed through `cabi_realloc`. Calling TU must also
//     include <psio1/guest_alloc.hpp> (single-TU export) so the symbol
//     resolves. wasi/string.hpp only forward-declares cabi_realloc to
//     avoid dragging the export definition into every TU.
//   • Host (native build) — buffer lives in host heap, backed by
//     malloc/free. This is what makes wasi::string usable in
//     host-side unit tests and in shared host/guest contract headers.
//
// Ownership rules:
//   • Copy is deleted. Use `wasi::string{sv}` to deep-copy from a view.
//   • Move transfers ownership; destruct on a moved-from instance is
//     a no-op.
//   • Destructor frees the buffer via the platform's dealloc path.

#include <cstddef>
#include <cstring>
#include <string_view>
#include <utility>

#ifdef __wasm__
extern "C" void* cabi_realloc(void* old_ptr, std::size_t old_size,
                              std::size_t align, std::size_t new_size);
#else
#include <cstdlib>
#endif

namespace wasi
{

struct string
{
   // Members are in canonical-ABI order on purpose: a {const char*,
   // size_t} in linear memory *is* the lowered form. Exposed as
   // private-ish via the API below; the raw pair is only accessed by
   // release() and adopt() to avoid callers reaching in.
 private:
   const char* _ptr = nullptr;
   std::size_t _len = 0;

 public:
   string() = default;

   // Deep-copy from a view. Allocates a fresh buffer.
   explicit string(std::string_view v)
   {
      if (v.empty())
         return;
      char* buf = static_cast<char*>(alloc_bytes(v.size()));
      std::memcpy(buf, v.data(), v.size());
      _ptr = buf;
      _len = v.size();
   }

   // Allocate-only ctor. Reserves an n-byte buffer the caller fills via
   // the non-const data() accessor. Intended for canonical-ABI return
   // paths where the guest knows the final size up front and wants to
   // write into the buffer directly without a staging copy.
   explicit string(std::size_t n)
   {
      if (n == 0)
         return;
      _ptr = static_cast<char*>(alloc_bytes(n));
      _len = n;
   }

   // Copy forbidden — the allocator contract gives each buffer a
   // single owner; copying would need to go through alloc_bytes
   // explicitly, so spell it at the call site via `string{sv}`.
   string(const string&)            = delete;
   string& operator=(const string&) = delete;

   string(string&& o) noexcept : _ptr(o._ptr), _len(o._len)
   {
      o._ptr = nullptr;
      o._len = 0;
   }
   string& operator=(string&& o) noexcept
   {
      if (this != &o)
      {
         free_bytes(_ptr, _len);
         _ptr   = o._ptr;
         _len   = o._len;
         o._ptr = nullptr;
         o._len = 0;
      }
      return *this;
   }

   ~string() { free_bytes(_ptr, _len); }

   // Read-only access. Matches the std::string_view surface.
   const char* data() const noexcept { return _ptr; }
   std::size_t size() const noexcept { return _len; }
   bool        empty() const noexcept { return _len == 0; }

   // Mutable access. Only valid on buffers produced by string(size_t) or
   // string(string_view) — the owned allocation is writable, but a
   // string adopted from an external source shares whatever mutability
   // contract that source had.
   char* data() noexcept { return const_cast<char*>(_ptr); }

   operator std::string_view() const noexcept { return {_ptr, _len}; }

   // Surrender the buffer to a new owner (typically the canonical ABI
   // return-slot pair on a guest export). After release(), this
   // instance is empty and its destructor is a no-op.
   struct raw_handoff
   {
      const char* ptr;
      std::size_t len;
   };
   raw_handoff release() noexcept
   {
      raw_handoff r{_ptr, _len};
      _ptr = nullptr;
      _len = 0;
      return r;
   }

   // Take ownership of an existing buffer produced by the same
   // module's allocator. No copy, no allocation. Used by guest export
   // thunks to wrap the incoming canonical-ABI slots.
   static string adopt(const char* p, std::size_t n) noexcept
   {
      string s;
      s._ptr = p;
      s._len = n;
      return s;
   }

 private:
#ifdef __wasm__
   static void* alloc_bytes(std::size_t n)
   {
      return cabi_realloc(nullptr, 0, 1, n);
   }
   static void free_bytes(const char* p, std::size_t n) noexcept
   {
      if (p)
         cabi_realloc(const_cast<char*>(p), n, 1, 0);
   }
#else
   static void* alloc_bytes(std::size_t n) { return std::malloc(n); }
   static void  free_bytes(const char* p, std::size_t) noexcept
   {
      std::free(const_cast<char*>(p));
   }
#endif
};

static_assert(sizeof(string) == sizeof(const char*) + sizeof(std::size_t),
              "wasi::string must match the canonical-ABI {ptr, len} layout");

}  // namespace wasi
