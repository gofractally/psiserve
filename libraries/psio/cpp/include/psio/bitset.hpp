#pragma once
// Packed bit types with compile-time bounds (SSZ Bitvector / Bitlist analogs).
//
//   psio::bitvector<N>  — fixed-length N bits, packed (ceil(N/8) bytes)
//   psio::bitlist<N>    — variable-length 0..N bits, packed
//
// Both use LSB-first bit layout within each byte (SSZ convention), so byte 0
// bit 0 is overall bit 0.
//
// Wire formats (per format):
//   fracpack  bitvector: raw bytes (fixed size — no prefix)
//             bitlist:   [bit_count: bounded_length_t<N>][ceil(count/8) bytes]
//   bincode   bitvector: raw bytes
//             bitlist:   u64 bit_count + packed bytes
//   borsh     same as bincode but u32 count
//   SSZ       bitvector: raw bytes
//             bitlist:   delimiter-bit encoding (no prefix, span from parent)
//
// For non-SSZ formats the bit count is stored explicitly so the decoder
// knows exactly how many bits are valid; trailing bits within the last
// byte are ignored on decode and zero on encode.

#include <psio/bounded.hpp>
#include <psio/check.hpp>
#include <psio/stream.hpp>

#include <array>
#include <bit>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <span>
#include <type_traits>
#include <vector>

namespace psio
{
   // ── bitvector<N> ──────────────────────────────────────────────────────────

   template <std::size_t N>
   class bitvector
   {
     public:
      static constexpr std::size_t size_v     = N;
      static constexpr std::size_t byte_count = (N + 7) / 8;

      constexpr bitvector() noexcept = default;

      bitvector(std::initializer_list<bool> init)
      {
         check(init.size() <= N, "bitvector init too large");
         std::size_t i = 0;
         for (bool b : init)
            set(i++, b);
      }

      // ── Element access ──
      constexpr bool test(std::size_t i) const noexcept
      {
         return (bytes_[i >> 3] >> (i & 7u)) & 1u;
      }
      constexpr bitvector& set(std::size_t i, bool v = true) noexcept
      {
         if (v)
            bytes_[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7u));
         else
            bytes_[i >> 3] &= static_cast<std::uint8_t>(~(1u << (i & 7u)));
         return *this;
      }
      constexpr bitvector& reset(std::size_t i) noexcept { return set(i, false); }
      constexpr bitvector& flip(std::size_t i) noexcept
      {
         bytes_[i >> 3] ^= static_cast<std::uint8_t>(1u << (i & 7u));
         return *this;
      }

      // ── Whole-vector predicates ──
      constexpr std::size_t size() const noexcept { return N; }

      constexpr std::size_t count() const noexcept
      {
         std::size_t c = 0;
         for (auto b : bytes_)
            c += static_cast<std::size_t>(std::popcount(b));
         return c;
      }

      constexpr bool any() const noexcept
      {
         for (auto b : bytes_)
            if (b != 0)
               return true;
         return false;
      }
      constexpr bool none() const noexcept { return !any(); }

      constexpr bool all() const noexcept
      {
         constexpr std::size_t full_bytes = N / 8;
         constexpr std::size_t tail_bits  = N % 8;
         for (std::size_t i = 0; i < full_bytes; ++i)
            if (bytes_[i] != 0xffu)
               return false;
         if constexpr (tail_bits > 0)
         {
            constexpr std::uint8_t mask = static_cast<std::uint8_t>((1u << tail_bits) - 1u);
            if ((bytes_[full_bytes] & mask) != mask)
               return false;
         }
         return true;
      }

