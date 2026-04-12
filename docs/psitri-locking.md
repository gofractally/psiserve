PsiTri is a modified btree where divider keys are at most 1 byte,
       and this tree impliments COW semantics which requires reallocaing nodes to the root.

It provides a transaction interface that allows unlimited parallel readers, but only
a single writer, the single writer needs to gaurantee that during a transaction no other writers can touch anything that was read or written and thereby the total ordering of all transctions is as if everything were sequential. 

Readers grab the read lock long enough to increment the reference count of the root, then they can read the root and all children safely. A writer must block other writers (but not readers) until it is has swapped the root.


      writer:
         grab write-exclusion lock
         start doing edits in parallel tree..
         after all edits are done...
         grab the write side of the read-write lock (blocking readers)
         swap the root node.
         release read-write lock
         release write-exclusion lock 
         release old root node 

Currently we have a fixed number of nodes protected in this fashion; however, that is probably a bad design. What this represents is a "table-level" lock, and a database can in theory have any number of tables.

When the database "syncs to disk" these root node pointers must be synced, and the OS syncs at the page level, but in reality is SSD page sizes can be larger than os page sizes, and the min erase block is much larger than min write block and min write block can be larger than 4k... 

Bottom line, it doesn't cost us anything to sync rounded up to the nearest page; thus the fixed number of top roots.

From contention perspective, you don't want to store your "top roots" in atomics on the same cacheline; thus spreading them out over the page makes sense. 

From a developer friendliness perspective, table names are better than numbers. 

So now I want to introduce the following concepts:

- tree = a set of nodes containing key-value paires referenced by pointing to the root node
- subtree = is when a tree's root node is stored as a value in the key/value store of another tree

- two writers COW from the same tree at the same time; but, there is no means of merging their writes; therefore,
    we introduce tables, which are named trees which ensure that if one writer starts a COW with intent to replace the root that no other writer starts a COW modification of the same table.

- table = a named tree protected to ensure that two writers don't attempt to modify the tree at the same time.

Smart ways that parallelsm can be achieved with these primatives:

 - if you have a large master tree, and many writes in a narrow key range, you can use a sub tree, do all of your writes in a COW of the subtree and when done, do a single cow on the master tree to update the subtree. This works fine so long as:
     1. the transaction doring writes on the subtree doesn't need to read outside that subtree
     2. no one else wants to do a read of that area while writing to another area.
     3. in other words, we need a way to "lock" subtrees where the lock is scoped to the root tree

In Practice:
      Today we say writeLock(TopRootN) - no one can start a write transction within TopRootN
      Tomorrow we say  writeLock( TopRootN / key ) where key has a value of a subroot, 
          a transction can sspecify several of these keys and will acquire all locks in order. 

      On commit we say writeLock(TopRootN) and update just the keys with the subtrees, a much shorter critical section.

The question becomes where does one find the combination of write-exclusion/reader-writer locks for TopRootN/Key

and more broadly, how does this nest if subtrees have keys whose value is another subtree.. 

The critical insight is that we have a path.. 

TopRootN / key => subtree / subkey => subsubtree2 / ... 

The user ends up doing this:

WriteExcludeLock( subtree(TopRootN/Key) / subkey )
  COW updates produce newsubtree derived from subtree(TopRootN/key)
  (could be bulk of work)
  WriteExcludeLock( TopRootN ) 
      COW update TopRootN/key = newsubtree
      ReaderExcludeLock ( TopRootN )
         swap root pointer 
      ReaderExcludeUnLock ( TopRootN )
  WriteExcludeUnLock( TopRootN ) 
WriteExcludeUnLock( subtree(TopRootN/Key) / subkey )
          
              
Locks are not relocatable; therefore, they cannot be stored in leaf tree nodes. 
Locks do not need to be persistent; so they do not need to be on a memap file
We do not need to have a unique lock for every possible place to lock, we only need 
to have enough locks that two unrelated trees are unlikely to need the lock at the same time;
therefore I propose locks orgnaized in a flat has table:

   - Say there were a total of 8192 locks (this can be configurable) 
   - xxhash ( NodeAddress / "key" ) % 8192 becomes the index of the lock you grab

