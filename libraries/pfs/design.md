# pfs — IPFS-Compatible Filesystem on psitri

## Overview

pfs is a C++ library that provides an IPFS-compatible content-addressable filesystem
built on top of psitri. It is a standalone library (`libraries/pfs/`) that links
against psitri but does not modify it. Three composable layers, each usable
independently:

```
┌─────────────────────────────────────────────┐
│  filesystem    paths, dirs, tenants, quotas  │  ← pfs::filesystem
├─────────────────────────────────────────────┤
│  cas           CID-keyed dedup, refcounts    │  ← pfs::cas
├─────────────────────────────────────────────┤
│  block_store   4MB blocks in psitri roots    │  ← pfs::block_store
├─────────────────────────────────────────────┤
│  psitri        btree, DWAL, SAL, COW, MVCC   │  (external dependency)
└─────────────────────────────────────────────┘
```

Each layer exposes a session-based API that mirrors psitri's threading model
(one session per thread).

## Root and key layout

pfs uses **256 psitri roots** (one per shard), with a configurable
**base root** so multiple applications can coexist in the same database.
All three logical tables (blocks, CAS entries, filesystem entries) live
in the **same shard**, distinguished by a one-byte key prefix:

| Prefix | Table       | Key after prefix                          |
|--------|-------------|-------------------------------------------|
| `'B'`  | block_store | block_id (uint64 big-endian)              |
| `'C'`  | cas         | raw CID binary (36 bytes for CIDv1/dag-pb/SHA-256) |
| `'F'`  | filesystem  | name_id (uint64 big-endian) + path bytes  |

Shard selection:
- block_store: `hash(block_id) % shard_count`
- cas: `hash(CID) % shard_count`
- filesystem: `hash(tenant.value) % shard_count`

Tenant is `psio1::name_id` — a 64-bit arithmetic-encoded name with
human-readable string conversion (`"alice"_n`). Fixed 8 bytes in the key,
no separator needed.

Root index = `config.root_base + shard`. All three tables within a shard
share one psitri root, so a write that touches CAS + filesystem in the
same shard is a single transaction on one root — no cross-root atomicity
needed when hashes align.

```cpp
namespace pfs {

struct config {
    uint32_t root_base   = 0;    // first psitri root index (256 roots used)
    uint32_t shard_count = 256;  // number of shards (must be power of 2, ≤ 256)
};

} // namespace pfs
```

**Total roots consumed: `shard_count`** (default 256). The caller must
ensure `root_base + shard_count` doesn't exceed psitri's root limit.

---

## Schemas (fracpack)

All values are fracpack-serialized via `psio1::to_frac()` / `psio1::from_frac()`.
Key bytes are hand-built (prefix + raw bytes), not fracpacked.

### Key helpers

```cpp
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace pfs {

// Build a block_store key: 'B' + block_id (big-endian uint64) = 9 bytes
inline std::string block_key(uint64_t block_id)
{
    std::string k(9, '\0');
    k[0] = 'B';
    for (int i = 0; i < 8; ++i)
        k[1 + i] = static_cast<char>((block_id >> (56 - 8 * i)) & 0xFF);
    return k;
}

// Build a CAS key: 'C' + 36-byte raw CID = 37 bytes total
inline std::string cas_key(const cid& c)
{
    std::string k(1 + cid::size, '\0');
    k[0] = 'C';
    std::memcpy(k.data() + 1, c.bytes.data(), cid::size);
    return k;
}

// Encode name_id as 8 big-endian bytes
inline void encode_name(std::string& k, psio1::name_id tenant)
{
    uint64_t v = tenant.value;
    for (int i = 0; i < 8; ++i)
        k.push_back(static_cast<char>((v >> (56 - 8 * i)) & 0xFF));
}

// Build a filesystem key: 'F' + name_id (8 bytes BE) + path
// Fixed 9-byte prefix per tenant, no separator needed.
inline std::string fs_key(psio1::name_id tenant, std::string_view path)
{
    std::string k;
    k.reserve(9 + path.size());
    k += 'F';
    encode_name(k, tenant);
    k += path;
    return k;
}

// Prefix for scanning all FS entries of a given tenant
inline std::string fs_tenant_prefix(psio1::name_id tenant)
{
    std::string k;
    k.reserve(9);
    k += 'F';
    encode_name(k, tenant);
    return k;
}

// Prefix for scanning a directory within a tenant
inline std::string fs_dir_prefix(psio1::name_id tenant, std::string_view dir_path)
{
    std::string k;
    k.reserve(9 + dir_path.size());
    k += 'F';
    encode_name(k, tenant);
    k += dir_path;
    return k;
}

} // namespace pfs
```

