#pragma once
#include <cstdint>

namespace pfs
{

struct config
{
   uint32_t root_base   = 0;
   uint32_t shard_count = 256;
};

static constexpr uint32_t chunk_size     = 256 * 1024;  // 256 KB IPFS default
static constexpr uint32_t max_block_size = 4 * 1024 * 1024;  // 4 MB

}  // namespace pfs
