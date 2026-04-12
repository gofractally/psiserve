# Fiber Scheduling and Cross-Thread Communication

## Overview

psiserve uses cooperative fiber scheduling within each worker thread. Each worker has its
own `Scheduler` running N fibers on one OS thread. Fibers yield on I/O (kqueue/epoll) or
lock contention, never blocking the OS thread.

Cross-thread communication (wake notifications, task dispatch) must be lock-free and
allocation-free on the hot path.

## Lessons from fc (github.com/openhive-network/fc)

The fc library pioneered many of these patterns. Key insights preserved:

### What worked well

- **MPSC linked list with batch drain**: Lock-free stack (CAS push, atomic exchange to
  drain). Receiver gets entire batch in one atomic op, reverses to restore FIFO order.
  Single-word CAS is the only contention point and requires no queue memory on the receiver.

- **Context pooling**: Completed fiber contexts are recycled via `reinitialize()` instead
  of allocating new stacks. Pool is a singly-linked free list — zero overhead.

- **Priority demotion on yield**: Yielding fibers get temporarily demoted to prevent
  starvation of same-priority peers. Original priority restored after context switch.

- **Three-tier lock design**:
  - `spin_lock` — pure atomic spin, nanosecond critical sections
  - `spin_yield_lock` — atomic spin + fiber yield on contention
  - `mutex` — FIFO wait queue, fiber yields to scheduler, woken by `unlock()`

- **Cross-thread dispatch via async task posting**: If `unblock()` targets a different
  thread, it posts an async task to that thread's MPSC queue + condition variable poke.

### What we improve upon

- **No cross-thread malloc/free**: fc allocates tasks with `new` on the posting thread
  and `delete`s on the consuming thread. This cross-thread allocation pattern chokes the
  memory allocator (remote frees hit transfer batches, not thread-local caches) and became
  the bottleneck in an otherwise efficient system.

- **Back pressure**: fc has zero back pressure — unbounded `std::vector` queues grow
  without limit. A fast producer can flood a slow consumer with millions of heap-allocated
  tasks. Memory climbs without bound.

## Cross-Thread Wake Notifications

When a mutex unlock on thread A needs to wake a fiber blocked on thread B:

### Design: Intrusive linked list, fiber IS the node

Each fiber struct has an intrusive `next_wake` pointer. The wake mechanism:

1. Sender (thread A) CAS-pushes the fiber pointer onto receiver's (thread B) atomic wake
   list head — same pattern as fc's `task_in_queue`
2. Sender pokes receiver's `EVFILT_USER` / `eventfd` (idempotent — multiple pokes collapse)
3. Receiver does `exchange(nullptr)` to drain the whole batch in one atomic op
4. Receiver reverses the list to restore FIFO order
5. Receiver moves all woken fibers to its ready queue

**Properties:**
- Zero allocation — the fiber struct IS the linked list node
- Zero contention between wakes to different fibers (independent cache lines)
- Naturally bounded — a fiber can only be woken once (it's either blocked or it isn't)
- No back pressure needed — wake count is bounded by total fiber count
- Multiple senders waking different fibers on the same receiver only contend on the list
  head CAS, which is rare (cross-thread wakes are infrequent)

## Cross-Thread Task Dispatch

For posting work to another thread's scheduler (e.g., distributing connections, spawning
tasks). This is where back pressure and allocation matter.

### Design: Sender-owned ring buffer + intrusive linked list

The sender owns the storage. The receiver borrows it temporarily.

**Sender side:**

Each fiber (or scheduler) has a fixed-size ring buffer of task slots, pre-allocated:

```
struct task_slot {
    task_data  payload;       // the task itself
    task_slot* next;          // intrusive linked list pointer (for receiver's drain list)
    atomic<bool> consumed;    // set by receiver when done
};

struct sender_ring {
    task_slot          slots[N];       // pre-allocated, no malloc
    uint32_t           head;           // next slot to use
    atomic<uint32_t>   free_count;     // decremented by sender, incremented by receiver
};
```

**Send path:**

1. Check `free_count` — if zero, yield (back pressure, per-sender)
2. Scan forward from `head` for a free slot (normal case: slot at head is free)
3. Fill in the task payload
4. CAS-push the slot onto receiver's intrusive linked list head
5. Poke receiver's `EVFILT_USER` / `eventfd`
6. Decrement `free_count`

**Receive path:**

1. `exchange(nullptr)` on list head — drain entire batch, one atomic op
2. Reverse list for FIFO order
3. Process each task
4. Set `consumed = true` on each slot (store-release)
5. Increment sender's `free_count` (fetch_add)

**Sender reclaim:**

When the sender wraps around and finds a stale (unconsumed) slot at its head position,
it skips forward looking for the first free slot. The `free_count` atomic tells it
whether any space exists at all — if zero, yield immediately without scanning.

**Properties:**
- Zero malloc/free — sender pre-allocates all slots
- No cross-thread free — receiver marks consumed, sender reclaims its own memory
- Lock-free — CAS on receiver's list head, atomics for free_count and consumed flags
- Per-sender back pressure — slow receiver only blocks senders actually sending to it
- Normal case is pure ring buffer — advance head, slot is free, fill and push
- Degenerate case (out-of-order consumption) causes a short scan, bounded by ring size

### Ring sizing

Size the ring to the expected concurrency. A fiber rarely has more than a few outstanding
cross-thread tasks, so 32-64 slots per sender is generous. The ring almost never wraps
around in practice.

## Fiber-Aware Mutex Types

### `fiber_mutex` — general purpose

FIFO wait queue, no transaction awareness. On contention:

1. Append fiber to wait queue (protected by a spin-yield lock on the queue head)
2. Yield fiber to scheduler
3. On `unlock()`: pop next waiter, wake it (local `add_to_ready` or cross-thread wake)

### `fiber_tx_mutex` — transaction-aware, wound-wait

Same as `fiber_mutex` but with wound-wait deadlock prevention for database transactions:

- Each transaction has a timestamp (from global `atomic<uint64_t>` counter)
- On contention: compare requester's timestamp with holder's timestamp
- Older (lower timestamp) requester wounds younger holder — sets an abort flag
- Younger requester waits for older holder
- No deadlock possible — circular wait requires a cycle of "younger waits for older",
  which is impossible since timestamps are totally ordered

### `fiber_shared_mutex` — reader-writer

For SAL's `_root_object_mutex` pattern — multiple concurrent readers, exclusive writers.
Brief hold times (root pointer swap). May be a spin-yield lock rather than a full
queue-based mutex, since the critical section is nanoseconds.

## Integration with Scheduler

The scheduler's poll loop drains multiple sources each iteration:

1. **I/O events** — kqueue/epoll results, move I/O-blocked fibers to ready queue
2. **Wake list** — atomic exchange on wake list head, move woken fibers to ready queue
3. **Sleep queue** — check timers, move expired sleepers to ready queue
4. **Shutdown check** — external signal handler flag

All three wake sources (I/O, cross-thread wake, timer) funnel into the same ready queue.
The `EVFILT_USER` / `eventfd` poke ensures the scheduler wakes from its blocking poll
when a cross-thread wake or task arrives.
