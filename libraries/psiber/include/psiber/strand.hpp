#pragma once

#include <psiber/detail/fiber.hpp>
#include <psiber/spin_lock.hpp>
#include <psiber/types.hpp>

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>

namespace psiber
{
   class reactor;

   /// Serialization barrier with per-strand arena allocator.
   ///
   /// A strand guarantees at most one fiber in Ready/Running state at a
   /// time across all threads in a reactor pool.  Fibers are FIFO within
   /// a strand.  When the active fiber blocks or finishes, the next
   /// waiting fiber becomes active and the strand re-posts to the
   /// reactor's ready queue.
   ///
   /// Each strand owns a private arena for zero-allocation task storage.
   /// Free is lock-free (CAS push onto return stack).  Alloc uses a
   /// spin_lock to drain returned blocks, scan the free list, or bump.
   class strand
   {
      friend class reactor;

      using Fiber = detail::Fiber;

     public:
      explicit strand(uint32_t arena_bytes = 4096);
      strand(reactor& pool, uint32_t arena_bytes = 4096);
      ~strand();

      strand(const strand&)            = delete;
      strand& operator=(const strand&) = delete;
      strand(strand&&)                 = delete;
      strand& operator=(strand&&)      = delete;

      // ── Arena allocator ─────────────────────────────────────────

      /// Allocate `bytes` from the per-strand arena.
      /// Returns nullptr if the arena is exhausted.
      /// Thread-safe (spin_lock protected).
      void* alloc(uint32_t bytes);

      /// Free a pointer previously returned by alloc().
      /// Lock-free — any thread can call this (CAS push).
      void free(void* ptr);

      // ── Strand scheduling ──────────────────────────────────────

      /// Enqueue a fiber into this strand.
      /// If no fiber is currently active, the fiber becomes active.
      /// - If bound to a reactor: posts to the reactor's ready queue,
      ///   returns nullptr (reactor will schedule it).
      /// - If standalone: returns the fiber for the caller to enqueue
      ///   directly into its local ready queue.
      /// If another fiber is active, the fiber waits in the strand's
      /// queue and nullptr is returned.
      Fiber* enqueue(Fiber* fiber);

      /// Release the current active fiber (it blocked or finished).
      /// Returns the next waiting fiber, or nullptr if the strand is empty.
      /// If a fiber is returned, the strand re-posts to the ready queue.
      Fiber* release();

      /// The currently active fiber (or nullptr).
      Fiber* active() const { return _active.load(std::memory_order_acquire); }

     private:
      void maybe_post();

      // ── Arena internals ─────────────────────────────────────────

      struct Block
      {
         uint32_t size;  // total size: sizeof(Block) + payload, 16-aligned
         Block*   next;  // free-list linkage
      };

      char*    _region;
      uint32_t _capacity;
      uint32_t _bump = 0;

      // Lock-free return stack: any thread can free() via CAS push
      alignas(cache_line_size) std::atomic<Block*> _returned{nullptr};

      // Protected alloc state
      Block*    _free_list = nullptr;
      spin_lock _alloc_lock;

      // ── Scheduling internals ────────────────────────────────────

      // Currently active fiber (at most one across all pool threads)
      std::atomic<Fiber*> _active{nullptr};

      // Wait queue: fibers ready to run but blocked on _active
      Fiber*    _wait_head = nullptr;
      Fiber*    _wait_tail = nullptr;
      spin_lock _queue_lock;

      // Intrusive node for reactor's ready-strand queue
      strand* next_ready = nullptr;

      // Prevents double-posting to the ready queue
      std::atomic<bool> _queued{false};

      // Owning reactor (nullptr until bound)
      reactor* _pool = nullptr;
   };

}  // namespace psiber