### block_store value

Block values are **raw bytes** (no fracpack wrapper). The value IS the
block payload, up to 4MB. No metadata header — block_id encodes the shard,
and compression flags are reserved for v2.

```
key:    'B' + block_id (big-endian uint64)   = 9 bytes
value:  raw bytes (up to 4MB)
```

### cas_entry

```cpp
#pragma once
#include <psio1/reflect.hpp>
#include <cstdint>
#include <vector>

namespace pfs {

struct chunk_ref {
    uint64_t              block_id;    // block_store block containing this chunk
    uint32_t              offset;      // byte offset within the block
    uint32_t              size;        // chunk size (usually 256KB, last may be smaller)
};
PSIO1_REFLECT(chunk_ref, block_id, offset, size)

struct cas_entry {
    uint32_t              refcount   = 0;
    uint64_t              total_size = 0;   // original file size in bytes
    std::vector<uint8_t>  inline_data;      // non-empty for small files (≤ 256KB)
    std::vector<chunk_ref> chunks;          // non-empty for large files (> 256KB)
    // Exactly one of inline_data/chunks is populated.
    // inline_data.empty() == true means chunked storage.
};
PSIO1_REFLECT(cas_entry, refcount, total_size, inline_data, chunks)

} // namespace pfs
```

**Size rule**: if `total_size ≤ 256KB` (one IPFS chunk), data goes in
`inline_data`. Otherwise data is split into 256KB chunks packed into 4MB
blocks, and `chunks` holds the manifest.

This means single-chunk files (the vast majority by count) never touch
block_store — one psitri entry, one control_block, done.

### fs_entry

```cpp
#pragma once
#include <psio1/reflect.hpp>
#include <cstdint>
#include <vector>

namespace pfs {

enum class entry_type : uint8_t {
    file      = 0,
    directory = 1,
};
PSIO1_REFLECT(entry_type)

struct fs_entry {
    entry_type            type     = entry_type::file;
    uint16_t              mode     = 0644;   // unix permission bits
    uint32_t              owner    = 0;      // user_id within tenant
    uint64_t              mtime_ns = 0;      // nanoseconds since epoch
    uint64_t              size        = 0;   // file size (0 for dirs)
    std::optional<cid>    content_cid;       // present for files, absent for dirs
};
PSIO1_REFLECT(fs_entry, type, mode, owner, mtime_ns, size, content_cid)

} // namespace pfs
```

### fs_quota (stored at special key)

```cpp
namespace pfs {

struct fs_quota {
    uint64_t limit = 0;   // max bytes allowed
    uint64_t used  = 0;   // sum of logical file sizes
};
PSIO1_REFLECT(fs_quota, limit, used)

} // namespace pfs
```

Key: `fs_key(tenant, "$meta")` → `'F' + name_id(8 bytes) + "$meta"`

The `$` prefix is illegal in normal paths, so there's no collision with
user files.

---

## Query plans

All queries go through psitri's `transaction` / `tree_handle` (for writes)
or `cursor` (for reads). The root index for any key is always:
`config.root_base + shard(key)`.

### block_store queries

| Operation | psitri call | Key |
|-----------|------------|-----|
| alloc(data) | `tx.upsert(block_key(id), data)` | `'B' + id` |
| free(id) | `tx.remove(block_key(id))` | `'B' + id` |
| read(id) | `tree.get(block_key(id))` → raw bytes | `'B' + id` |
| read_range(id, off, len) | `tree.get(block_key(id))` → slice `[off, off+len)` | `'B' + id` |

All block_store operations are point lookups — no scans.

### CAS queries

