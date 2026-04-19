#pragma once

// handle_table.hpp — Fixed-capacity slab allocator for WIT resource handles.
//
// Maps u32 handles to host-owned objects. O(1) create/lookup/destroy.
// Zero heap allocation — the slab is embedded at a fixed capacity.
// Generation counters detect use-after-free. Capacity caps prevent
// resource exhaustion by malicious guests.
//
// Handle encoding: (index << 16) | (generation & 0xFFFF)
// - index: position in the slab array
// - generation: incremented on each reuse, detects stale handles

#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

namespace psizam {

template <typename T, uint32_t Capacity = 256>
class handle_table {
   static_assert(Capacity <= 0xFFFF, "Capacity must fit in 16 bits");

   struct slot {
      alignas(T) char storage[sizeof(T)];
      uint32_t next_free = 0;
      uint16_t generation = 0;
      bool     occupied   = false;

      T&       value()       { return *reinterpret_cast<T*>(storage); }
      const T& value() const { return *reinterpret_cast<const T*>(storage); }
   };

   slot     slots_[Capacity];
   uint32_t free_head_  = 0;
   uint32_t live_count_ = 0;
   uint32_t max_live_;

   static uint32_t pack(uint32_t index, uint16_t gen) {
      return (index << 16) | gen;
   }
   static uint32_t unpack_index(uint32_t handle) { return handle >> 16; }
   static uint16_t unpack_gen(uint32_t handle)   { return handle & 0xFFFF; }

public:
   static constexpr uint32_t invalid_handle = UINT32_MAX;
   static constexpr uint32_t capacity       = Capacity;

   explicit handle_table(uint32_t max_live = Capacity)
      : max_live_(max_live)
   {
      for (uint32_t i = 0; i < Capacity; ++i)
         slots_[i].next_free = i + 1;
   }

   ~handle_table() { destroy_all(); }

   handle_table(const handle_table&) = delete;
   handle_table& operator=(const handle_table&) = delete;

   // ── Create ──────────────────────────────────────────────────────
   // Returns a handle, or invalid_handle if at capacity.
   template <typename... Args>
   uint32_t create(Args&&... args) {
      if (live_count_ >= max_live_ || free_head_ >= Capacity)
         return invalid_handle;

      uint32_t idx = free_head_;
      auto& s = slots_[idx];
      free_head_ = s.next_free;

      new (s.storage) T(std::forward<Args>(args)...);
      s.occupied = true;
      ++live_count_;

      return pack(idx, s.generation);
   }

   // ── Lookup ──────────────────────────────────────────────────────
   // Returns nullptr if handle is invalid or stale.
   T* get(uint32_t handle) {
      uint32_t idx = unpack_index(handle);
      uint16_t gen = unpack_gen(handle);
      if (idx >= Capacity) return nullptr;
      auto& s = slots_[idx];
      if (!s.occupied || s.generation != gen) return nullptr;
      return &s.value();
   }

   const T* get(uint32_t handle) const {
      uint32_t idx = unpack_index(handle);
      uint16_t gen = unpack_gen(handle);
      if (idx >= Capacity) return nullptr;
      auto& s = slots_[idx];
      if (!s.occupied || s.generation != gen) return nullptr;
      return &s.value();
   }

   // ── Destroy ─────────────────────────────────────────────────────
   // Returns true if the handle was valid and destroyed.
   bool destroy(uint32_t handle) {
      uint32_t idx = unpack_index(handle);
      uint16_t gen = unpack_gen(handle);
      if (idx >= Capacity) return false;
      auto& s = slots_[idx];
      if (!s.occupied || s.generation != gen) return false;

      s.value().~T();
      s.occupied = false;
      s.generation++;
      s.next_free = free_head_;
      free_head_ = idx;
      --live_count_;
      return true;
   }

   // ── Bulk cleanup ────────────────────────────────────────────────
   // Destroy all live resources. Called on transaction exit.
   void destroy_all() {
      for (uint32_t i = 0; i < Capacity; ++i) {
         if (slots_[i].occupied) {
            slots_[i].value().~T();
            slots_[i].occupied = false;
            slots_[i].generation++;
         }
      }
      live_count_ = 0;
      for (uint32_t i = 0; i < Capacity; ++i)
         slots_[i].next_free = i + 1;
      free_head_ = 0;
   }

   // ── Introspection ───────────────────────────────────────────────
   uint32_t live_count() const { return live_count_; }
   uint32_t max_live()   const { return max_live_; }
   bool     full()       const { return live_count_ >= max_live_; }
};

} // namespace psizam
