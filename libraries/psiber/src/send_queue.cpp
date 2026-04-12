#include <psiber/detail/send_queue.hpp>

#include <cstdlib>
#include <stdexcept>

namespace psiber::detail
{
   SendQueue::SendQueue(uint32_t ring_bytes)
      : _ring_bytes(align_up(ring_bytes)), _free_bytes(align_up(ring_bytes))
   {
      // Aligned allocation for the ring buffer
      _ring = static_cast<char*>(std::aligned_alloc(alignment, _ring_bytes));
      if (!_ring)
         throw std::bad_alloc();
   }

   SendQueue::~SendQueue()
   {
      // Destroy any unconsumed tasks still in the ring
      while (_free_bytes.load(std::memory_order_relaxed) < _ring_bytes)
      {
         auto* slot = reinterpret_cast<TaskSlotHeader*>(_ring + _tail);
         if (slot->destroy)
         {
            void* payload = slot + 1;
            slot->destroy(payload);
         }
         uint32_t size = slot->total_size;
         _tail         = (_tail + size) % _ring_bytes;
         _free_bytes.fetch_add(size, std::memory_order_relaxed);
      }
      std::free(_ring);
   }

   TaskSlotHeader* SendQueue::alloc(uint32_t payload_size)
   {
      uint32_t total = align_up(static_cast<uint32_t>(sizeof(TaskSlotHeader)) + payload_size);

      // Quick check: is there enough total free space?
      uint32_t free = _free_bytes.load(std::memory_order_relaxed);
      if (free < total)
         return nullptr;

      // Try to allocate at _head
      uint32_t remaining_at_end = _ring_bytes - _head;

      if (total <= remaining_at_end)
      {
         // Fits contiguously at _head
         auto* slot       = reinterpret_cast<TaskSlotHeader*>(_ring + _head);
         slot->total_size = total;
         _head            = (_head + total) % _ring_bytes;
         _free_bytes.fetch_sub(total, std::memory_order_relaxed);
         return slot;
      }

      // Doesn't fit at end — need to wrap around.
      // We waste the remaining bytes at the end by placing a sentinel.
      uint32_t waste = remaining_at_end;
      if (free < waste + total)
         return nullptr;  // not enough space even with wrap

      // Place a zero-size sentinel to mark the wasted region
      if (waste >= sizeof(TaskSlotHeader))
      {
         auto* sentinel       = reinterpret_cast<TaskSlotHeader*>(_ring + _head);
         sentinel->total_size = waste;
         sentinel->run        = nullptr;
         sentinel->destroy    = nullptr;
         sentinel->consumed.store(true, std::memory_order_relaxed);  // reclaimable
      }

      _free_bytes.fetch_sub(waste, std::memory_order_relaxed);
      _head = 0;

      // Now allocate at position 0
      auto* slot       = reinterpret_cast<TaskSlotHeader*>(_ring);
      slot->total_size = total;
      _head            = total;
      _free_bytes.fetch_sub(total, std::memory_order_relaxed);
      return slot;
   }

   void SendQueue::reclaim()
   {
      while (_free_bytes.load(std::memory_order_relaxed) < _ring_bytes)
      {
         auto* slot = reinterpret_cast<TaskSlotHeader*>(_ring + _tail);
         if (!slot->consumed.load(std::memory_order_acquire))
            break;  // not yet consumed — stop here

         // Destroy the functor if it has one (sentinels have destroy == nullptr)
         if (slot->destroy)
         {
            void* payload = slot + 1;
            slot->destroy(payload);
            slot->destroy = nullptr;
         }

         uint32_t size = slot->total_size;
         _tail         = (_tail + size) % _ring_bytes;
         _free_bytes.fetch_add(size, std::memory_order_relaxed);
      }
   }

   bool SendQueue::hasSpace(uint32_t payload_size) const
   {
      uint32_t total = align_up(static_cast<uint32_t>(sizeof(TaskSlotHeader)) + payload_size);
      return _free_bytes.load(std::memory_order_relaxed) >= total;
   }

}  // namespace psiber::detail