      // ── Bitwise ops ──
      constexpr bitvector& operator&=(const bitvector& o) noexcept
      {
         for (std::size_t i = 0; i < byte_count; ++i)
            bytes_[i] &= o.bytes_[i];
         return *this;
      }
      constexpr bitvector& operator|=(const bitvector& o) noexcept
      {
         for (std::size_t i = 0; i < byte_count; ++i)
            bytes_[i] |= o.bytes_[i];
         return *this;
      }
      constexpr bitvector& operator^=(const bitvector& o) noexcept
      {
         for (std::size_t i = 0; i < byte_count; ++i)
            bytes_[i] ^= o.bytes_[i];
         return *this;
      }
      constexpr bitvector operator~() const noexcept
      {
         bitvector r;
         for (std::size_t i = 0; i < byte_count; ++i)
            r.bytes_[i] = static_cast<std::uint8_t>(~bytes_[i]);
         // Clear trailing bits beyond N
         if constexpr ((N % 8) != 0)
         {
            constexpr std::uint8_t mask = static_cast<std::uint8_t>((1u << (N % 8)) - 1u);
            r.bytes_[byte_count - 1] &= mask;
         }
         return r;
      }

      // ── Raw byte access (for wire serialization) ──
      std::span<const std::uint8_t, byte_count> bytes() const noexcept { return bytes_; }
      std::span<std::uint8_t, byte_count>       bytes() noexcept { return bytes_; }

      const std::uint8_t* data() const noexcept { return bytes_.data(); }
      std::uint8_t*       data() noexcept { return bytes_.data(); }

      bool operator==(const bitvector&) const = default;