As long as a transaction calls 
    WriteExcludeLock(...) on every subtree it might read or write, then
    it can safely operate in paralell with other writers in the same higher level tree;
    and we end up with a database that looks like a filesystem, where keys are the names of the
    files and immediate directories in a folder, file is a key with a value, a subfolder is a subtree. 

If you want to modify a file, you only have to lock the directory it is in, this will be a COW 
   update to the directory... which can happen in parallel.. then when you are done with that you
   grab a write lock on the parent directory just long enough to update the subdirectory's pointer...
   recurse up to the top of the tree. 

End result is dramatic reduction in the total time the root() directory is locked even if the total 
number of edits to the root remains the same; throughput should be increased.

# MVCC  - multiversion concurrency control 

  The whole reason we have to update everything to the root is because, currently, everything is versioned from the root; however, there is another way of having multiple versions, each "key" can have multiple "values" each tagged with a version number. 
  Every root node is now a pair, Root + Version# 
  When you want to update the root you atomically increment the version number, then
  you traverse the tree and when you find the value node you like:
     1. grab a ValueLock for that value node
     2. COW the value node and add the new value with version number to it
     3. atomiclly swap in the new copy
            - all readers will either see the old version at old location
            - or the new version in new location, but they will not see anything mid wright
                    
 In this mode of operation, readers of a snapshot at version N, simply have to find the greatest revision less than or equal to version. For small numbers of snapshots or updates to a key this is a very fast SIMD search... for worst case this becomes a subtree traversal as all versions of the key end up in a subtree. 

Now in the copy-to-root and retain reference at root mode we had a nice and easy way to keep track of versions, when the root ref goes to 0, decrment the refs on children, when they go to 0, repeate... finding the nodes to clean up becomes easy.   However, in MVCC we would need a referenec count on the version number and only when *that* goes to 0 can we clean it up. 

  We pay a price in read performance to gain an advantage in write performance, but readers are parallel and writers are sequental.  Also, the consequence of this design is our ability to do rapid count(lower, upper) disapears and we can at best estimate or worst have to do a linear scan of not just all keys/values but all versions of those keys and values.  

  One way to address this clean up:
  1. flat hash table of active version numbers to their atomic reference counts 
  2. compactor can remove inactive version records... which is only those records for which there 
     exists a replace ment key >= the oldest version... I suppose with some work you could also free gaps
     between versions with snapshots... the problem we run into is that the block "free space" estimate that
     triggers compaction is now vastly under reporting. 

## Lock Provider Abstraction

psitri currently hardcodes `std::mutex` and `std::shared_mutex` arrays directly in the database class.
This blocks the OS thread on contention — incompatible with fiber schedulers, coroutine runtimes,
or single-threaded embeds. The same problem applies to any internal use of `std::atomic<T>::wait()`.

The fix: psitri never owns mutexes. It computes a hash index for every lock operation and calls
through an externally-provided lock map. The database class templates on the lock map type — one
template parameter, everything else stays concrete.

```cpp
// Concept: LockMap
//   void lock(uint32_t index, transaction& tx)
//   void unlock(uint32_t index, transaction& tx)
//   void lock_shared(uint32_t index)
//   void unlock_shared(uint32_t index)

template<typename LockMap = default_lock_map>
class database {
    LockMap& _locks;

    ptr_address start_transaction(root_object_number ro, transaction& tx) {
        _locks.lock(lock_index(ro), tx);
        return get(ro);
    }

    uint32_t lock_index(root_object_number ro) {
        return hash(ro) % _locks.size();
    }
};
```

### Host-provided implementations

**Standalone (blocking threads):**

    struct default_lock_map {
        std::array<std::mutex, 1024> _mutexes;
        void lock(uint32_t i, transaction&)   { _mutexes[i].lock(); }
        void unlock(uint32_t i, transaction&) { _mutexes[i].unlock(); }
        // shared variants wrap std::shared_mutex
    };

