#pragma once
#include <pfs/cid.hpp>
#include <psio/reflect.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace pfs
{

struct chunk_ref
{
   uint64_t block_id;
   uint32_t offset;
   uint32_t size;
};
PSIO_REFLECT(chunk_ref, block_id, offset, size)

struct cas_entry
{
   uint32_t               refcount   = 0;
   uint64_t               total_size = 0;
   std::vector<uint8_t>   inline_data;
   std::vector<chunk_ref> chunks;
};
PSIO_REFLECT(cas_entry, refcount, total_size, inline_data, chunks)

enum class entry_type : uint8_t
{
   file      = 0,
   directory = 1,
};

struct fs_entry
{
   entry_type         type     = entry_type::file;
   uint16_t           mode     = 0644;
   uint32_t           owner    = 0;
   uint64_t           mtime_ns = 0;
   uint64_t           size     = 0;
   std::optional<cid> content_cid;
};
PSIO_REFLECT(fs_entry, type, mode, owner, mtime_ns, size, content_cid)

struct fs_quota
{
   uint64_t limit = 0;
   uint64_t used  = 0;
};
PSIO_REFLECT(fs_quota, limit, used)

struct dir_entry
{
   std::string name;
   fs_entry    entry;
};

struct cas_stat
{
   uint64_t total_size;
   uint32_t chunk_count;
   uint32_t refcount;
};

}  // namespace pfs
