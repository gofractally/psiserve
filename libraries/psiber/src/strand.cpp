#include <psiber/strand.hpp>
#include <psiber/reactor.hpp>

#include <cassert>
#include <cstdlib>

namespace psiber
{
   using Fiber = detail::Fiber;

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

      // 2. First-fit scan of free list
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

      // 3. Bump pointer
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
         // Append to wait queue (FIFO)
         fiber->next_wake = nullptr;
         if (_wait_tail)
         {
            _wait_tail->next_wake = fiber;
            _wait_tail            = fiber;
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
      bool   should_post = false;

      _queue_lock.lock();

      _active.store(nullptr, std::memory_order_release);

      if (_wait_head)
      {
         next       = _wait_head;
         _wait_head = next->next_wake;
         if (!_wait_head)
            _wait_tail = nullptr;
         next->next_wake = nullptr;

         _active.store(next, std::memory_order_release);
         should_post = true;
      }

      _queue_lock.unlock();

      if (should_post)
         maybe_post();

      return next;
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
