#include <pfs/store.hpp>
#include <pfs/keys.hpp>

#include <psio1/fracpack.hpp>

#include <chrono>
#include <cstring>
#include <stdexcept>

namespace pfs
{

// ── file_handle ────────────────────────────────────────────────────

void file_handle::read(uint64_t                                              offset,
                       uint64_t                                              length,
                       std::function<void(psio1::bytes_view)> const&  cb)
{
   if (!_entry.content_cid)
      throw std::runtime_error("pfs::file_handle: no content CID (directory?)");
   _cas->read(*_entry.content_cid, offset, length, cb);
}

void file_handle::read(std::function<void(psio1::bytes_view)> const& cb)
{
   read(0, _entry.size, cb);
}

// ── store ──────────────────────────────────────────────────────────

store::store(std::shared_ptr<psitri::database> db, config cfg)
    : _db(db), _cfg(cfg), _bs(db, cfg), _cas(db, _bs, cfg)
{
}

std::string store::normalize_path(const path& p)
{
   auto s = p.generic_string();
   // Strip leading slash if present
   if (!s.empty() && s[0] == '/')
      s = s.substr(1);
   return s;
}

uint32_t store::root_for_tenant(psio1::name_id tenant) const
{
   return _cfg.root_base + fs_shard(tenant, _cfg.shard_count);
}

uint64_t store::now_ns() const
{
   using namespace std::chrono;
   return static_cast<uint64_t>(
       duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count());
}

void store::update_quota(psitri::transaction& tx,
                         psio1::name_id        tenant,
                         int64_t              size_delta)
{
   auto meta_key = fs_key(tenant, "$meta");
   auto existing = tx.get<std::string>(meta_key);

   fs_quota q;
   if (existing)
      q = psio1::from_frac<fs_quota>(
          std::span<const char>(existing->data(), existing->size()));

   q.used = static_cast<uint64_t>(static_cast<int64_t>(q.used) + size_delta);

   if (q.limit > 0 && q.used > q.limit)
      throw std::runtime_error("pfs::store: quota exceeded");

   auto val = psio1::to_frac(q);
   tx.upsert(meta_key, std::string_view(val.data(), val.size()));
}

// ── Content-addressed storage ──────────────────────────────────────

cid store::put(psio1::bytes_view data)
{
   return _cas.put(data);
}

void store::unpin(const cid& c)
{
   _cas.unpin(c);
}

// ── Mutations ──────────────────────────────────────────────────────

cid store::write(psio1::name_id            tenant,
                 const path&              p,
                 psio1::bytes_view data,
                 uint16_t                 mode,
                 uint32_t                 owner)
{
   auto path_str = normalize_path(p);

   // Put data into CAS (handles dedup + chunking)
   auto content_cid = _cas.put(data);

   // Create/update filesystem entry
   auto root_idx = root_for_tenant(tenant);
   auto ws       = _db->start_write_session();
   auto tx       = ws->start_transaction(root_idx);

   auto key = fs_key(tenant, path_str);

   // Check for existing entry to get old size for quota delta
   int64_t old_size = 0;
   cid     old_cid;
   bool    had_old = false;

   auto existing = tx.get<std::string>(key);
   if (existing)
   {
      auto old_entry = psio1::from_frac<fs_entry>(
          std::span<const char>(existing->data(), existing->size()));
      old_size = static_cast<int64_t>(old_entry.size);
      if (old_entry.content_cid)
      {
         old_cid = *old_entry.content_cid;
         had_old = true;
      }
   }

   fs_entry entry;
   entry.type        = entry_type::file;
   entry.mode        = mode;
   entry.owner       = owner;
   entry.mtime_ns    = now_ns();
   entry.size        = data.size();
   entry.content_cid = content_cid;

   auto val = psio1::to_frac(entry);
   tx.upsert(key, std::string_view(val.data(), val.size()));

   // Update quota
   update_quota(tx, tenant, static_cast<int64_t>(data.size()) - old_size);

   tx.commit();

   // Unpin old CID (outside tx — may be in different shard)
   if (had_old && old_cid != content_cid)
      _cas.unpin(old_cid);

   return content_cid;
}

void store::remove(psio1::name_id tenant, const path& p)
{
   auto path_str = normalize_path(p);
   auto root_idx = root_for_tenant(tenant);
   auto ws       = _db->start_write_session();
   auto tx       = ws->start_transaction(root_idx);

   auto key      = fs_key(tenant, path_str);
   auto existing = tx.get<std::string>(key);
   if (!existing)
   {
      tx.abort();
      return;
   }

   auto entry = psio1::from_frac<fs_entry>(
       std::span<const char>(existing->data(), existing->size()));

   tx.remove(key);

   if (entry.type == entry_type::file)
      update_quota(tx, tenant, -static_cast<int64_t>(entry.size));

   tx.commit();

   // Unpin CID after tx
   if (entry.content_cid)
      _cas.unpin(*entry.content_cid);
}

void store::mkdir(psio1::name_id tenant,
                  const path&   p,
                  uint16_t      mode,
                  uint32_t      owner)
{
   auto path_str = normalize_path(p);
   // Directories are stored with trailing '/'
   if (path_str.empty() || path_str.back() != '/')
      path_str += '/';

   auto root_idx = root_for_tenant(tenant);
   auto ws       = _db->start_write_session();
   auto tx       = ws->start_transaction(root_idx);

   auto key = fs_key(tenant, path_str);

   fs_entry entry;
   entry.type     = entry_type::directory;
   entry.mode     = mode;
   entry.owner    = owner;
   entry.mtime_ns = now_ns();

   auto val = psio1::to_frac(entry);
   tx.upsert(key, std::string_view(val.data(), val.size()));
   tx.commit();
}

void store::chmod(psio1::name_id tenant, const path& p, uint16_t mode)
{
   auto path_str = normalize_path(p);
   auto root_idx = root_for_tenant(tenant);
   auto ws       = _db->start_write_session();
   auto tx       = ws->start_transaction(root_idx);

   auto key      = fs_key(tenant, path_str);
   auto existing = tx.get<std::string>(key);
   if (!existing)
      throw std::runtime_error("pfs::store::chmod: path not found");

   auto entry = psio1::from_frac<fs_entry>(
       std::span<const char>(existing->data(), existing->size()));
   entry.mode = mode;

   auto val = psio1::to_frac(entry);
   tx.upsert(key, std::string_view(val.data(), val.size()));
   tx.commit();
}

// ── Reads ──────────────────────────────────────────────────────────

file_handle store::open(psio1::name_id tenant, const path& p)
{
   auto path_str = normalize_path(p);
   auto root_idx = root_for_tenant(tenant);

   auto rs     = _db->start_read_session();
   auto cursor = rs->create_cursor(root_idx);
   auto key    = fs_key(tenant, path_str);
   auto result = cursor.get<std::string>(key);
   if (!result)
      throw std::runtime_error("pfs::store::open: file not found");

   auto entry = psio1::from_frac<fs_entry>(
       std::span<const char>(result->data(), result->size()));
   return file_handle(std::move(entry), &_cas);
}

file_handle store::open(const cid& c)
{
   auto s = _cas.stat(c);
   if (!s)
      throw std::runtime_error("pfs::store::open: CID not found");

   fs_entry entry;
   entry.type        = entry_type::file;
   entry.size        = s->total_size;
   entry.content_cid = c;

   return file_handle(std::move(entry), &_cas);
}

// ── dir_cursor ────────────────────────────────────────────────────

dir_cursor::dir_cursor(psitri::read_session::ptr rs, uint32_t root, std::string prefix)
    : _rs(std::move(rs)),
      _cursor(_rs->create_cursor(root)),
      _prefix(std::move(prefix))
{
   _cursor.lower_bound(_prefix);
   _valid = skip_to_direct_child(true);
}

bool dir_cursor::valid() const { return _valid; }

const dir_entry& dir_cursor::entry() const { return _current; }

bool dir_cursor::next()
{
   if (!_valid)
      return false;
   _cursor.next();
   _valid = skip_to_direct_child(true);
   return _valid;
}

bool dir_cursor::prev()
{
   if (!_valid)
      return false;
   _cursor.prev();
   _valid = skip_to_direct_child(false);
   return _valid;
}

bool dir_cursor::skip_to_direct_child(bool forward)
{
   while (!(forward ? _cursor.is_end() : _cursor.is_rend()))
   {
      auto key = _cursor.key();
      if (key.size() < _prefix.size() ||
          std::memcmp(key.data(), _prefix.data(), _prefix.size()) != 0)
         return false;

      std::string_view remainder(key.data() + _prefix.size(),
                                 key.size() - _prefix.size());

      if (!remainder.empty())
      {
         auto slash = remainder.find('/');
         if (slash == std::string_view::npos || slash == remainder.size() - 1)
         {
            decode_current();
            return true;
         }
      }

      if (forward)
         _cursor.next();
      else
         _cursor.prev();
   }
   return false;
}

void dir_cursor::decode_current()
{
   auto key = _cursor.key();
   std::string_view remainder(key.data() + _prefix.size(),
                              key.size() - _prefix.size());

   auto val = _cursor.value<std::string>();
   if (val)
   {
      _current.name  = std::string(remainder);
      _current.entry = psio1::from_frac<fs_entry>(
          std::span<const char>(val->data(), val->size()));
   }
}

dir_cursor store::ls(psio1::name_id tenant, const path& p)
{
   auto path_str = normalize_path(p);
   if (!path_str.empty() && path_str.back() != '/')
      path_str += '/';

   auto root_idx = root_for_tenant(tenant);
   auto rs       = _db->start_read_session();
   auto prefix   = fs_dir_prefix(tenant, path_str);

   return dir_cursor(std::move(rs), root_idx, std::move(prefix));
}

dir_cursor store::ls(psio1::name_id tenant, const path& p, std::string_view after)
{
   auto path_str = normalize_path(p);
   if (!path_str.empty() && path_str.back() != '/')
      path_str += '/';

   auto root_idx = root_for_tenant(tenant);
   auto rs       = _db->start_read_session();
   auto prefix   = fs_dir_prefix(tenant, path_str);

   dir_cursor dc(std::move(rs), root_idx, prefix);
   if (!after.empty())
   {
      auto seek_key = prefix + std::string(after);
      dc._cursor.upper_bound(seek_key);
      dc._valid = dc.skip_to_direct_child(true);
   }
   return dc;
}

std::optional<fs_entry> store::stat(psio1::name_id tenant, const path& p)
{
   auto path_str = normalize_path(p);
   auto root_idx = root_for_tenant(tenant);

   auto rs     = _db->start_read_session();
   auto cursor = rs->create_cursor(root_idx);
   auto key    = fs_key(tenant, path_str);
   auto result = cursor.get<std::string>(key);
   if (!result)
      return std::nullopt;

   return psio1::from_frac<fs_entry>(
       std::span<const char>(result->data(), result->size()));
}

// ── Quota ──────────────────────────────────────────────────────────

void store::set_quota(psio1::name_id tenant, uint64_t limit)
{
   auto root_idx = root_for_tenant(tenant);
   auto ws       = _db->start_write_session();
   auto tx       = ws->start_transaction(root_idx);

   auto meta_key = fs_key(tenant, "$meta");
   auto existing = tx.get<std::string>(meta_key);

   fs_quota q;
   if (existing)
      q = psio1::from_frac<fs_quota>(
          std::span<const char>(existing->data(), existing->size()));

   q.limit = limit;
   auto val = psio1::to_frac(q);
   tx.upsert(meta_key, std::string_view(val.data(), val.size()));
   tx.commit();
}

fs_quota store::quota(psio1::name_id tenant)
{
   auto root_idx = root_for_tenant(tenant);
   auto rs       = _db->start_read_session();
   auto cursor   = rs->create_cursor(root_idx);

   auto meta_key = fs_key(tenant, "$meta");
   auto result   = cursor.get<std::string>(meta_key);
   if (!result)
      return {};

   return psio1::from_frac<fs_quota>(
       std::span<const char>(result->data(), result->size()));
}

// ── Sharing ────────────────────────────────────────────────────────

void store::share(psio1::name_id src,
                  const path&   src_path,
                  psio1::name_id dst,
                  const path&   dst_path)
{
   // Read source entry
   auto src_entry = stat(src, src_path);
   if (!src_entry)
      throw std::runtime_error("pfs::store::share: source not found");
   if (!src_entry->content_cid)
      throw std::runtime_error("pfs::store::share: source has no content CID");

   // Pin the CID (increment refcount)
   _cas.pin(*src_entry->content_cid);

   // Create destination entry
   auto dst_path_str = normalize_path(dst_path);
   auto root_idx     = root_for_tenant(dst);
   auto ws           = _db->start_write_session();
   auto tx           = ws->start_transaction(root_idx);

   auto key = fs_key(dst, dst_path_str);

   fs_entry entry  = *src_entry;
   entry.mtime_ns  = now_ns();

   auto val = psio1::to_frac(entry);
   tx.upsert(key, std::string_view(val.data(), val.size()));

   update_quota(tx, dst, static_cast<int64_t>(entry.size));
   tx.commit();
}

}  // namespace pfs
