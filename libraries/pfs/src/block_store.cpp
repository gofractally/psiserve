#include <pfs/block_store.hpp>

#include <cstring>
#include <stdexcept>

namespace pfs
{

block_store::block_store(std::shared_ptr<psitri::database> db, const config& cfg)
    : _db(std::move(db)), _cfg(cfg)
{
}

uint64_t block_store::alloc(psitri::transaction& tx, psio::bytes_view data)
{
   if (data.size() > max_block_size)
      throw std::runtime_error("pfs::block_store: data exceeds max block size");

   uint64_t seq = _next_seq.fetch_add(1, std::memory_order_relaxed);
   // Encode shard into top bits so block_shard() can extract it
   uint32_t shard    = static_cast<uint32_t>(seq % _cfg.shard_count);
   uint64_t block_id = (static_cast<uint64_t>(shard) << 56) | (seq & 0x00FFFFFFFFFFFFFFULL);

   auto key = block_key(block_id);
   auto val = std::string_view(reinterpret_cast<const char*>(data.data()), data.size());
   tx.upsert(key, val);

   return block_id;
}

void block_store::free(psitri::transaction& tx, uint64_t block_id)
{
   auto key = block_key(block_id);
   tx.remove(key);
}

void block_store::read(uint64_t block_id, std::vector<uint8_t>& out)
{
   auto rs     = _db->start_read_session();
   auto root   = _cfg.root_base + block_shard(block_id, _cfg.shard_count);
   auto cursor = rs->create_cursor(root);

   auto key    = block_key(block_id);
   auto result = cursor.get<std::string>(key);
   if (!result)
      throw std::runtime_error("pfs::block_store: block not found");

   out.resize(result->size());
   std::memcpy(out.data(), result->data(), result->size());
}

void block_store::read_range(uint64_t              block_id,
                             uint64_t              offset,
                             uint64_t              length,
                             std::vector<uint8_t>& out)
{
   auto rs     = _db->start_read_session();
   auto root   = _cfg.root_base + block_shard(block_id, _cfg.shard_count);
   auto cursor = rs->create_cursor(root);

   auto key    = block_key(block_id);
   auto result = cursor.get<std::string>(key);
   if (!result)
      throw std::runtime_error("pfs::block_store: block not found");

   if (offset + length > result->size())
      throw std::runtime_error("pfs::block_store: range out of bounds");

   out.resize(length);
   std::memcpy(out.data(), result->data() + offset, length);
}

}  // namespace pfs
