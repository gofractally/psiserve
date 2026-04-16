#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

namespace psiber::detail
{
   /// Fixed-size slot pool backed by an atomic bitset freelist.
   ///
   /// ## How it works
   ///
   /// N slots are tracked by ceil(N/64) atomic<uint64_t> words.
   /// Bit set = slot free.  Bit clear = slot claimed.
   ///
   /// **Pop (try_pop):** Scan words left-to-right.  For each word,
   /// load the bits; if non-zero, `ctz` finds the lowest free slot.
   /// CAS clears that bit.  If the CAS fails (another caller claimed
   /// it), the `bits` variable is updated by the CAS — recompute and
   /// retry within the same word.  If the word is exhausted, move to
   /// the next.  Returns the slot index on success, nullopt on empty.
   ///
   /// **Push (push):** Convert the index to (word, bit).  `fetch_or`
   /// sets the bit, releasing the slot.  fetch_or is unconditional —
   /// no CAS loop needed because setting a bit is idempotent (the
   /// caller must guarantee the slot was claimed).
   ///
   /// ## Why it's correct
   ///
   /// - **No double-claim:** CAS on pop is atomic — two threads
   ///   targeting the same bit will have one succeed and one fail
   ///   (the failure retries on the updated word).
   /// - **No lost slots:** push uses fetch_or which always succeeds.
   ///   The slot bit transitions: 1 (free) → 0 (claimed via CAS) →
   ///   1 (freed via fetch_or).  Each transition is atomic.
   /// - **No ABA:** We're flipping individual bits, not swapping
   ///   pointers.  The bit value is the entire state — there's no
   ///   pointer indirection to go stale.
   /// - **Memory ordering:** `acq_rel` on pop's CAS ensures the
   ///   caller sees the slot's prior contents (if any).  `release`
   ///   on push ensures the slot's new contents are visible before
   ///   the bit is set.  `acquire` on the initial load in pop pairs
   ///   with the `release` in push.
   ///
   /// ## Performance characteristics
   ///
   /// - Pop: O(words) scan + O(1) CAS.  With 256 slots (4 words),
   ///   the scan is 4 loads in the worst case.
   /// - Push: O(1) fetch_or.  No contention unless the same word
   ///   is being freed concurrently (rare: fetch_or is RMW, not CAS).
   /// - No linked list, no ABA, no lock, no allocation.
   /// - Cache-friendly: the entire bitset fits in a single cache line.
   ///
   /// @tparam N  Number of slots.  Rounded up to a multiple of 64.
   ///
   /// Usage:
   /// ```
   ///   bitset_pool<256> pool;
   ///   auto idx = pool.try_pop();     // claim a slot
   ///   if (idx) use(slots[*idx]);
   ///   pool.push(*idx);               // return the slot
   /// ```
   template <uint32_t N>
   class bitset_pool
   {
     public:
      static constexpr uint32_t num_slots = N;
      static constexpr uint32_t num_words = (N + 63) / 64;

      bitset_pool() noexcept
      {
         // Set all N bits to 1 (free).  Partial last word gets only
         // the valid bits set (upper bits stay 0 = permanently claimed).
         for (uint32_t w = 0; w < num_words; ++w)
         {
            uint32_t bits_in_word = (w < num_words - 1)
                                      ? 64
                                      : (N - w * 64);
            uint64_t mask = (bits_in_word == 64)
                               ? ~uint64_t(0)
                               : (uint64_t(1) << bits_in_word) - 1;
            _bits[w].store(mask, std::memory_order_relaxed);
         }
      }

      /// Claim one free slot.  Returns the slot index, or nullopt
      /// if no slots are available.  Thread-safe, lock-free.
      std::optional<uint32_t> try_pop() noexcept
      {
         for (uint32_t w = 0; w < num_words; ++w)
         {
            uint64_t bits = _bits[w].load(std::memory_order_acquire);
            while (bits)
            {
               int      bit     = __builtin_ctzll(bits);
               uint64_t cleared = bits & ~(1ULL << bit);
               if (_bits[w].compare_exchange_weak(
                     bits, cleared,
                     std::memory_order_acq_rel,
                     std::memory_order_acquire))
               {
                  return w * 64 + static_cast<uint32_t>(bit);
               }
               // `bits` was updated by the failed CAS — retry
            }
         }
         return std::nullopt;
      }

      /// Release a previously claimed slot back to the pool.
      /// The caller must guarantee `idx` was returned by try_pop()
      /// and has not already been pushed back.
      void push(uint32_t idx) noexcept
      {
         uint32_t word = idx / 64;
         uint32_t bit  = idx % 64;
         _bits[word].fetch_or(1ULL << bit, std::memory_order_release);
      }

     private:
      alignas(64) std::atomic<uint64_t> _bits[num_words];
   };

}  // namespace psiber::detail