| Operation | psitri call | Key |
|-----------|------------|-----|
| put(data) | compute CID, `tree.get(cas_key(cid))` to check existence; if exists: read→increment refcount→`tx.update()`; else: alloc blocks, `tx.insert(cas_key(cid), fracpack(cas_entry))` | `'C' + cid` |
| pin(cid) | `tree.get()` → deserialize → refcount++ → `tx.update()` | `'C' + cid` |
| unpin(cid) | `tree.get()` → deserialize → refcount-- → if 0: free blocks + `tx.remove()`; else `tx.update()` | `'C' + cid` |
| get(cid) | `tree.get(cas_key(cid))` → deserialize → if inline: return inline_data; else: read each chunk from block_store, concatenate | `'C' + cid` |
| get_range(cid, off, len) | deserialize cas_entry → compute which chunks overlap `[off, off+len)` → read only those chunks from block_store → slice to exact range | `'C' + cid` + block reads |
| stat(cid) | `tree.get(cas_key(cid))` → deserialize → return {total_size, chunks.size(), refcount} | `'C' + cid` |
| ls(cid) | deserialize cas_entry → reconstruct UnixFS DAG links from chunk list (deterministic: fixed 256KB chunker → known chunk CIDs) | `'C' + cid` |
| pin/ls (list all) | `cursor.first("C")` → iterate while key starts with `'C'` → collect entries with refcount > 0 | prefix scan `'C'` |

All CAS operations except `pin/ls` are point lookups on the CID key.
`get_range` is one point lookup (CAS entry) + targeted block reads.

### Filesystem queries

| Operation | psitri call | Key |
|-----------|------------|-----|
| write(t, path, data) | CAS put → `tx.upsert(fs_key(t,path), fracpack(fs_entry))` + update quota at `fs_key(t,"$meta")` | `'F' + name_id + path` |
| remove(t, path) | read old entry for CID → `tx.remove(fs_key(t,path))` → CAS unpin(old.cid) → update quota | `'F' + name_id + path` |
| mkdir(t, path) | `tx.upsert(fs_key(t,path), fracpack(fs_entry{directory}))` | `'F' + name_id + path` |
| chmod(t, path) | read → modify mode → `tx.update()` | `'F' + name_id + path` |
| stat(t, path) | `tree.get(fs_key(t,path))` → deserialize | `'F' + name_id + path` |
| read(t, path) | stat → CAS get(entry.cid) | point + CAS read |
| read_range(t, path, off, len) | stat → CAS get_range(entry.cid, off, len) | point + CAS range |
| get_quota(t) | `tree.get(fs_key(t,"$meta"))` → deserialize fs_quota | `'F' + name_id + $meta` |
| set_quota(t, limit) | upsert fs_quota at `fs_key(t,"$meta")` | `'F' + name_id + $meta` |

#### Directory listing (ls)

```cpp
// ls("alice"_n, "photos/")
auto prefix = fs_dir_prefix("alice"_n, "photos/");

auto root = config.root_base + shard("alice"_n);
auto cursor = read_session.create_cursor(root);
cursor.first(prefix);  // seek to first key with this prefix

std::vector<dir_entry> results;
while (!cursor.is_end()) {
    auto key = cursor.key();
    // key still starts with prefix?
    if (key.size() < prefix.size() ||
        memcmp(key.data(), prefix.data(), prefix.size()) != 0)
        break;

    // Extract the part after the prefix
    std::string_view remainder(key.data() + prefix.size(),
                               key.size() - prefix.size());

    // Direct child = no '/' in remainder (or only trailing '/')
    auto slash = remainder.find('/');
    if (slash == std::string_view::npos || slash == remainder.size() - 1) {
        auto val = cursor.value<std::string>();
        auto entry = psio1::from_frac<fs_entry>(*val);
        results.push_back({std::string(remainder), entry_to_stat(entry)});
    }
    // else: deeper nested entry, skip (not a direct child)

    cursor.next();
}
```

This is O(entries under prefix) — psitri's sorted btree makes the prefix
scan efficient. For very large directories, the cursor streams without
loading all leaves at once.

#### Recursive remove

