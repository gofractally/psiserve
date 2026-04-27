#pragma once

// wasi::list<T> — owning peer to wasi::string for canonical-ABI
// list<T>. The same layering tricks apply:
//
//   • Layout is the canonical-ABI list representation — {T* ptr,
//     size_t len} — so a guest export thunk reads two flat slots
//     and adopts, and release() hands the buffer back to the wire
//     without a copy.
//   • On __wasm__ the buffer lives in guest linear memory and is
//     allocated/freed via cabi_realloc (via psio/guest_alloc.hpp).
//     On the host, it's plain heap memory (malloc/free).
//   • Copy is deleted; move is cheap; destructor frees.
//
// v1 scope: T must be trivially-copyable (plain scalars and PODs).
// A record containing a wasi::string or wasi::list lowers to nested
// {ptr, len} pairs, which the backing buffer happily stores — but
// each nested allocation is a SEPARATE cabi_realloc call, and the
// ctor we expose here only reserves the top-level array. Producing
// nested-string lists goes through psio::canonical_lower_fields +
// the return-area policy in component_proxy.hpp (not this class).

#include <cstddef>
#include <cstring>
#include <span>
#include <type_traits>
#include <utility>

#ifdef __wasm__
extern "C" void* cabi_realloc(void* old_ptr, std::size_t old_size,
                              std::size_t align, std::size_t new_size);
#else
#include <cstdlib>
#endif

namespace wasi
{

template <typename T>
struct list
{
   static_assert(std::is_trivially_copyable_v<T>,
                 "wasi::list<T>: v1 only supports trivially-copyable T; "
                 "for records with nested strings/lists, use canonical "
                 "return-area lowering via psio::canonical_lower_fields");

 private:
   T*          _ptr = nullptr;
   std::size_t _len = 0;

 public:
   list() = default;

   // Allocate-only: reserve n elements; caller fills via data().
   explicit list(std::size_t n)
   {
      if (n == 0)
         return;
      _ptr = static_cast<T*>(alloc_bytes(n * sizeof(T)));
      _len = n;
   }

   // Deep-copy from a borrowed span.
   explicit list(std::span<const T> src)
   {
      if (src.empty())
         return;
      _ptr = static_cast<T*>(alloc_bytes(src.size() * sizeof(T)));
      std::memcpy(_ptr, src.data(), src.size() * sizeof(T));
      _len = src.size();
   }

   list(const list&)            = delete;
   list& operator=(const list&) = delete;

   list(list&& o) noexcept : _ptr(o._ptr), _len(o._len)
   {
      o._ptr = nullptr;
      o._len = 0;
   }
   list& operator=(list&& o) noexcept
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

   ~list() { free_bytes(_ptr, _len); }

   const T*    data() const noexcept { return _ptr; }
   T*          data() noexcept { return _ptr; }
   std::size_t size() const noexcept { return _len; }
   bool        empty() const noexcept { return _len == 0; }

   const T& operator[](std::size_t i) const noexcept { return _ptr[i]; }
   T&       operator[](std::size_t i) noexcept { return _ptr[i]; }

   const T* begin() const noexcept { return _ptr; }
   const T* end() const noexcept { return _ptr + _len; }
   T*       begin() noexcept { return _ptr; }
   T*       end() noexcept { return _ptr + _len; }

   operator std::span<const T>() const noexcept { return {_ptr, _len}; }

   struct raw_handoff
   {
      const T*    ptr;
      std::size_t len;
   };
   raw_handoff release() noexcept
   {
      raw_handoff r{_ptr, _len};
      _ptr = nullptr;
      _len = 0;
      return r;
   }

   // Take ownership of an existing (ptr, len) pair produced by the same
   // module's allocator. Used by canonical lift.
   static list adopt(const T* p, std::size_t n) noexcept
   {
      list l;
      l._ptr = const_cast<T*>(p);
      l._len = n;
      return l;
   }

 private:
#ifdef __wasm__
   static void* alloc_bytes(std::size_t n)
   {
      return cabi_realloc(nullptr, 0, alignof(T), n);
   }
   static void free_bytes(T* p, std::size_t n) noexcept
   {
      if (p)
         cabi_realloc(p, n * sizeof(T), alignof(T), 0);
   }
#else
   static void* alloc_bytes(std::size_t n) { return std::malloc(n); }
   static void  free_bytes(T* p, std::size_t) noexcept { std::free(p); }
#endif
};

static_assert(sizeof(list<int>) == sizeof(int*) + sizeof(std::size_t),
              "wasi::list<T> must match the canonical-ABI {ptr, len} layout");

}  // namespace wasi
