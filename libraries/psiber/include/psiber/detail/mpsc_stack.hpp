#pragma once

#include <atomic>
#include <cstdint>

namespace psiber::detail
{
   /// Lock-free multi-producer single-consumer intrusive stack.
   ///
   /// ## How it works
   ///
   /// This is a classic Treiber stack adapted for the MPSC (many-push,
   /// one-drain) pattern used throughout the scheduler.  The core idea:
   ///
   /// **Push (any thread):** CAS-loop on the head pointer.  Each node's
   /// `next` pointer is set to the current head, then we CAS the head
   /// from old→node.  If the CAS fails (another thread pushed first),
   /// the loop retries with the updated head.  The `release` ordering
   /// on the CAS ensures the node's payload is visible to the consumer
   /// before the node appears in the list.  `relaxed` on the initial
   /// load is fine — the CAS itself provides the synchronization.
   ///
   /// **Drain (single consumer only):** An atomic `exchange(nullptr)`
   /// atomically detaches the entire list.  `acquire` ordering ensures
   /// the consumer sees all writes from all producers.  The result is
   /// a singly-linked list in LIFO order (most recent push at head).
   /// The caller reverses it for FIFO processing.
   ///
   /// ## Why it's correct
   ///
   /// - **No ABA problem:** push never reads `next` from the stack — it
   ///   writes a fresh `next` before CAS.  The CAS operand (head) is
   ///   only compared, not dereferenced, so stale pointers can't cause
   ///   corruption.
   /// - **Linearizability:** Each push linearizes at its successful CAS.
   ///   Each drain linearizes at its exchange.  Between drain and the
   ///   next push, the stack is empty.
   /// - **Memory visibility:** release-on-push + acquire-on-drain forms
   ///   a happens-before edge from each producer to the consumer.
   /// - **Single consumer:** drain() is not thread-safe against itself.
   ///   The scheduler calls drain from its run loop on one thread.
   ///
   /// @tparam T        Node type (must be a struct/class, not a pointer)
   /// @tparam NextPtr  Pointer-to-member for the intrusive next pointer.
   ///                  Must be `T* T::*` (e.g., `&Fiber::next_wake`).
   ///
   /// Usage:
   /// ```
   ///   mpsc_stack<Fiber, &Fiber::next_wake> wake_list;
   ///   // Producer thread:
   ///   wake_list.push(fiber_ptr);
   ///   // Consumer (scheduler thread):
   ///   Fiber* batch = wake_list.drain();  // LIFO-ordered chain
   /// ```
   template <typename T, T* T::*NextPtr>
   class mpsc_stack
   {
     public:
      mpsc_stack() = default;

      /// Push a node onto the stack.  Thread-safe, lock-free.
      /// The node must not already be in any list.
      void push(T* node) noexcept
      {
         T* old_head = _head.load(std::memory_order_relaxed);
         do
         {
            node->*NextPtr = old_head;
         } while (!_head.compare_exchange_weak(
            old_head, node,
            std::memory_order_release, std::memory_order_relaxed));
      }

      /// Atomically detach and return the entire list.
      /// Returns a LIFO-ordered chain (most recent push at head).
      /// The caller typically reverses it for FIFO processing.
      /// Single-consumer only — not safe to call from multiple threads.
      T* drain() noexcept
      {
         return _head.exchange(nullptr, std::memory_order_acquire);
      }

      /// Reverse a LIFO chain into FIFO order.
      /// Utility for the common drain→reverse→process pattern.
      static T* reverse(T* head) noexcept
      {
         T* reversed = nullptr;
         while (head)
         {
            T* next       = head->*NextPtr;
            head->*NextPtr = reversed;
            reversed       = head;
            head           = next;
         }
         return reversed;
      }

      /// Check if the stack appears non-empty.
      /// Relaxed load — for speculative checks (spin loops), not decisions.
      bool probably_non_empty() const noexcept
      {
         return _head.load(std::memory_order_acquire) != nullptr;
      }

     private:
      alignas(64) std::atomic<T*> _head{nullptr};
   };

}  // namespace psiber::detail
