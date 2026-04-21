#pragma once
#include <pfs/block_store.hpp>
#include <pfs/cid.hpp>
#include <pfs/config.hpp>
#include <pfs/schema.hpp>

#include <psio/bytes_view.hpp>
#include <psio/fracpack.hpp>
#include <psitri/database.hpp>

#include <memory>
#include <optional>

namespace pfs
{

   class cas
   {
     public:
      cas(std::shared_ptr<psitri::database> db, block_store& bs, const config& cfg = {});

      // Store data, returns the CID. Handles dedup via refcount.
      cid put(psio::bytes_view data);

      // Increment refcount for an existing CID.
      void pin(const cid& c);

      // Decrement refcount. If zero, frees blocks and deletes entry.
      void unpin(const cid& c);

      // Read full content of a CID.
      std::vector<uint8_t> get(const cid& c);

      // Read a byte range from a CID.
      std::vector<uint8_t> get_range(const cid& c, uint64_t offset, uint64_t length);

      // Zero-copy read: calls cb with each chunk's data in order.
      void read(const cid&                                   c,
                uint64_t                                     offset,
                uint64_t                                     length,
                std::function<void(psio::bytes_view)> const& cb);

      // Stat a CID.
      std::optional<cas_stat> stat(const cid& c);

      uint32_t root_for_cid(const cid& c) const
      {
         return _cfg.root_base + cas_shard(c, _cfg.shard_count);
      }

     private:
      cas_entry read_entry(const cid& c);

      std::shared_ptr<psitri::database> _db;
      block_store&                      _bs;
      config                            _cfg;
   };

}  // namespace pfs
