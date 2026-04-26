#include <pfs/cas.hpp>
#include <pfs/keys.hpp>

#include <psio1/fracpack.hpp>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace pfs
{

cas::cas(std::shared_ptr<psitri::database> db, block_store& bs, const config& cfg)
    : _db(std::move(db)), _bs(bs), _cfg(cfg)
{
}

cid cas::put(psio1::bytes_view data)
{
   auto content_cid = compute_cid(data);
   auto key         = cas_key(content_cid);
   auto root_idx    = root_for_cid(content_cid);

   auto ws = _db->start_write_session();
   auto tx = ws->start_transaction(root_idx);

   // Check if already exists → dedup
   auto existing = tx.get<std::string>(key);
   if (existing)
   {
      auto entry = psio1::from_frac<cas_entry>(
          std::span<const char>(existing->data(), existing->size()));
      entry.refcount++;
      auto val = psio1::to_frac(entry);
      tx.upsert(key, std::string_view(val.data(), val.size()));
      tx.commit();
      return content_cid;
   }

   // New entry
   cas_entry entry;
   entry.refcount   = 1;
   entry.total_size = data.size();

   if (data.size() <= chunk_size)
   {
      // Small file: inline
      entry.inline_data.assign(data.begin(), data.end());
   }
   else
   {
      // Large file: chunk into blocks
      for (size_t off = 0; off < data.size(); off += chunk_size)
      {
         size_t len   = std::min(static_cast<size_t>(chunk_size), data.size() - off);
         auto   chunk = data.subspan(off, len);

         // Allocate in same transaction for same-shard, or separate tx if different shard
         // For v1 simplicity, alloc blocks in the same transaction
         uint64_t block_id = _bs.alloc(tx, chunk);

         chunk_ref ref;
         ref.block_id = block_id;
         ref.offset   = 0;
         ref.size     = static_cast<uint32_t>(len);
         entry.chunks.push_back(ref);
      }
   }

   auto val = psio1::to_frac(entry);
   tx.upsert(key, std::string_view(val.data(), val.size()));
   tx.commit();
   return content_cid;
}

void cas::pin(const cid& c)
{
   auto key      = cas_key(c);
   auto root_idx = root_for_cid(c);

   auto ws = _db->start_write_session();
   auto tx = ws->start_transaction(root_idx);

   auto existing = tx.get<std::string>(key);
   if (!existing)
      throw std::runtime_error("pfs::cas::pin: CID not found");

   auto entry = psio1::from_frac<cas_entry>(
       std::span<const char>(existing->data(), existing->size()));
   entry.refcount++;
   auto val = psio1::to_frac(entry);
   tx.upsert(key, std::string_view(val.data(), val.size()));
   tx.commit();
}

void cas::unpin(const cid& c)
{
   auto key      = cas_key(c);
   auto root_idx = root_for_cid(c);

   auto ws = _db->start_write_session();
   auto tx = ws->start_transaction(root_idx);

   auto existing = tx.get<std::string>(key);
   if (!existing)
      return;  // already gone

   auto entry = psio1::from_frac<cas_entry>(
       std::span<const char>(existing->data(), existing->size()));

   if (entry.refcount <= 1)
   {
      // Free all blocks
      for (auto& chunk : entry.chunks)
         _bs.free(tx, chunk.block_id);
      tx.remove(key);
   }
   else
   {
      entry.refcount--;
      auto val = psio1::to_frac(entry);
      tx.upsert(key, std::string_view(val.data(), val.size()));
   }
   tx.commit();
}

cas_entry cas::read_entry(const cid& c)
{
   auto key      = cas_key(c);
   auto root_idx = root_for_cid(c);

   auto rs     = _db->start_read_session();
   auto cursor = rs->create_cursor(root_idx);
   auto result = cursor.get<std::string>(key);
   if (!result)
      throw std::runtime_error("pfs::cas: CID not found");

   return psio1::from_frac<cas_entry>(
       std::span<const char>(result->data(), result->size()));
}

std::vector<uint8_t> cas::get(const cid& c)
{
   auto entry = read_entry(c);

   if (!entry.inline_data.empty())
      return entry.inline_data;

   // Reassemble from chunks
   std::vector<uint8_t> data;
   data.reserve(entry.total_size);

   for (auto& chunk : entry.chunks)
   {
      std::vector<uint8_t> block_data;
      _bs.read(chunk.block_id, block_data);

      auto start = block_data.data() + chunk.offset;
      data.insert(data.end(), start, start + chunk.size);
   }

   return data;
}

std::vector<uint8_t> cas::get_range(const cid& c, uint64_t offset, uint64_t length)
{
   auto entry = read_entry(c);

   if (offset + length > entry.total_size)
      throw std::runtime_error("pfs::cas::get_range: out of bounds");

   if (!entry.inline_data.empty())
   {
      return std::vector<uint8_t>(entry.inline_data.begin() + offset,
                                 entry.inline_data.begin() + offset + length);
   }

   std::vector<uint8_t> result;
   result.reserve(length);

   uint64_t file_pos = 0;
   for (auto& chunk : entry.chunks)
   {
      uint64_t chunk_end = file_pos + chunk.size;
      if (file_pos >= offset + length)
         break;
      if (chunk_end <= offset)
      {
         file_pos = chunk_end;
         continue;
      }

      uint64_t read_start = (offset > file_pos) ? offset - file_pos : 0;
      uint64_t read_end   = std::min(static_cast<uint64_t>(chunk.size), offset + length - file_pos);

      std::vector<uint8_t> block_data;
      _bs.read_range(chunk.block_id, chunk.offset + read_start, read_end - read_start, block_data);
      result.insert(result.end(), block_data.begin(), block_data.end());

      file_pos = chunk_end;
   }

   return result;
}

void cas::read(const cid&                                            c,
               uint64_t                                              offset,
               uint64_t                                              length,
               std::function<void(psio1::bytes_view)> const&  cb)
{
   auto entry = read_entry(c);

   if (offset + length > entry.total_size)
      throw std::runtime_error("pfs::cas::read: out of bounds");

   if (!entry.inline_data.empty())
   {
      auto span = psio1::bytes_view(entry.inline_data).subspan(offset, length);
      cb(span);
      return;
   }

   uint64_t file_pos = 0;
   for (auto& chunk : entry.chunks)
   {
      uint64_t chunk_end = file_pos + chunk.size;
      if (file_pos >= offset + length)
         break;
      if (chunk_end <= offset)
      {
         file_pos = chunk_end;
         continue;
      }

      uint64_t read_start = (offset > file_pos) ? offset - file_pos : 0;
      uint64_t read_end   = std::min(static_cast<uint64_t>(chunk.size), offset + length - file_pos);

      std::vector<uint8_t> block_data;
      _bs.read_range(chunk.block_id, chunk.offset + read_start, read_end - read_start, block_data);
      cb(block_data);

      file_pos = chunk_end;
   }
}

std::optional<cas_stat> cas::stat(const cid& c)
{
   auto key      = cas_key(c);
   auto root_idx = root_for_cid(c);

   auto rs     = _db->start_read_session();
   auto cursor = rs->create_cursor(root_idx);
   auto result = cursor.get<std::string>(key);
   if (!result)
      return std::nullopt;

   auto entry = psio1::from_frac<cas_entry>(
       std::span<const char>(result->data(), result->size()));

   return cas_stat{
       .total_size   = entry.total_size,
       .chunk_count  = static_cast<uint32_t>(entry.chunks.size()),
       .refcount     = entry.refcount,
   };
}

}  // namespace pfs