```cpp
// rm -r "alice"_n / "photos/"
auto prefix = fs_dir_prefix("alice"_n, "photos/");
auto root_idx = config.root_base + shard("alice"_n);
auto tx = write_session.start_transaction(root_idx);
auto tree = tx.primary();

// Collect all CIDs to unpin
std::vector<std::vector<uint8_t>> cids_to_unpin;
auto cursor = tree.read_cursor();
cursor.first(prefix);
while (!cursor.is_end()) {
    if (!starts_with(cursor.key(), prefix)) break;
    auto entry = psio1::from_frac<fs_entry>(cursor.value<std::string>());
    if (entry.type == entry_type::file && !entry.cid.empty())
        cids_to_unpin.push_back(entry.cid);
    cursor.next();
}

// Remove the range and update quota
tree.remove_range(prefix, prefix_upper_bound(prefix));
// ... update quota ...
tx.commit();

// Unpin CIDs (may be in different shards, separate transactions)
for (auto& c : cids_to_unpin)
    cas_session.unpin(c);
```

Uses `tree.remove_range()` for bulk deletion — one btree operation
instead of N individual removes.

#### Pin listing (scan all CAS entries)

```cpp
// List all pinned CIDs across all shards
for (uint32_t shard = 0; shard < config.shard_count; ++shard) {
    auto cursor = read_session.create_cursor(config.root_base + shard);
    cursor.first("C");  // seek to first CAS entry in this shard
    while (!cursor.is_end()) {
        if (cursor.key()[0] != 'C') break;  // past CAS prefix
        auto entry = psio1::from_frac<cas_entry>(cursor.value<std::string>());
        if (entry.refcount > 0) {
            auto key = cursor.key();
            auto c = cid::from_bytes(
                std::span<const uint8_t, cid::size>(
                    reinterpret_cast<const uint8_t*>(key.data() + 1), cid::size));
            results.push_back({c, entry.refcount, entry.total_size});
        }
        cursor.next();
    }
}
```

This is a full scan of the `'C'` key space. Expensive but pin listing is
an admin operation, not a hot path.

---

## Layer 1: block_store

Raw block allocation and I/O. Manages 4MB logical blocks in psitri, one
control_block each.

### Block ID allocation

Block IDs encode their shard: `(shard << 56) | sequence`. Top 8 bits =
shard, bottom 56 bits = per-shard sequence. Allocation is lock-free
within a shard.

```cpp
struct block_id { uint64_t id; };

// Extract shard from a block_id
inline uint32_t block_shard(block_id b) { return b.id >> 56; }
```

### API

```cpp
namespace pfs {

class block_store {
public:
    block_store(std::shared_ptr<psitri::database> db, const config& cfg = {});

    class write_session {
    public:
        block_id alloc(std::span<const uint8_t> data);  // ≤ 4MB
        void     free(block_id);
        void     read(block_id, std::vector<uint8_t>& out);
    };

    class read_session {
    public:
        void read(block_id, std::vector<uint8_t>& out);
    };

    write_session start_write_session();
    read_session  start_read_session();
};

} // namespace pfs
```

### Direct-COW write path (future)

Large block writes bypass DWAL:

1. `alloc()` allocates SAL cachelines for the block data.
2. Writes bytes directly; calls `fsync`.
3. Commits the block_id → location mapping through DWAL (metadata only).

Not in v1 — initial implementation uses normal `tx.upsert()` which goes
through DWAL. Direct-COW is a performance optimization for later.

## Layer 2: cas (Content-Addressable Store)

CID-keyed file storage with file-level dedup and reference counting.

### CID computation

IPFS-compatible CIDs so stock clients (Kubo CLI, ipfs-http-client, browser
extensions) work unchanged:

- **Chunker**: fixed-size 256KB (IPFS default)
- **DAG codec**: UnixFS v1 (dag-pb protobuf)
- **Hash**: SHA-256
- **CID format**: CIDv1, base32 multibase

The CID computation happens entirely in pfs — no external IPFS daemon.

Implementation: a small UnixFS DAG builder that:
1. Splits input bytes into 256KB chunks.
2. Wraps each chunk in a UnixFS protobuf Data node (type=File, data=chunk).
3. Serializes each as a dag-pb block, computes SHA-256 → per-chunk CID.
4. If file has multiple chunks, builds an intermediate UnixFS node with
   Links entries pointing at chunk CIDs + sizes.
5. Returns the root CID.

For single-chunk files (≤ 256KB), this simplifies to one protobuf wrap +
one SHA-256.

### Dedup

File-level, not chunk-level. On `put(bytes)`:
1. Compute CID from bytes.
2. If CAS entry exists for that CID: increment refcount, update in place. Done.
3. Else: if large, chunk bytes into block_store blocks; create CAS entry
   with refcount=1.

