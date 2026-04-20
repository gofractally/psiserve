#pragma once
#include <psio/bytes_view.hpp>
#include <psio/fracpack.hpp>
#include <psio/reflect.hpp>

#include <array>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>

namespace pfs
{

// CIDv1 / dag-pb / SHA-256 — fixed 36 bytes.
// Binary on disk, base32-lower multibase at HTTP boundary only.
struct cid
{
   static constexpr uint32_t digest_size = 32;
   static constexpr uint32_t header_size = 4;
   static constexpr uint32_t size        = header_size + digest_size;

   std::array<uint8_t, size> bytes = {
       0x01,  // CIDv1
       0x70,  // dag-pb codec
       0x12,  // sha2-256 multihash fn
       0x20,  // digest length = 32
   };

   static cid from_digest(std::span<const uint8_t, digest_size> digest);
   static cid from_string(std::string_view s);
   static cid from_bytes(std::span<const uint8_t, size> raw);

   std::string to_string() const;

   std::span<const uint8_t, digest_size> digest() const
   {
      return std::span<const uint8_t, digest_size>(bytes.data() + header_size, digest_size);
   }

   auto operator<=>(const cid&) const = default;
   bool operator==(const cid&) const  = default;

   friend std::ostream& operator<<(std::ostream& os, const cid& c) { return os << c.to_string(); }
};
static_assert(sizeof(cid) == 36);

// Compute CID from raw data using IPFS-compatible chunking + UnixFS DAG.
cid compute_cid(psio::bytes_view data);

}  // namespace pfs

namespace psio
{
template <>
struct is_packable_memcpy<pfs::cid> : std::bool_constant<true>
{
};
}  // namespace psio
