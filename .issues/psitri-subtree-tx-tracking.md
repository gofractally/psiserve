---
id: psitri-subtree-tx-tracking
title: psitri transaction does not track per-subtree COW deltas — subtree edits lost on commit
status: ready
priority: high
area: psitri
agent: ~
branch: ~
created: 2026-04-20
depends_on: []
blocks: []
---

## Problem

`psitri::transaction` has one flat `write_cursor _cursor` and one flat
`write_buffer _buffer`. There is no per-subtree delta tracking.

When a caller obtains a subtree write cursor via `transaction::get_subtree_cursor(key)`
and modifies it, those modifications are **completely invisible to the transaction**.
The only way to make them land is to manually call `txn.upsert(key, sub.take_root())`
— and if the caller forgets, or if an abort is needed mid-way, there is no
transactional protection around those subtree edits.

Concretely:

```cpp
auto txn = ses->start_transaction(0);
auto sub = txn.get_subtree_cursor("accounts");
sub.upsert("alice", encode(balance));   // writes into a detached cursor
txn.commit();   // sub was never stored back — alice's balance is silently lost
```

This is especially dangerous when a logical transaction spans multiple subtrees
(e.g. accounts, indices, metadata). Each subtree cursor is detached; the caller
must manually round-trip every one via `take_root` + `upsert`. One forgotten
store-back silently corrupts state.

Also note (from `transaction.hpp:147,154`):
```cpp
assert(_mode == tx_mode::batch && "subtree upsert not supported in micro mode");
```
Micro-mode transactions cannot work with subtrees at all.

## What "sub-transaction" is NOT

A `transaction_frame_ref` / `sub_transaction()` is a savepoint within the flat
key-value namespace of one tree. It is not a subtree modification scope. These
are orthogonal concepts and must not be confused.

## Requirements

1. `transaction::open_subtree(key)` — returns a `write_cursor&` whose lifetime
   is tied to the transaction. Every call with the same key returns the same cursor.
2. On `commit()`: all open subtree cursors are flushed to `_cursor` (via
   `upsert(key, sub.take_root())`), then the normal root-publish happens.
3. On `abort()`: all open subtree cursors are discarded (no store-back).
4. On `push_frame()`: snapshot each open subtree cursor's current root.
5. On `abort_frame()`:
   - Subtrees opened **for the first time** within the frame: remove from
     `_open_subtrees`.
   - Subtrees opened **before** the frame and modified within it: restore
     cursor root to the pre-frame snapshot.
6. On `commit_frame()`: discard snapshots; the parent inherits the cursors as-is.
7. Micro mode: `open_subtree` flushes the flat buffer first (so the subtree
   root is consistent), then opens the subtree cursor. The existing assert on
   `upsert(key, subtree_root)` remains (it is an internal guard); `open_subtree`
   is the correct public API.

## Data structure changes

```cpp
// In transaction::frame add:
std::map<std::string, sal::smart_ptr<sal::alloc_header>> subtree_snapshots;
std::vector<std::string>                                  new_subtrees;

// In transaction add:
std::map<std::string, write_cursor> _open_subtrees;
```

## Acceptance criteria

- [ ] `open_subtree` method exists on `transaction` and `transaction_frame_ref`.
- [ ] Open subtree changes are committed atomically with flat-key changes.
- [ ] Abort discards all open subtree changes (no partial state visible).
- [ ] Frame abort rolls back subtree modifications made within that frame only.
- [ ] Frame commit propagates subtree state to parent frame scope.
- [ ] Micro mode: `open_subtree` works (buffer flushed before subtree opened).
- [ ] All tests in `subtree_tx_tracking_tests.cpp` pass.
- [ ] Existing `subtree_tests.cpp` still passes (no regression).
- [ ] No memory leaks after the new paths (validate with `assert_no_leaks`).
