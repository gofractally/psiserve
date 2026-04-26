#pragma once
#include <pfs/block_store.hpp>
#include <pfs/cas.hpp>
#include <pfs/cid.hpp>
#include <pfs/config.hpp>
#include <pfs/schema.hpp>

#include <psio1/bytes_view.hpp>
#include <psio1/fracpack.hpp>
#include <psio1/name.hpp>

#include <psitri/read_session.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

namespace pfs
{

using path = std::filesystem::path;

class file_handle
{
  public:
   uint64_t        size() const { return _entry.size; }
   const cid&      content_cid() const { return *_entry.content_cid; }
   const fs_entry& stat() const { return _entry; }

   void read(uint64_t                                              offset,
             uint64_t                                              length,
             std::function<void(psio1::bytes_view)> const&  cb);

   void read(std::function<void(psio1::bytes_view)> const& cb);

  private:
   friend class store;
   file_handle(fs_entry entry, cas* cas_ptr) : _entry(std::move(entry)), _cas(cas_ptr) {}

   fs_entry _entry;
   cas*     _cas;
};

class dir_cursor
{
  public:
   bool valid() const;
   const dir_entry& entry() const;

   bool next();
   bool prev();

  private:
   friend class store;
   dir_cursor(psitri::read_session::ptr rs, uint32_t root, std::string prefix);

   bool skip_to_direct_child(bool forward);
   void decode_current();

   psitri::read_session::ptr _rs;
   psitri::cursor            _cursor;
   std::string               _prefix;
   dir_entry                 _current;
   bool                      _valid = false;
};

class store
{
  public:
   store(std::shared_ptr<psitri::database> db, config cfg = {});

   // ── Content-addressed storage (no filesystem entry) ──────────────
   cid         put(psio1::bytes_view data);
   void        unpin(const cid& c);
   file_handle open(const cid& c);

   // ── File handles (read) ─────────────────────────────────────────
   file_handle open(psio1::name_id tenant, const path& p);

   // ── Mutations ───────────────────────────────────────────────────
   cid  write(psio1::name_id            tenant,
              const path&              p,
              psio1::bytes_view data,
              uint16_t                 mode  = 0644,
              uint32_t                 owner = 0);
   void remove(psio1::name_id tenant, const path& p);
   void mkdir(psio1::name_id tenant,
              const path&   p,
              uint16_t      mode  = 0755,
              uint32_t      owner = 0);
   void chmod(psio1::name_id tenant, const path& p, uint16_t mode);

   // ── Directory listing ───────────────────────────────────────────
   dir_cursor ls(psio1::name_id tenant, const path& p);
   dir_cursor ls(psio1::name_id tenant, const path& p, std::string_view after);

   // ── Metadata ────────────────────────────────────────────────────
   std::optional<fs_entry> stat(psio1::name_id tenant, const path& p);

   // ── Quota ───────────────────────────────────────────────────────
   void     set_quota(psio1::name_id tenant, uint64_t limit);
   fs_quota quota(psio1::name_id tenant);

   // ── Sharing ─────────────────────────────────────────────────────
   void share(psio1::name_id src,
              const path&   src_path,
              psio1::name_id dst,
              const path&   dst_path);

  private:
   std::string normalize_path(const path& p);
   uint32_t    root_for_tenant(psio1::name_id tenant) const;
   void        update_quota(psitri::transaction& tx,
                            psio1::name_id        tenant,
                            int64_t              size_delta);
   uint64_t    now_ns() const;

   std::shared_ptr<psitri::database> _db;
   config                            _cfg;
   block_store                       _bs;
   cas                               _cas;
};

}  // namespace pfs
