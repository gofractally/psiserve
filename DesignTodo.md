# Used to Track Development Plan and Design Dependencies 

psiserve needs psitri database to be exposed to wasm instances
   - this means psitri public-api needs to be defined
   - psitri locking system needs to be flexible enough to work with fibers
   - wasm hosts need the ability to expose psitri cursors/transactions as
     resources
   - config file needs ability to control what databases/tables/files each
     wasm has access to. 

Step 1 is to determine what locking, if-any, should be done at the SAL level. 
   SAL is Smart Allocator with movable memory, it is designed to generically handle
   movable reference counted objects. 

   PsiTri is a btree variant built on SAL that defines locking mechanics 
   psizam (pzam) is the web assembly engine
   psio reflects interfaces and defines serializations. 

   These things come together in defining the WIT interface of the Host.

   There are N top roots of the database, writers writing to different top-roots 
   can write in parallel with 0 synchronization overhead. We will call these 
   top roots "virtual databases" and the config file will give names to the numbers,
   and assign access rights (RO or RW) that each wasm "virtual process" has access to. 

   Each "virtual database" will have any number of named "tables" where a table's name
   is the key that lets you lookup the table's subtree.

   WASMs should have no exposure to the subtree ID or they could end up creating cycles
   in the node retention that produces a database memory leak. 


   Root [0] = "webserver.db"
      |  "users" =>  [subtree table] 
      |  "permissions" => ...

   Thinking through abstract API
   
   dbRef = openDb( "webserver.db" )
   curRef = txRef.get()/.find()/.lowerBound()/.upperBound()
   curRef.valid() / .next() / .prev() / .key() / .value() /.begin() / .end() / .front() / .back()

   tableRef = dbRef.createTable( "users" ) / dropTable( "users" )
   txRef = tableRef.startTransaction();
   txCurRef = txRef.get()/.find()/.lowerBound()/.upperBound()
   txCurRef.valid() / .next() / .prev() / .key() / .value() /.begin() / .end() / .front() / .back()
   txRef.insert/upsert/remove/update/removeRange()
   subTxRef = txRef.startSubTransaction()
     ... 
   subTxRef.abort/commit
   txRef.abort()/commit()

   One of the apps that we will be running will be a blockchain and the blockchain needs 
   to track snapshots at various points in time, but the snapshot API must not give people the 
   power to create cycles; therefore: snapshots need to have their own "virtual database" that
   is able to store other "top roots" as subtrees on named keys. 

   Root[1] = "snapshots" 

   createSnapshot( dbRef, "snapshot name" )
       Root[1] / "rootN-snapshot name" => RootN's value at time of snapshot
   releaseSnapshot( dbRef, "snapshot name" )
   snapShotsdbRef = openSnapshot( dbRef, "snapshot name" );
   snapShotsdbRef.
   dbRef.restoreToSnapshot( snapshotDbRef );

   I may be missing some things, but this seems to be the approximate shape of the guest API.

   The host needs to manage permissions and track resource use of each table:
       1. number of keys - per table
       2. size of all keys, size of all values per table
   
    We need an API to list/iterate/count total number of tables in each virtual database. 


  1. Watchdog is signal-based (watchdog.hpp) — wall-clock timers are non-deterministic by construction. Consensus needs
  instruction-count-based gas metering, not SIGALRM.
          - gas metering doesn't capture native host costs, our consensus algorithm trusts the producer to record and bill
          accordingly; thus this more effecient method is what we can use. 



  2. jit2 segfaults are memory-safety bugs, not just test failures (KNOWN_ISSUES.md):
    - simd_const_385/387 — SIMD segfault
    - conversions_0, traps_2 — segfault
    - host function / call depth / reentry — 5 segfaults
  These are exploitable on untrusted WASM. jit2 shouldn't be consensus-eligible until fixed.
  3. aarch64 JIT SIMD FP isn't fully softfloat — ARM64 consensus nodes would diverge from x86_64 on SIMD. Same class of bug as
  issue #1 in the FP list but broader.
  4. Multi-module linking missing (96-98 failures) — not a bug, but a feature gap that blocks real module composition. Relevant
  to your COW/fork story.
  5. externref globals crash the JIT backend (global_0) — crash on untrusted input = DoS vector.
  6. Guard-page bounds checks assume OS trap semantics — fine on Linux/macOS, but the guard-page model means any TLB/MMU
  surprise becomes a silent correctness issue instead of a deterministic trap. No in-engine verification of trap codes.
  7. EH v2 (just merged) — try_table/throw/throw_ref added across backends in 7484ad4/2d64140/9576322. No differential fuzzing
  yet; worth confirming exception unwinding is deterministic across all four backends before it's load-bearing.
  8. Stale submodule tracking — .claude/scheduled_tasks.lock recurs untracked; external/psitri has drifted with untracked
  content. Housekeeping.


  PSIO 
    - dynamic verifciation of binary blobs given schemas... 
    - there are N different ways of expressing a schema (FracPack, FlatBuf, CapNProto, others...) but all of them 
    fundamentally point to the same underdlying datatypes... 
    - so given any of these schemas, and a binary, we should be able to determine if the binary conforms. 
       * what I mean is this, I can verfiy whether a binary.fracpack is compliant with the schema in any of the above.. 
       * this means users can pick any schema they want to express their datatypes, and validate any binary format they want
    - this dynamic validation means we need the ability not just to generate these schemas, but to parse them 
    - for frac-pack specifically, we need to parse them and compile their essence into something that allows rapid verification
      of whether or not a .frac buffer is compatible with the schema
          - use case, a database table uses the schema to define rows, gets an inssert request with an untrusted binary blob,
            needs to determine whether:
                 a. exact compat - everything matches
                 b. forward compat - elements in data that are not in schema
                 c. backward compat - schema defines elements not in data
                 d. exacat and canoncal 




   





