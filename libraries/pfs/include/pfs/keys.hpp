#pragma once
#include <pfs/cid.hpp>
#include <pfs/config.hpp>
#include <psio/name.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace pfs
{

inline void encode_be64(std::string& k, uint64_t v)
{
   for (int i = 0; i < 8; ++i)
      k.push_back(static_cast<char>((v >> (56 - 8 * i)) & 0xFF));
}

inline std::string block_key(uint64_t block_id)
{
   std::string k;
   k.reserve(9);
   k += 'B';
   encode_be64(k, block_id);
   return k;
}

inline std::string cas_key(const cid& c)
{
   std::string k(1 + cid::size, '\0');
   k[0] = 'C';
   std::memcpy(k.data() + 1, c.bytes.data(), cid::size);
   return k;
}

inline std::string fs_key(psio::name_id tenant, std::string_view path)
{
   std::string k;
   k.reserve(9 + path.size());
   k += 'F';
   encode_be64(k, tenant.value);
   k += path;
   return k;
}

inline std::string fs_tenant_prefix(psio::name_id tenant)
{
   std::string k;
   k.reserve(9);
   k += 'F';
   encode_be64(k, tenant.value);
   return k;
}

inline std::string fs_dir_prefix(psio::name_id tenant, std::string_view dir_path)
{
   std::string k;
   k.reserve(9 + dir_path.size());
   k += 'F';
   encode_be64(k, tenant.value);
   k += dir_path;
   return k;
}

// Shard selection: hash the discriminant to a shard index.
inline uint32_t shard_of(uint64_t discriminant, uint32_t shard_count)
{
   // Simple multiplicative hash
   discriminant ^= discriminant >> 33;
   discriminant *= 0xff51afd7ed558ccdULL;
   discriminant ^= discriminant >> 33;
   return static_cast<uint32_t>(discriminant & (shard_count - 1));
}

inline uint32_t block_shard(uint64_t block_id, uint32_t shard_count)
{
   return shard_of(block_id, shard_count);
}

inline uint32_t cas_shard(const cid& c, uint32_t shard_count)
{
   uint64_t v;
   std::memcpy(&v, c.bytes.data() + cid::header_size, sizeof(v));
   return shard_of(v, shard_count);
}

inline uint32_t fs_shard(psio::name_id tenant, uint32_t shard_count)
{
   return shard_of(tenant.value, shard_count);
}

}  // namespace pfs