On `unpin(CID)`:
1. Decrement refcount.
2. If refcount hits 0: free all block_store blocks, delete CAS entry.

### API

```cpp
namespace pfs {

// Fixed 36 bytes: 0x01 (CIDv1) + 0x70 (dag-pb) + 0x12 (sha2-256) + 0x20 (32) + <32-byte digest>
// Base32 text encoding only at the HTTP/display boundary, never on disk.
struct cid {
    static constexpr uint32_t digest_size = 32;
    static constexpr uint32_t header_size = 4;   // version + codec + hash-fn + digest-len
    static constexpr uint32_t size        = header_size + digest_size;  // 36

    std::array<uint8_t, size> bytes = {
        0x01,  // CIDv1
        0x70,  // dag-pb
        0x12,  // sha2-256
        0x20,  // 32-byte digest
    };

    // Construct from a raw SHA-256 digest (32 bytes)
    static cid from_digest(std::span<const uint8_t, digest_size> digest);

    // Parse from base32-lower multibase string ("bafy...")
    static cid from_string(std::string_view s);

    // Parse from raw 36-byte binary
    static cid from_bytes(std::span<const uint8_t, size> raw);

    // Encode as base32-lower multibase string ("b" + base32)
    std::string to_string() const;

    // Access the 32-byte SHA-256 digest
    std::span<const uint8_t, digest_size> digest() const
    {
        return std::span<const uint8_t, digest_size>(bytes.data() + header_size, digest_size);
    }

    auto operator<=>(const cid&) const = default;
    bool operator==(const cid&) const  = default;

    friend std::ostream& operator<<(std::ostream& os, const cid& c)
    {
        return os << c.to_string();
    }
};
static_assert(sizeof(cid) == 36);

// Fracpack: serialize as flat 36 bytes (same as std::array<uint8_t, 36>).
// No struct framing, no heap offset — just raw memcpy.
namespace psio1 {
    template<> struct is_packable_memcpy<pfs::cid> : std::bool_constant<true> {};
}

struct cas_stat {
    uint64_t total_size;
    uint32_t chunk_count;
    uint32_t refcount;
};

class cas {
public:
    cas(std::shared_ptr<psitri::database> db, block_store& bs, const config& cfg = {});

    class write_session {
    public:
        cid  put(std::span<const uint8_t> data);
        void pin(const cid& c);
        void unpin(const cid& c);
    };

    class read_session {
    public:
        std::vector<uint8_t>   get(const cid& c);
        std::vector<uint8_t>   get_range(const cid& c, uint64_t offset, uint64_t length);
        std::optional<cas_stat> stat(const cid& c);

        struct dag_link { cid link_cid; uint64_t size; std::string name; };
        std::vector<dag_link> ls(const cid& c);
    };

    write_session start_write_session();
    read_session  start_read_session();
};

} // namespace pfs
```

## Layer 3: filesystem

Multi-tenant namespace with paths, directories, permissions, and quotas.
This is the user-facing layer.

### Path conventions

- Paths are relative to the tenant root, no leading `/`.
- Directories end with `/` in the key (e.g., `photos/`).
- Files do not end with `/` (e.g., `photos/cat.jpg`).
- `$meta` is reserved for quota metadata.
- Tenant is `psio1::name_id` (64-bit, fixed 8 bytes in key).

### Write path

```
write(tenant, path, bytes):
    cid = cas.put(bytes)                    // may hit dedup
    old = lookup(tenant, path)
    upsert(tenant, path, {file, cid, size, ...})
    quota_used[tenant] += size - (old ? old.size : 0)
    enforce: quota_used <= quota_limit
    if old: cas.unpin(old.cid)              // may free blocks
```

The CAS entry (sharded by CID hash) and filesystem entry (sharded by
tenant hash) may land in the same or different roots. When they're in
the same root, both updates are a single psitri transaction. When they're
in different roots, we write CAS first (idempotent on dedup-hit, safe to
retry) then filesystem. A crash between the two leaves an orphan CAS
refcount bump — harmless, cleaned up by periodic GC scan.

### Cross-tenant sharing

Sharing a file between tenants = one filesystem entry write in the
destination tenant + one CAS `pin(cid)`. Zero bytes copied.