**psiserve (fiber-aware, wound-wait):**

    struct fiber_lock_map {
        Scheduler& _sched;
        std::array<fiber_tx_mutex, 8192> _mutexes;
        void lock(uint32_t i, transaction& tx)   { _mutexes[i].lock(_sched, tx); }
        void unlock(uint32_t i, transaction& tx) { _mutexes[i].unlock(_sched, tx); }
        // fiber_tx_mutex implements wound-wait: compare timestamps,
        // wound younger owner, yield fiber via sched.parkCurrentFiber()
    };

**Single-threaded (no-op):**

    struct noop_lock_map {
        void lock(uint32_t, transaction&)   {}
        void unlock(uint32_t, transaction&) {}
    };

### What this enables

- psitri never owns a mutex — lock storage, lifetime, and strategy belong to the host
- The host controls table size: 1024 for per-root, 8192 for subtree-granularity
- No virtual dispatch — template compiles away to direct calls
- Wound-wait, fiber yielding, and transaction awareness are entirely in the lock map
  implementation, invisible to psitri
- All internal uses of `std::mutex`, `std::shared_mutex`, `std::atomic::wait()` must go
  through the lock map to maintain this separation

### What needs to change in psitri

- Remove `std::array<std::mutex, 1024> _write_mutex` from database
- Remove `std::array<std::shared_mutex, 1024> _root_object_mutex` from database
- Add `template<typename LockMap>` to database class
- Replace all direct mutex usage with `_locks.lock(index, tx)` / `_locks.unlock(index, tx)`
- Audit for internal `std::atomic::wait()` calls and route through lock map
- Hash-based index computation for all lock operations (already conceptually present
  in start_transaction which indexes by root_object_number)

## MVCC Garbage Reclamation

The fundamental challenge: dead versions live inside live value nodes. The existing ref-count cascade
frees whole objects when ref→0, but dead version entries are sub-object garbage — embedded bytes
inside a node that's still alive. Three mechanisms work together to reclaim them, using infrastructure
that already exists or extends naturally.

### 1. Writer-Side Pruning During COW (Primary)

When a writer COWs a value node to add version V, it's already copying entries from old node to new
node. During that copy, skip dead entries:

    dead if: version < oldest_active_version
         AND a newer version ≤ oldest_active_version exists for this key

The new node comes out smaller. Zero extra allocation, zero extra control blocks, zero extra I/O —
the memcpy was happening anyway, just copy fewer bytes. This catches hot keys immediately since
they're being written to frequently.

The watermark (`oldest_active_version`) is the minimum across all retained snapshot versions —
not just current readers. A snapshot retained at version 5 keeps those version entries alive
regardless of whether any session is currently reading through it.

Active versions must be explicitly tracked in a snapshot registry:

    class snapshot_registry {
        std::mutex _mtx;  // snapshot create/release is rare
        // sorted (version, ref_count) pairs
        std::vector<std::pair<uint64_t, uint32_t>> _snapshots;
        std::atomic<uint64_t> _watermark{UINT64_MAX};  // cached min

    public:
        void retain(uint64_t version);   // add or increment ref
        void release(uint64_t version);  // decrement, remove if 0, update watermark
        uint64_t watermark() const;      // relaxed load of cached min
    };

    retain(): called when any snapshot is created (not just when a reader opens a session —
              also when a snapshot is saved for later use, backup, replication, etc.)
    release(): called when a snapshot is dropped (ref count on that version → 0)
    watermark(): cheap relaxed load, used by writers and compactor for pruning

