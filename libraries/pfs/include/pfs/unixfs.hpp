#pragma once
#include <psio/bytes_view.hpp>

#include <cstdint>
#include <vector>

namespace pfs::unixfs
{

// Wrap raw data bytes as a UnixFS File data block (dag-pb encoded).
// Returns the serialized dag-pb block ready for CID hashing.
std::vector<uint8_t> encode_file_block(psio::bytes_view data);

// Build a UnixFS directory link entry for a dag-pb Links array.
// Used to construct intermediate DAG nodes for multi-chunk files.
struct dag_link
{
   std::vector<uint8_t> cid_bytes;  // raw CID (36 bytes)
   uint64_t             tsize;      // total size of linked subtree
};

// Wrap a set of links as a UnixFS File DAG root (dag-pb encoded).
// total_size = original file size, used for the Data field.
std::vector<uint8_t> encode_file_root(std::span<const dag_link> links,
                                      uint64_t                  total_size,
                                      std::span<const uint64_t> block_sizes);

}  // namespace pfs::unixfs
