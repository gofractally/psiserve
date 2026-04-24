#pragma once
// Extended integer types:
//   psio::uint128  — alias for unsigned __int128 (GCC/Clang extension)
//   psio::int128   — alias for          __int128 (GCC/Clang extension)
//   psio::uint256  — 32-byte LE unsigned integer, 4 × uint64 limbs (limb[0] = least-significant)
//
// Purpose: first-class support for SSZ/Ethereum-consensus workloads where
// 128/256-bit integers appear (balances, storage, KZG). Types are designed
// purely for wire serialization — arithmetic is intentionally minimal;
// users needing full bignum math should convert to their preferred library
// (intx, boost::multiprecision) via the limb array.

#include <psio/stream.hpp>

#include <array>
#include <compare>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace psio
{
   // ── Native compiler-extension aliases (sizeof == 16, 16-byte aligned) ────
   using uint128 = unsigned __int128;
   using  int128 =          __int128;

   // ── uint256: 32-byte LE, four u64 limbs ──────────────────────────────────
   struct uint256
   {
      std::uint64_t limb[4]{};  // limb[0] = least-significant 64 bits

      constexpr uint256() noexcept = default;
      constexpr explicit uint256(std::uint64_t v) noexcept : limb{v, 0, 0, 0} {}
      constexpr explicit uint256(uint128 v) noexcept
          : limb{static_cast<std::uint64_t>(v),
                 static_cast<std::uint64_t>(v >> 64), 0, 0}
      {
      }

      bool operator==(const uint256&) const noexcept = default;
      auto operator<=>(const uint256&) const noexcept = default;
   };

   static_assert(sizeof(uint256) == 32, "uint256 must be exactly 32 bytes");
   static_assert(alignof(uint256) == alignof(std::uint64_t));
   static_assert(std::is_standard_layout_v<uint256>);
   static_assert(std::is_trivially_copyable_v<uint256>);

   // ── Register as bitwise-serializable ─────────────────────────────────────
   // Allows run-batching, memcpy-path vector pack/unpack, and format detection
   // of these types as fixed-width primitives across all psio encoders.

   template <>
   struct is_bitwise_copy<uint128> : std::true_type
   {
   };

   template <>
   struct is_bitwise_copy<int128> : std::true_type
   {
   };

   template <>
   struct is_bitwise_copy<uint256> : std::true_type
   {
   };

}  // namespace psio