### Snapshots

A tenant snapshot:
1. Copy the tenant's filesystem subtree (psitri COW — structural sharing,
   nearly free).
2. `pin(cid)` for every CID reachable under that subtree (linear in file
   count, not byte count).

### Developer API

```cpp
namespace pfs {

using path = std::filesystem::path;

struct dir_entry {
    std::string name;
    fs_entry    entry;
};

// Handle to an open file — caches cas_entry for size/range queries.
// Lightweight, no lock held. Snapshot-consistent for reads.
class file_handle {
public:
    uint64_t          size() const;
    const cid&        content_cid() const;
    const fs_entry&   stat() const;

    // Zero-copy read — cb invoked with each chunk's mapped data.
    // May call cb multiple times across chunk boundaries.
    void read(uint64_t offset, uint64_t length,
              std::function_ref<void(std::span<const uint8_t>)> cb);
    void read(std::function_ref<void(std::span<const uint8_t>)> cb);
};

class store {
public:
    store(std::shared_ptr<psitri::database> db, config cfg = {});

    // ── File handles (read) ─────────────────────────────────────────
    file_handle open(psio1::name_id tenant, const path& p);
    file_handle open(const cid& c);

    // ── Mutations ───────────────────────────────────────────────────
    cid  write(psio1::name_id tenant, const path& p,
               std::span<const uint8_t> data,
               uint16_t mode = 0644, uint32_t owner = 0);
    void remove(psio1::name_id tenant, const path& p);
    void mkdir(psio1::name_id tenant, const path& p,
               uint16_t mode = 0755, uint32_t owner = 0);
    void chmod(psio1::name_id tenant, const path& p, uint16_t mode);

    // ── Directory listing ───────────────────────────────────────────
    void ls(psio1::name_id tenant, const path& p,
            std::function_ref<void(const dir_entry&)> cb);

    // ── Metadata ────────────────────────────────────────────────────
    std::optional<fs_entry> stat(psio1::name_id tenant, const path& p);

    // ── Quota ───────────────────────────────────────────────────────
    void       set_quota(psio1::name_id tenant, uint64_t limit);
    fs_quota   quota(psio1::name_id tenant);

    // ── Sharing / snapshots ─────────────────────────────────────────
    void share(psio1::name_id src, const path& src_path,
               psio1::name_id dst, const path& dst_path);
    void snapshot(psio1::name_id tenant, psio1::name_id snapshot_id);

    // ── IPFS gateway ────────────────────────────────────────────────
    // Resolve /ipfs/<cid>/<subpath> — returns handle to the resolved file
    file_handle resolve(const cid& root, const path& subpath = {});
};

} // namespace pfs
```

Thread-local psitri sessions are acquired internally from the shared `db`.
The block_store and CAS layers are implementation details — not exposed.

Usage:
```cpp
auto db = psitri::database::open("/data/mydb");
pfs::store fs(db, {.root_base = 0});

auto c = fs.write("alice"_n, "photos/cat.jpg", bytes);
auto h = fs.open("alice"_n, "photos/cat.jpg");
std::cout << "size: " << h.size() << ", cid: " << h.content_cid() << "\n";

// Zero-copy read into WASM linear memory
h.read(0, h.size(), [&](std::span<const uint8_t> chunk) {
    memcpy(wasm_mem + guest_ptr, chunk.data(), chunk.size());
    guest_ptr += chunk.size();
});

// List a directory
fs.ls("alice"_n, "photos/", [](const pfs::dir_entry& e) {
    std::cout << e.name << "\n";
});
```

## IPFS HTTP API (served by psiserve WASM service)

The HTTP layer is NOT part of the pfs library. It's a thin WASM service
running on psiserve that translates IPFS HTTP RPC to pfs C++ calls.
Supported endpoints:

### Blockstore / CAS endpoints
```
POST /api/v0/add                   → cas.put(body)
GET  /api/v0/cat?arg=<cid>         → cas.get(cid)
GET  /api/v0/get?arg=<cid>         → cas.get(cid) as tar
GET  /api/v0/ls?arg=<cid>          → cas.ls(cid)
POST /api/v0/pin/add?arg=<cid>     → cas.pin(cid)
POST /api/v0/pin/rm?arg=<cid>      → cas.unpin(cid)
GET  /api/v0/pin/ls                → list all pinned CIDs
```