     private:
      std::array<std::uint8_t, byte_count> bytes_{};
   };

   static_assert(std::is_trivially_copyable_v<bitvector<1>>);
   static_assert(std::is_standard_layout_v<bitvector<1>>);
   static_assert(sizeof(bitvector<8>)  == 1);
   static_assert(sizeof(bitvector<16>) == 2);
   static_assert(sizeof(bitvector<17>) == 3);
   static_assert(sizeof(bitvector<512>) == 64);

   // bitvector is a POD of bytes — opt into the memcpy/run-batch path.
   template <std::size_t N>
   struct is_bitwise_copy<bitvector<N>> : std::true_type
   {
   };

   // ── bitlist<N> ────────────────────────────────────────────────────────────

   template <std::size_t MaxN>
   class bitlist
   {
     public:
      static constexpr std::size_t max_size_v = MaxN;

      constexpr bitlist() = default;

      bitlist(std::initializer_list<bool> init)
      {
         check(init.size() <= MaxN, "bitlist init too large");
         reserve_bits(init.size());
         for (bool b : init)
            push_back(b);
      }

      // ── Element access ──
      bool test(std::size_t i) const noexcept
      {
         return (bytes_[i >> 3] >> (i & 7u)) & 1u;
      }
      void set(std::size_t i, bool v = true) noexcept
      {
         if (v)
            bytes_[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7u));
         else
            bytes_[i >> 3] &= static_cast<std::uint8_t>(~(1u << (i & 7u)));
      }

      void push_back(bool v)
      {
         check(bit_count_ + 1 <= MaxN, "bitlist overflow");
         std::size_t new_byte = (bit_count_ >> 3);
         if (new_byte >= bytes_.size())
            bytes_.push_back(0);
         if (v)
            bytes_[new_byte] |= static_cast<std::uint8_t>(1u << (bit_count_ & 7u));
         ++bit_count_;
      }

      void clear() noexcept
      {
         bytes_.clear();
         bit_count_ = 0;
      }

      void resize(std::size_t new_bit_count)
      {
         check(new_bit_count <= MaxN, "bitlist overflow");
         std::size_t new_byte_count = (new_bit_count + 7) / 8;
         bytes_.resize(new_byte_count, 0);
         // Clear any now-trailing bits in the final byte.
         if (new_bit_count < bit_count_ && (new_bit_count % 8) != 0)
         {
            std::uint8_t mask =
                static_cast<std::uint8_t>((1u << (new_bit_count % 8)) - 1u);
            bytes_.back() &= mask;
         }
         bit_count_ = new_bit_count;
      }

      // ── Size queries ──
      std::size_t                 size() const noexcept { return bit_count_; }
      bool                        empty() const noexcept { return bit_count_ == 0; }
      static constexpr std::size_t max_size() noexcept { return MaxN; }
      std::size_t byte_count() const noexcept { return (bit_count_ + 7) / 8; }

      std::size_t count() const noexcept
      {
         std::size_t c = 0;
         for (auto b : bytes_)
            c += static_cast<std::size_t>(std::popcount(b));
         return c;
      }

      // ── Raw access (wire serialization) ──
      std::span<const std::uint8_t> bytes() const noexcept
      {
         return {bytes_.data(), bytes_.size()};
      }
      std::span<std::uint8_t> bytes() noexcept
      {
         return {bytes_.data(), bytes_.size()};
      }

      bool operator==(const bitlist&) const = default;

      // Low-level setter: install raw bit count + byte contents (used by
      // deserialization paths). Caller guarantees bit_count <= MaxN and
      // bytes has correct size.
      void assign_raw(std::size_t bit_count, const std::uint8_t* src)
      {
         check(bit_count <= MaxN, "bitlist overflow on assign");
         bit_count_ = bit_count;
         std::size_t nb = (bit_count + 7) / 8;
         bytes_.assign(src, src + nb);
      }

     private:
      void reserve_bits(std::size_t n)
      {
         bytes_.reserve((n + 7) / 8);
      }

      std::vector<std::uint8_t> bytes_;
      std::size_t               bit_count_{0};
   };

   // ── Type traits for dispatch ──────────────────────────────────────────────

   template <typename T>
   struct is_bitvector : std::false_type
   {
   };
   template <std::size_t N>
   struct is_bitvector<bitvector<N>> : std::true_type
   {
   };
   template <typename T>
   inline constexpr bool is_bitvector_v = is_bitvector<T>::value;

   template <typename T>
   struct is_bitlist : std::false_type
   {
   };
   template <std::size_t N>
   struct is_bitlist<bitlist<N>> : std::true_type
   {
   };
   template <typename T>
   inline constexpr bool is_bitlist_v = is_bitlist<T>::value;

   template <typename T>
   struct is_std_bitset : std::false_type
   {
   };
   template <std::size_t N>
   struct is_std_bitset<std::bitset<N>> : std::true_type
   {
   };
   template <typename T>
   inline constexpr bool is_std_bitset_v = is_std_bitset<T>::value;

   // ── Compile-time std::bitset layout probe ─────────────────────────────────
   //
   // std::bitset's internal layout is implementation-defined. However if we
   // can constexpr-cast a bitset to a byte array (C++20 std::bit_cast) and
   // observe that bit i lives at byte (i/8), bit (i%8) LSB-first, we can
   // take a direct memcpy path instead of the bit-by-bit one.
   //
   // libstdc++ and libc++ both use an array of unsigned long (or unsigned
   // long long) LSB-first, which matches our canonical form on little-endian
   // targets. This detection handles both the match and non-match case
   // uniformly: constexpr-check once, use the result at every call site.

   namespace bitset_detail
   {
      template <std::size_t N>
      consteval bool layout_canonical_probe() noexcept
      {
         if constexpr (N == 0)
            return true;
         if constexpr (!std::is_trivially_copyable_v<std::bitset<N>>)
            return false;

         // Exercise bits 0, 7, 8 (if present) — spans both within-byte and
         // cross-byte boundaries. If any fails, layout is not canonical.
         constexpr std::size_t raw_bytes = sizeof(std::bitset<N>);
         constexpr std::size_t tests     = (N > 9) ? 9 : N;

         for (std::size_t i = 0; i < tests; ++i)
         {
            std::bitset<N> bs;
            bs.set(i);
            auto bytes = std::bit_cast<std::array<std::uint8_t, raw_bytes>>(bs);

            std::size_t  expect_byte = i >> 3;
            std::uint8_t expect_val  = static_cast<std::uint8_t>(1u << (i & 7u));
            for (std::size_t j = 0; j < raw_bytes; ++j)
            {
               std::uint8_t expected = (j == expect_byte) ? expect_val : std::uint8_t{0};
               if (bytes[j] != expected)
                  return false;
            }
         }
         return true;
      }
   }  // namespace bitset_detail

   template <std::size_t N>
   inline constexpr bool bitset_layout_is_canonical_v =
       bitset_detail::layout_canonical_probe<N>();

   // ── Encode/decode helpers for std::bitset<N> ──────────────────────────────
   //
   // Canonical layout → direct memcpy of first ceil(N/8) bytes.
   // Otherwise → fall back to bit-by-bit packing via to_ullong (N ≤ 64) or a
   // per-bit loop (larger sizes).

   template <std::size_t N>
   inline void pack_bitset_bytes(const std::bitset<N>& bs, std::uint8_t* out) noexcept
   {
      constexpr std::size_t NB = (N + 7) / 8;
      if constexpr (N == 0)
      {
         return;
      }
      else if constexpr (bitset_layout_is_canonical_v<N>)
      {
         // Memcpy path: the bitset's internal bytes are already LSB-first
         // canonical; just take the valid prefix.
         std::memcpy(out, &bs, NB);
      }
      else if constexpr (N <= 64)
      {
         std::uint64_t v = bs.to_ullong();
         std::memcpy(out, &v, NB);
      }
      else
      {
         std::memset(out, 0, NB);
         for (std::size_t i = 0; i < N; ++i)
            if (bs[i])
               out[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7u));
      }
   }

   template <std::size_t N>
   inline void unpack_bitset_bytes(const std::uint8_t* in, std::bitset<N>& bs) noexcept
   {
      if constexpr (N == 0)
      {
         return;
      }
      else if constexpr (bitset_layout_is_canonical_v<N>)
      {
         // Default-construct (zero-initializes padding), then memcpy into
         // the canonical-layout prefix.
         bs = std::bitset<N>{};
         std::memcpy(&bs, in, (N + 7) / 8);
      }
      else if constexpr (N <= 64)
      {
         std::uint64_t v = 0;
         std::memcpy(&v, in, (N + 7) / 8);
         bs = std::bitset<N>(v);
      }
      else
      {
         bs.reset();
         for (std::size_t i = 0; i < N; ++i)
            if (in[i >> 3] & (1u << (i & 7u)))
               bs.set(i);
      }
   }

   // Conversions between std::bitset<N> and psio::bitvector<N>
   //
   // Byte layout is identical (LSB-first in byte order). For N ≤ 64 the
   // conversion is a single memcpy through a uint64; for larger N it walks
   // bit-by-bit. Both directions are O(N / 64) memory operations in the
   // fast case, O(N) otherwise.

   template <std::size_t N>
   inline bitvector<N> to_bitvector(const std::bitset<N>& bs) noexcept
   {
      bitvector<N> out;
      pack_bitset_bytes(bs, out.data());
      return out;
   }

   template <std::size_t N>
   inline std::bitset<N> to_bitset(const bitvector<N>& bv) noexcept
   {
      std::bitset<N> out;
      unpack_bitset_bytes(bv.data(), out);
      return out;
   }

   // ── std::vector<bool> helpers ─────────────────────────────────────────────
   //
   // std::vector<bool> is a dynamic bit-packed array. We treat it as an
   // unbounded bitlist analogue. Because element access goes through proxy
   // references, we iterate bit-by-bit to pack/unpack LSB-first bytes.

   inline std::vector<std::uint8_t> pack_vector_bool(const std::vector<bool>& v)
   {
      std::vector<std::uint8_t> out((v.size() + 7) / 8, 0);
      for (std::size_t i = 0; i < v.size(); ++i)
         if (v[i])
            out[i >> 3] |= static_cast<std::uint8_t>(1u << (i & 7u));
      return out;
   }

   inline void unpack_vector_bool(const std::uint8_t* bytes,
                                   std::size_t         bit_count,
                                   std::vector<bool>&  v)
   {
      v.assign(bit_count, false);
      for (std::size_t i = 0; i < bit_count; ++i)
         if (bytes[i >> 3] & (1u << (i & 7u)))
            v[i] = true;
   }

}  // namespace psio