For fast per-version checking during reads and compaction, the registry can export a bloom filter
of active versions. The bloom filter is rebuilt periodically (snapshot create/release is infrequent
relative to reads). False positives (think a version is active when it's not) are conservative —
the version survives until the next compaction pass. False negatives are impossible (standard bloom
filter property) — an active version is never mistakenly pruned.

The watermark provides coarse pruning (everything below oldest snapshot). The bloom filter provides
fine-grained pruning between active snapshots: a version V between two active versions can be
identified as dead if V is NOT in the bloom filter and a newer version in the same value node IS
in the bloom filter (or is newer than all active versions).

### 2. Reader Hints via Activity Bits (Discovery)

Readers already load and scan the value node to find their version. While scanning, they observe
dead versions at zero extra cost. The reader signals "this node has reclaimable versions" using
the control block's existing `active` and `pending_cache` bits via `try_inc_activity()`.

This is idempotent — 100 parallel readers of the same hot key all try CAS on the same bit. One
wins, 99 fail cheaply. No queues, no freed_space inflation, no over-reporting problem.

The compactor already processes `pending_cache` objects in `compactor_promote_rcache_data`. When
it encounters a value node with dead versions during promotion, it rewrites with versions stripped.
The node is already being copied to a hot segment — perfect time to take out the trash. This keeps
hot pinned memory free from dead data without any extra passes.

Reader check cost per value node: for each scanned version entry, one watermark comparison (single
uint64_t, cached in session) for coarse detection. For precise between-snapshot detection, K bloom
filter hash probes per version entry (K typically 3, ~10-15ns). If dead versions are found, one
`try_inc_activity()` CAS on the control block (already in cache from the version scan). Total
overhead for a node with 5 version entries: ~50-75ns — negligible relative to the cache line
loads the reader already paid for.

### 3. Hierarchical Dirty-Node Bitmap (Targeted Compaction)

For write-cold keys where dead versions accumulate without writers touching them, and which aren't
being promoted via the activity bits, a hierarchical bitmap provides O(1) dedup and fast scanning:

    Level 0: 1 bit per ptr_address — "this value node has dead versions"
    Level 1: 1 bit per 64 level-0 bits — "this group has at least one dirty node"
    Level 2: 1 bit per 64 level-1 bits — coarse scan entry point

    Set: reader sets bit at ptr_address hash, then propagates up (atomic OR, idempotent)
    Scan: compactor walks level-2, skips zero words, drills into level-1/0 for dirty nodes
    Clear: compactor clears bits after pruning (atomic AND-NOT)

Thread safety: each level is an array of `std::atomic<uint64_t>`. Set is `fetch_or` (idempotent,
no races). Clear is `fetch_and`. Scan is relaxed load. No locks anywhere.

Space: with 4M max control blocks (22-bit ptr_address), level-0 is 512KB, level-1 is 8KB,
level-2 is 128 bytes. Total ~520KB — fits in L2 cache.

Reader cost: hash the ptr_address (already known), one `fetch_or` on level-0, one `fetch_or` on
level-1. ~2 atomic ops, both likely in cache for hot nodes. The reader also checks observed
versions against the watermark to decide whether to set the bit — same check it's already doing
for the activity bit path.

Compactor benefit: instead of walking entire segments to find value nodes with dead versions,
scan the level-2 bitmap (2 cache lines), drill into non-zero groups. This finds dirty nodes
across ALL segments in microseconds, enabling targeted rewrites without full compaction.

### 4. Compactor Prunes During Segment Compaction (Sweep)

During `compact_to` / `try_copy_node`, when copying a value node, apply the same pruning logic as
the writer. Uses the full active version set (not just watermark) for precise pruning — can
identify dead versions between active readers.

This catches everything eventually, regardless of whether readers or writers flagged it.

### Bounding the Worst Case

A hot key updated millions of times with a long-lived reader can grow the value node unboundedly.
Cap the inline version array (e.g., 32 entries) and spill overflow into a version subtree. That
subtree is itself a normal psitri tree — no new machinery, and the spill case is rare enough that
the extra control blocks and indirection don't matter.

### Summary

| Mechanism | Catches | Cost | Infrastructure |
|-----------|---------|------|----------------|
| Writer prune during COW | Hot keys (written frequently) | Zero — already copying | Snapshot registry watermark + bloom filter |
| Reader hint via activity bits | Hot reads, promotes to pinned memory clean | Bloom check + 1 CAS | Existing `try_inc_activity` + compactor promote path |
| Hierarchical dirty bitmap | Write-cold keys across all segments | 2 atomic ORs per reader hint | ~520KB bitmap, scan in µs |
| Compactor prune during compact_to | Everything eventually | Zero — already copying | Full snapshot registry |

No new background threads. No new queues. No freed_space inflation. Dead versions are reclaimed
at the earliest opportunity — during the COW that's already happening (writer), during the
promotion that's already happening (compactor+activity bits), or via targeted lookup (bitmap).


         