### MFS (Mutable File System) endpoints
```
POST /api/v0/files/write?arg=<path>  → fs.write(tenant, path, body)
GET  /api/v0/files/read?arg=<path>   → fs.read(tenant, path)
GET  /api/v0/files/ls?arg=<path>     → fs.ls(tenant, path)
POST /api/v0/files/mkdir?arg=<path>  → fs.mkdir(tenant, path)
POST /api/v0/files/rm?arg=<path>     → fs.remove(tenant, path)
POST /api/v0/files/stat?arg=<path>   → fs.stat(tenant, path)
```

### Gateway
```
GET /ipfs/<cid>                    → cas.get(cid) with Range support
GET /ipfs/<cid>/<subpath>          → resolve UnixFS path within DAG
```

Tenant is determined by HTTP session/auth. Single-tenant mode maps
everything to a default tenant, making `ipfs files ls /` work out of
the box for Kubo CLI compatibility.

## Dependencies

```
pfs
├── psitri (database, transactions, cursors)
├── psio   (fracpack serialization of cas_entry / fs_entry / fs_quota)
├── openssl or libsodium (SHA-256 for CID computation)
└── hand-rolled dag-pb/UnixFS encoder (< 100 lines)
```

## Directory structure

```
libraries/pfs/
├── CMakeLists.txt
├── design.md                  (this file)
├── include/pfs/
│   ├── block_store.hpp
│   ├── cas.hpp
│   ├── filesystem.hpp
│   ├── cid.hpp               (CID computation, multibase, multihash)
│   ├── unixfs.hpp             (dag-pb/UnixFS encoding)
│   ├── keys.hpp               (key builders: block_key, cas_key, fs_key)
│   └── schema.hpp             (fracpack types: cas_entry, fs_entry, fs_quota, chunk_ref)
├── src/
│   ├── block_store.cpp
│   ├── cas.cpp
│   ├── filesystem.cpp
│   ├── cid.cpp
│   └── unixfs.cpp
└── tests/
    ├── block_store_tests.cpp
    ├── cas_tests.cpp
    ├── filesystem_tests.cpp
    ├── cid_tests.cpp
    └── unixfs_tests.cpp
```

## Implementation order

1. **schema.hpp** — fracpack types (cas_entry, fs_entry, fs_quota, chunk_ref)
   with PSIO1_REFLECT. Pure types, no logic.
2. **keys.hpp** — key builders and shard computation. Unit test key encoding
   round-trips and sort order.
3. **cid.hpp/cpp** — CID computation (SHA-256 + multihash + multicodec + CIDv1).
   Test against known IPFS CID vectors.
4. **unixfs.hpp/cpp** — dag-pb + UnixFS protobuf encoding. Minimal hand-rolled
   encoder for the File data node and directory link types.
5. **block_store** — 4MB block allocation (`'B'`-prefixed keys).
6. **cas** — CID-keyed dedup store (`'C'`-prefixed keys).
7. **filesystem** — Tenant namespaces (`'F'`-prefixed keys).
8. **Integration tests** — Round-trip: write file via filesystem, read via CAS CID,
   verify CID matches Kubo output for same bytes.

## Open questions

1. **Inline threshold**: 256KB (one IPFS chunk) is the current plan. This means
   single-chunk files never touch block_store. Could go lower (64KB) to reduce
   psitri value sizes at the cost of more block_store round-trips for medium files.

2. **Cross-shard writes**: When CAS shard (by CID hash) differs from filesystem
   shard (by tenant hash), we do two separate transactions. CAS-first is safe
   (idempotent on dedup). Orphan refcount bumps from crashes between the two are
   cleaned by periodic GC. Is this sufficient, or do we want a two-phase commit?

3. **Streaming writes**: For large uploads, we want to stream chunks to block_store
   as they arrive rather than buffering the whole file. The CID can only be computed
   once all chunks are known. Plan: buffer chunk CIDs in memory during stream,
   compute root CID at end, then commit CAS entry.

4. **DAG-level storage**: Don't store intermediate UnixFS DAG nodes — reconstruct
   on the fly from the chunk list. The DAG structure is deterministic from the
   fixed-size 256KB chunker.
