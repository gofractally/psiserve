#pragma once
#include <pfs/config.hpp>
#include <pfs/keys.hpp>

#include <psio/bytes_view.hpp>

#include <psitri/database.hpp>
#include <psitri/transaction.hpp>
#include <psitri/write_session.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace pfs
{

class block_store
{
  public:
   block_store(std::shared_ptr<psitri::database> db, const config& cfg = {});

   // Allocate a block and write data. Returns the block_id.
   // Must be called within a transaction context (caller manages tx).
   uint64_t alloc(psitri::transaction& tx, psio::bytes_view data);

   // Free a block. Caller manages the transaction.
   void free(psitri::transaction& tx, uint64_t block_id);

   // Read a block into a buffer. Uses a read session internally.
   void read(uint64_t block_id, std::vector<uint8_t>& out);

   // Read a range within a block.
   void read_range(uint64_t               block_id,
                   uint64_t               offset,
                   uint64_t               length,
                   std::vector<uint8_t>&  out);

   uint32_t root_for_block(uint64_t block_id) const
   {
      return _cfg.root_base + block_shard(block_id, _cfg.shard_count);
   }

  private:
   std::shared_ptr<psitri::database> _db;
   config                            _cfg;
   std::atomic<uint64_t>             _next_seq{1};
};

}  // namespace pfs
