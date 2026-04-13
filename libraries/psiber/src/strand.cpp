#include <psiber/strand.hpp>
#include <psiber/reactor.hpp>

#include <cassert>
#include <cstdlib>

namespace psiber
{
   using Fiber      = detail::Fiber;
   using FiberState = detail::FiberState;

   strand::strand(uint32_t arena_bytes)
      : _capacity(arena_bytes)
   {
      _region = static_cast<char*>(std::aligned_alloc(16, arena_bytes));
      if (!_region)
         throw std::bad_alloc{};
   }

   strand::strand(reactor& pool, uint32_t arena_bytes)
      : strand(arena_bytes)
   {
      _pool = &pool;
   }

   strand::~strand()
   {
      std::free(_region);
   }

   // ── Arena allocator ─────────────────────────────────────────────────

   void* strand::alloc(uint32_t bytes)
   {
      uint32_t total = (sizeof(Block) + bytes + 15) & ~15u;

      _alloc_lock.lock();

      // 1. Drain returned blocks into the free list
      Block* ret = _returned.exchange(nullptr, std::memory_order_acquire);
      if (ret)
      {
         Block* tail = ret;
         while (tail->next)
            tail = tail->next;
         tail->next = _free_list;
         _free_list = ret;
      }

      // 2. Coalesce adjacent free blocks and first-fit scan.
      //    Sort free list by address so we can detect adjacency,
      //    then merge neighbors and search in a single pass.
      if (_free_list)
      {
         // Insertion sort by address (free lists are typically short)
         Block* sorted = nullptr;
         Block* cur    = _free_list;
         while (cur)
         {
            Block* next = cur->next;
            if (!sorted || cur < sorted)
            {
               cur->next = sorted;
               sorted    = cur;
            }
            else
            {
               Block* s = sorted;
               while (s->next && s->next < cur)
                  s = s->next;
               cur->next = s->next;
               s->next   = cur;
            }
            cur = next;
         }

         // Merge adjacent blocks
         cur = sorted;
         while (cur && cur->next)
         {
            auto* end_of_cur = reinterpret_cast<char*>(cur) + cur->size;
            if (end_of_cur == reinterpret_cast<char*>(cur->next))
            {
               cur->size += cur->next->size;
               cur->next  = cur->next->next;
               // Don't advance — check if we can merge with the next one too
            }
            else
            {
               cur = cur->next;
            }
         }
         _free_list = sorted;
      }

      // 3. First-fit scan of (now coalesced) free list
      Block** prev = &_free_list;
      for (Block* b = _free_list; b; prev = &b->next, b = b->next)
      {
         if (b->size >= total)
         {
            *prev = b->next;
            b->next = nullptr;
            _alloc_lock.unlock();
            return reinterpret_cast<char*>(b) + sizeof(Block);
         }
      }

      // 4. Bump pointer
      if (_bump + total > _capacity)
      {
         _alloc_lock.unlock();
         return nullptr;
      }

      auto* b = reinterpret_cast<Block*>(_region + _bump);
      b->size = total;
      b->next = nullptr;
      _bump += total;

      _alloc_lock.unlock();
      return reinterpret_cast<char*>(b) + sizeof(Block);
   }

   void strand::free(void* ptr)
   {
      if (!ptr)
         return;

      auto* b = reinterpret_cast<Block*>(static_cast<char*>(ptr) - sizeof(Block));

      // CAS push onto the returned stack (lock-free)
      Block* old = _returned.load(std::memory_order_relaxed);
      do
      {
         b->next = old;
      } while (!_returned.compare_exchange_weak(
         old, b, std::memory_order_release, std::memory_order_relaxed));
   }

   // ── Strand scheduling ───────────────────────────────────────────────

   Fiber* strand::enqueue(Fiber* fiber)
   {
      assert(fiber);
      bool became_active = false;

      _queue_lock.lock();

      if (_active.load(std::memory_order_relaxed) == nullptr)
      {
         _active.store(fiber, std::memory_order_release);
         became_active = true;
      }
      else
      {
         // Append to wait queue (FIFO).
         // Uses next_blocked (not next_wake) so the strand wait queue
         // doesn't collide with the scheduler's cross-thread wake list.
         fiber->next_blocked = nullptr;
         if (_wait_tail)
         {
            _wait_tail->next_blocked = fiber;
            _wait_tail               = fiber;
         }
         else
         {
            _wait_head = fiber;
            _wait_tail = fiber;
         }
      }

      _queue_lock.unlock();

      if (became_active)
      {
         if (_pool)
         {
            maybe_post();
            return nullptr;  // reactor will schedule it
         }
         return fiber;  // caller should enqueue locally
      }
      return nullptr;
   }

   Fiber* strand::release()
   {
      Fiber* next = nullptr;

      _queue_lock.lock();

      _active.store(nullptr, std::memory_order_release);

      // Pop waiters, skipping any that already finished (e.g.
      // during shutdown the scheduler independently resumes all
      // Parked fibers — they run, throw shutdown_exception, and
      // become Recyclable before release() pops them here).
      while (_wait_head)
      {
         Fiber* candidate = _wait_head;
         _wait_head       = candidate->next_blocked;
         if (!_wait_head)
            _wait_tail = nullptr;
         candidate->next_blocked = nullptr;

         if (candidate->state != FiberState::Recyclable &&
             candidate->state != FiberState::Done)
         {
            _active.store(candidate, std::memory_order_release);
            next = candidate;
            break;
         }
         // else: already finished — skip
      }

      _queue_lock.unlock();

      return next;
   }

   bool strand::enter(Fiber* fiber)
   {
      assert(fiber);

      _queue_lock.lock();

      if (_active.load(std::memory_order_relaxed) == nullptr)
      {
         _active.store(fiber, std::memory_order_release);
         _queue_lock.unlock();
         return true;  // became active — keep running
      }

      // Queue behind the active fiber (FIFO)
      fiber->next_blocked = nullptr;
      if (_wait_tail)
      {
         _wait_tail->next_blocked = fiber;
         _wait_tail               = fiber;
      }
      else
      {
         _wait_head = fiber;
         _wait_tail = fiber;
      }

      _queue_lock.unlock();
      return false;  // queued — caller should park
   }

   void strand::maybe_post()
   {
      if (!_queued.exchange(true, std::memory_order_acq_rel))
      {
         if (_pool)
            _pool->post_strand(this);
      }
   }

}  // namespace psiber
