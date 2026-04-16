#pragma once

#include <atomic>
#include <cstdint>

namespace psiber::detail
{
   /// Lock-free multi-producer stack with sentinel-locked pop.
   ///
   /// ## How it works
   ///
   /// An intrusive singly-linked stack where push is a standard CAS
   /// loop and pop uses a sentinel value (0x01) to briefly "lock" the
   /// head while unlinking one node.
   ///
   /// **Push (any thread):** Same CAS-push as mpsc_stack, except
   /// pushers must spin past the sentinel.  If the head is 0x01 (a
   /// popper is mid-operation), the pusher spins with a CPU pause
   /// instruction until the popper stores the remainder.  This spin
   /// lasts nanoseconds — the popper only unlinks one node.
   ///
   /// **Pop (any thread):** CAS the head from a valid pointer to 0x01
   /// (the sentinel).  This gives the popper exclusive access to the
   /// list.  It takes the first node, then stores the rest back via
   /// a plain `store`.  If the head is already 0x01 or nullptr, the
   /// pop fails immediately (no spin — let the other popper finish).
   ///
   /// ## Why sentinel instead of exchange-drain?
   ///
   /// exchange-drain (mpsc_stack) takes the *entire* list.  That's
   /// correct for single-consumer drains (scheduler run loop), but
   /// wrong for the reactor's ready-strand queue where multiple
   /// workers pop one item each.  Sentinel-locked pop gives each
   /// worker exactly one strand, leaves the rest for other workers.
   ///
   /// ## Why not a CAS-pop (compare-exchange head with head->next)?
   ///
   /// Classic CAS-pop reads `head->next` before the CAS.  If another
   /// thread pops and frees `head` between the read and the CAS, the
   /// read is a use-after-free (ABA).  The sentinel avoids this: once
   /// a popper writes 0x01, no other thread can access the list, so
   /// `head->next` is safe to read.
   ///
   /// ## Why it's correct
   ///
   /// - **No ABA:** The sentinel eliminates the read-before-CAS race.
   ///   The popper has exclusive access between `CAS(head, 0x01)` and
   ///   `store(rest)`.
   /// - **No lost nodes:** Pushers that arrive during a pop spin on
   ///   the sentinel and retry their CAS after the popper stores the
   ///   remainder.  Their node ends up ahead of the remaining list.
   /// - **No deadlock:** Pop is non-blocking — if the CAS fails, it
   ///   returns nullptr immediately.  Push spins, but only for the
   ///   nanoseconds it takes to unlink one node and store.
   /// - **Memory ordering:** Pop's CAS uses `acquire` (sees producer
   ///   writes to the node).  Pop's store uses `release` (publishes
   ///   the updated list).  Push's CAS uses `release` (publishes the
   ///   new node).  Push's initial load is `relaxed` (the CAS
   ///   provides the synchronization).
   ///
   /// @tparam T        Node type
   /// @tparam NextPtr  Pointer-to-member for the intrusive next pointer.
   ///
   /// Usage:
   /// ```
   ///   sentinel_stack<strand, &strand::next_ready> ready_queue;
   ///   // Any thread:
   ///   ready_queue.push(strand_ptr);
   ///   // Any worker thread:
   ///   if (strand* s = ready_queue.try_pop()) { ... }
   /// ```
   template <typename T, T* T::*NextPtr>
   class sentinel_stack
   {
     public:
      sentinel_stack() = default;

      /// Push a node onto the stack.  Thread-safe, lock-free.
      /// Spins briefly if a popper holds the sentinel.
      void push(T* node) noexcept
      {
         T* old_head = _head.load(std::memory_order_relaxed);
         do
         {
            // Spin past the sentinel — a popper is mid-operation
            while (old_head == sentinel())
            {
#if defined(__x86_64__)
               __builtin_ia32_pause();
#elif defined(__aarch64__)
               asm volatile("yield" ::: "memory");
#endif
               old_head = _head.load(std::memory_order_relaxed);
            }
            node->*NextPtr = old_head;
         } while (!_head.compare_exchange_weak(
            old_head, node,
            std::memory_order_release, std::memory_order_relaxed));
      }

      /// Pop one node from the stack.  Thread-safe.
      /// Returns nullptr if empty or another thread is popping.
      /// Does not spin on contention — the caller should retry later.
      T* try_pop() noexcept
      {
         T* head = _head.load(std::memory_order_relaxed);

         // Empty or already locked by another popper
         if (head == nullptr || head == sentinel())
            return nullptr;

         // Lock the list with the sentinel
         if (!_head.compare_exchange_strong(
               head, sentinel(),
               std::memory_order_acquire, std::memory_order_relaxed))
            return nullptr;

         // Exclusive access — unlink the first node
         T* mine          = head;
         T* rest          = head->*NextPtr;
         mine->*NextPtr   = nullptr;

         // Unlock: store the remainder
         _head.store(rest, std::memory_order_release);

         return mine;
      }

      /// Check if the stack appears non-empty and unlocked.
      /// Relaxed load — for speculative checks only.
      bool probably_non_empty() const noexcept
      {
         auto* h = _head.load(std::memory_order_relaxed);
         return h != nullptr && h != sentinel();
      }

     private:
      static T* sentinel() noexcept
      {
         return reinterpret_cast<T*>(static_cast<uintptr_t>(1));
      }

      alignas(64) std::atomic<T*> _head{nullptr};
   };

}  // namespace psiber::detail
