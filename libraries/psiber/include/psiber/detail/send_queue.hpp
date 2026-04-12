#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>

namespace psiber::detail
{
   /// Header for a task slot in a SendQueue ring buffer.
   ///
   /// The sender placement-news a functor right after this header.
   /// The `next` pointer forms an intrusive MPSC linked list for the
   /// receiver's drain operation (CAS push onto receiver's head,
   /// exchange-null to drain, reverse for FIFO).
   struct alignas(16) TaskSlotHeader
   {
      uint32_t        total_size;  // header + payload, 16-byte aligned
      TaskSlotHeader* next;        // intrusive list pointer (receiver's drain list)
      void (*run)(void*);          // type-erased call (payload pointer)
      void (*destroy)(void*);      // type-erased destructor (payload pointer)
      std::atomic<bool> consumed{false};  // set by receiver when done
      bool heap_owned = false;     // if true, receiver frees after execution
   };

   static_assert(sizeof(TaskSlotHeader) <= 48);

   /// Sender-owned ring buffer for zero-allocation cross-thread task dispatch.
   ///
   /// The sender pre-allocates the ring.  Tasks are placement-new'd into the
   /// ring with a TaskSlotHeader prefix.  The receiver drains via an intrusive
   /// MPSC list (CAS push by sender, exchange-null by receiver), executes the
   /// task, and marks the slot `consumed`.  The sender reclaims consumed slots
   /// from the tail.
   ///
   /// Properties:
   /// - Zero malloc/free on the hot path
   /// - No cross-thread free (receiver marks consumed, sender reclaims)
   /// - Lock-free (CAS on receiver's list head, atomics for consumed)
   /// - Per-sender back pressure (full ring = sender must wait)
   class SendQueue
   {
     public:
      explicit SendQueue(uint32_t ring_bytes = 8192);
      ~SendQueue();

      SendQueue(const SendQueue&)            = delete;
      SendQueue& operator=(const SendQueue&) = delete;

      /// Emplace a callable into the ring buffer, returning a pointer to
      /// the TaskSlotHeader that can be CAS-pushed onto a receiver's intake
      /// list.  Returns nullptr if the ring is full (back pressure).
      template <typename F>
      TaskSlotHeader* emplace(F&& func);

      /// Reclaim consumed slots from the tail.  Call periodically on the
      /// sender thread (before emplace, or on a timer).
      void reclaim();

      /// Check if there's enough space for a payload of the given size.
      bool hasSpace(uint32_t payload_size) const;

      /// Total ring capacity in bytes.
      uint32_t capacity() const { return _ring_bytes; }

      /// Available bytes in the ring.
      uint32_t available() const { return _free_bytes.load(std::memory_order_relaxed); }

     private:
      TaskSlotHeader* alloc(uint32_t payload_size);

      static constexpr uint32_t alignment = 16;

      static constexpr uint32_t align_up(uint32_t n)
      {
         return (n + alignment - 1) & ~(alignment - 1);
      }

      char*                     _ring;
      uint32_t                  _ring_bytes;
      uint32_t                  _head = 0;
      uint32_t                  _tail = 0;
      std::atomic<uint32_t>     _free_bytes;
   };

   // ── Template implementation ───────────────────────────────────────────────

   template <typename F>
   TaskSlotHeader* SendQueue::emplace(F&& func)
   {
      using FuncType = std::decay_t<F>;
      static_assert(std::is_invocable_v<FuncType>, "SendQueue::emplace requires a callable");

      reclaim();

      TaskSlotHeader* slot = alloc(sizeof(FuncType));
      if (!slot)
         return nullptr;

      // Placement-new the functor right after the header
      void* payload = slot + 1;
      new (payload) FuncType(static_cast<F&&>(func));

      // Type-erased function pointers
      slot->run     = [](void* p) { (*static_cast<FuncType*>(p))(); };
      slot->destroy = [](void* p) { static_cast<FuncType*>(p)->~FuncType(); };
      slot->next    = nullptr;
      slot->consumed.store(false, std::memory_order_relaxed);

      return slot;
   }

}  // namespace psiber::detail
