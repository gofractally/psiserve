#include <psiber/scheduler.hpp>
#include <psiber/reactor.hpp>
#include <psiber/spin_lock.hpp>

#include <algorithm>
#include <cassert>

namespace psiber
{
   using Fiber          = detail::Fiber;
   using FiberState     = detail::FiberState;
   using TaskSlotHeader = detail::TaskSlotHeader;

   static thread_local std::unique_ptr<Scheduler> t_owned;
   static thread_local Scheduler*                 t_current = nullptr;

   template <typename Engine>
   basic_scheduler<Engine>& basic_scheduler<Engine>::current()
   {
      if (!t_current)
      {
         if (!t_owned)
            t_owned = std::unique_ptr<Scheduler>(new Scheduler());
         t_current = t_owned.get();
      }
      return *static_cast<basic_scheduler*>(t_current);
   }

   template <typename Engine>
   basic_scheduler<Engine>::basic_scheduler(uint32_t index)
      : _index(index),
        _work_pool(std::make_unique<WorkItem[]>(work_pool_size))
   {
   }

   template <typename Engine>
   basic_scheduler<Engine>::~basic_scheduler()
   {
      // Clean up all fibers with non-empty continuations to avoid
      // Boost.Context's forced_unwind, which corrupts heap metadata
      // on macOS ARM64 with fixedsize_stack.
      for (auto& f : _fibers)
      {
         if (!f->cont)
            continue;

         switch (f->state)
         {
            case FiberState::Ready:
            {
               // Fiber was spawned (initial yield completed) but never
               // entered the entry loop.  Destroy the entry callable
               // to prevent execution during cleanup.
               if (f->entry_dtor)
               {
                  f->entry_dtor(f->entry_buf);
                  f->entry_dtor = nullptr;
               }
               f->entry_run = nullptr;

               // Resume: fiber enters the while loop, runEntry() is
               // a no-op, fiber yields as Recyclable.
               f->cont = f->cont.resume();
               // Fall through to handle the now-Recyclable fiber.
            }
            [[fallthrough]];

            case FiberState::Recyclable:
               f->state = FiberState::Done;
               f->cont  = f->cont.resume();
               break;

            case FiberState::Parked:
            case FiberState::Blocked:
            case FiberState::Sleeping:
               // Fiber is stuck mid-execution.  Set _interrupted so it
               // throws shutdown_exception upon resume, then clean up.
               _interrupted = true;
               _current     = f.get();
               f->cont      = f->cont.resume();
               _current     = nullptr;
               if (f->state == FiberState::Recyclable && f->cont)
               {
                  f->state = FiberState::Done;
                  f->cont  = f->cont.resume();
               }
               break;

            default:
               break;
         }
      }
   }

   template <typename Engine>
   void basic_scheduler<Engine>::registerFiber(std::unique_ptr<Fiber> fiber)
   {
      Fiber* fp = fiber.get();
      fp->posted_num = _posted_counter++;
      addToReadyQueue(fp);
      _fibers.push_back(std::move(fiber));
   }

   template <typename Engine>
   void basic_scheduler<Engine>::run()
   {
      auto* prev_current = t_current;
      t_current = this;
      _io.registerUserEvent(_index);

      while (true)
      {
         // ── Always drain cross-thread wakes/tasks/work (cheap: atomic exchange) ──
         drainWakeList();
         drainTaskList();
         drainWorkList();

         // ── Pick next fiber: LIFO slot first, then priority queues ──
         Fiber* fiber = nullptr;

         if (_lifo_slot && _lifo_consecutive < lifo_cap)
         {
            fiber       = _lifo_slot;
            _lifo_slot  = nullptr;
            ++_lifo_consecutive;
         }
         else
         {
            _lifo_consecutive = 0;
            if (_lifo_slot)
            {
               addToReadyQueue(_lifo_slot);
               _lifo_slot = nullptr;
            }
            fiber = popFromReadyQueues();
         }

         if (!fiber)
         {
            // ── Slow path: nothing ready — poll for I/O + timers ──
            pollAndUnblock(/*blocking=*/false);

            // Drain again: wakes/tasks that arrived during the poll
            drainWakeList();
            drainTaskList();
            drainWorkList();

            // Re-check queues after poll+drain
            if (_lifo_slot)
            {
               fiber       = _lifo_slot;
               _lifo_slot  = nullptr;
               _lifo_consecutive = 1;
            }
            else
            {
               fiber = popFromReadyQueues();
            }
         }

         if (fiber)
         {
            _current         = fiber;
            fiber->state     = FiberState::Running;
            fiber->home_sched = this;  // track current scheduler (fiber may have migrated)

            assert(fiber->cont && "resuming fiber with null continuation");
            fiber->cont = fiber->cont.resume();

            _current = nullptr;

            // Strand release: any yield-like state (Parked, Blocked,
            // Sleeping, Recyclable) releases the strand so other queued
            // fibers can make progress.  I/O is cooperative — a fiber
            // blocked on an fd or timer does not own its strand during
            // the wait.  On wake, the fiber routes back through
            // home_strand->enqueue() (pollAndUnblock / wakePoolWaiter /
            // drainWakeList all use this pattern).
            //
            // Guard: only release if this fiber is actually _active of
            // its strand.  A fiber resumed via the shutdown path may
            // have home_strand set but was already released on its
            // earlier Blocked/Sleeping transition.
            if (fiber->home_strand &&
                fiber->home_strand->active() == fiber &&
                (fiber->state == FiberState::Parked ||
                 fiber->state == FiberState::Recyclable ||
                 fiber->state == FiberState::Blocked ||
                 fiber->state == FiberState::Sleeping))
            {
               Fiber* next = fiber->home_strand->release();
               if (next)
                  promoteStrandWaiter(next);
            }

            // If the fiber's entry finished, pool it for reuse
            if (fiber->state == FiberState::Recyclable)
               _free_fibers.push_back(fiber);
         }
         else
         {
            // Check if any fibers are still alive (pooled/daemon fibers don't count)
            bool any_alive = std::any_of(_fibers.begin(), _fibers.end(),
               [](const auto& f) {
                  return f->state != FiberState::Done &&
                         f->state != FiberState::Recyclable &&
                         !f->daemon;
               });

            if (!any_alive)
            {
               // Reactor-bound schedulers stay alive until the reactor
               // stops.  They may receive work via cross-thread post()
               // or the reactor's ready-strand queue even with no
               // current local fibers.  Fall through to the shutdown
               // check / reactor pull / poll path.
               if (_reactor)
               {
                  if (_shutdownCheck && _shutdownCheck())
                  {
                     // Reactor is stopping — terminate daemons and exit.
                     _interrupted = true;
                     bool any_remaining = false;
                     for (auto& f : _fibers)
                     {
                        if (f->state == FiberState::Parked ||
                            f->state == FiberState::Blocked ||
                            f->state == FiberState::Sleeping)
                        {
                           f->state = FiberState::Ready;
                           addToReadyQueue(f.get());
                           any_remaining = true;
                        }
                     }
                     if (any_remaining)
                        continue;
                     break;
                  }
                  // Not stopping — skip shutdown, wait for work below.
               }
               else
               {
                  // Standalone scheduler: no external work source.
                  // Terminate daemon fibers — resume them so they throw
                  // shutdown_exception and cleanly exit their entry loop.
                  // Without this, Boost.Context's forced_unwind during
                  // destruction would crash (sched continuation is stale).
                  _interrupted = true;
                  bool any_remaining = false;
                  for (auto& f : _fibers)
                  {
                     if (f->state == FiberState::Parked ||
                         f->state == FiberState::Blocked ||
                         f->state == FiberState::Sleeping)
                     {
                        f->state = FiberState::Ready;
                        addToReadyQueue(f.get());
                        any_remaining = true;
                     }
                  }
                  if (any_remaining)
                     continue;  // resume daemons so they exit cleanly
                  break;
               }
            }

            if (_shutdownCheck && _shutdownCheck())
               _interrupted = true;

            if (_interrupted)
            {
               bool any_resumed = false;
               for (auto& f : _fibers)
               {
                  if (f->state == FiberState::Blocked ||
                      f->state == FiberState::Sleeping ||
                      f->state == FiberState::Parked)
                  {
                     f->state = FiberState::Ready;
                     addToReadyQueue(f.get());
                     any_resumed = true;
                  }
               }
               if (!any_resumed)
                  break;
               continue;
            }

            // ── Reactor pull: steal work from the shared strand queue ──
            if (_reactor)
            {
               strand* s = _reactor->try_pop_strand();
               if (s)
               {
                  Fiber* f = s->active();
                  if (f)
                  {
                     f->home_sched = this;
                     f->state      = FiberState::Ready;
                     f->posted_num = _posted_counter++;
                     addToReadyQueue(f);
                  }
                  continue;
               }
            }

            // ── Adaptive spin-before-block ──
            // Spin budget grows when work arrives during the spin window
            // (cross-thread traffic is active), shrinks when spin expires
            // with nothing found (thread is idle → go straight to kevent).
            if (_spin_budget > 0)
            {
               bool found = false;
               for (int spin = 0; spin < _spin_budget; ++spin)
               {
                  if (_wake_list.probably_non_empty() ||
                      _task_list.probably_non_empty() ||
                      _work_list.probably_non_empty())
                  {
                     found = true;
                     break;
                  }
#if defined(__x86_64__)
                  __builtin_ia32_pause();
#elif defined(__aarch64__)
                  asm volatile("yield" ::: "memory");
#endif
               }

               if (found)
               {
                  // Work arrived during spin — increase budget (up to max)
                  _spin_budget = std::min(_spin_budget * 2, max_spin);
                  continue;  // loop back to drain + pick
               }

               // Spin expired with no work — halve budget
               _spin_budget /= 2;
            }

            // All living fibers are blocked -- wait for I/O or cross-thread wake.
            pollAndUnblock(/*blocking=*/true);
         }

         // Clean up completed fibers (Recyclable ones stay in _fibers for ownership)
         std::erase_if(_fibers,
            [](const auto& f) { return f->state == FiberState::Done; });
      }

      // ── Clean exit for all recyclable fibers ─────────────────────────
      //
      // Resume each recyclable fiber one last time so the callcc lambda
      // returns normally (instead of relying on forced_unwind during
      // ~Fiber, which triggers sporadic heap corruption on macOS with
      // protected_fixedsize_stack).
      for (auto& f : _fibers)
      {
         if (f->state == FiberState::Recyclable)
         {
            f->state = FiberState::Done;
            assert(f->cont && "cleanup: resuming fiber with null continuation");
            f->cont  = f->cont.resume();
         }
      }

      t_current = prev_current;
   }

   template <typename Engine>
   void basic_scheduler<Engine>::yield(RealFd fd, EventKind events)
   {
      _current->state          = FiberState::Blocked;
      _current->blocked_fd     = fd;
      _current->blocked_events = events;
      _io.addFd(fd, events);

      auto& sched = *_current->sched_cont;
      sched       = sched.resume();

      if (_interrupted)
         throw shutdown_exception{};

      // Note: fd event removal is done by pollAndUnblock on the scheduler
      // that owns the io_engine — the fiber may resume on a different
      // scheduler (through strand re-entry) than the one where the fd
      // was registered.
   }

   template <typename Engine>
   void basic_scheduler<Engine>::sleep(std::chrono::milliseconds duration)
   {
      _current->state     = FiberState::Sleeping;
      _current->wake_time = std::chrono::steady_clock::now() + duration;

      auto& sched = *_current->sched_cont;
      sched       = sched.resume();

      if (_interrupted)
         throw shutdown_exception{};
   }

   template <typename Engine>
   void basic_scheduler<Engine>::parkCurrentFiber()
   {
      assert(_current && "parkCurrentFiber called with no current fiber");
      _current->state = FiberState::Parked;

      auto& sched = *_current->sched_cont;
      sched       = sched.resume();

      if (_interrupted)
         throw shutdown_exception{};
   }

   template <typename Engine>
   void basic_scheduler<Engine>::registerSignal(int signo)
   {
      _io.registerSignal(signo);
   }

   template <typename Engine>
   void basic_scheduler<Engine>::waitForSignal(int signo)
   {
      assert(_current && "waitForSignal called with no current fiber");
      _current->state          = FiberState::Blocked;
      _current->blocked_fd     = RealFd{signo};
      _current->blocked_events = Signal;

      auto& sched = *_current->sched_cont;
      sched       = sched.resume();

      if (_interrupted)
         throw shutdown_exception{};
   }

   template <typename Engine>
   void basic_scheduler<Engine>::yieldCurrentFiber()
   {
      assert(_current && "yieldCurrentFiber called with no current fiber");

      Fiber*  me = _current;
      strand* s  = me->home_strand;

      if (s && s->active() == me)
      {
         // Strand-bound yield is a true suspend point: release the
         // strand so other fibers on S can run, then re-enter at the
         // tail.  If we skipped the release, any arrival during our
         // yield (via cross-thread strand::post) would queue behind
         // us and be stuck until our next real park/block/finish.
         // home_strand goes nullptr during transit so the run loop's
         // release-on-park logic doesn't misfire if we park.
         me->home_strand = nullptr;

         Fiber* next = s->release();
         if (next)
            promoteStrandWaiter(next);

         if (s->enter(me))
         {
            // Nobody else wanted S in the gap — active again.
            me->home_strand = s;
            me->state       = FiberState::Ready;
            me->posted_num  = _posted_counter++;
            addToReadyQueue(me);
         }
         else
         {
            // Queued behind a fiber that entered S during our gap.
            // Park with home_strand==nullptr so the run loop skips
            // the release-on-park path; when release promotes us,
            // we'll re-set home_strand after the resume below.
            me->state = FiberState::Parked;
         }

         auto& sched = *me->sched_cont;
         sched       = sched.resume();

         // Restore home_strand after potential park.  Safe to overwrite
         // even in the no-park case — promoteStrandWaiter doesn't touch it.
         me->home_strand = s;
      }
      else
      {
         // Non-strand fiber (or stale active check): classic ready
         // queue cycle.
         me->state      = FiberState::Ready;
         me->posted_num = _posted_counter++;
         addToReadyQueue(me);

         auto& sched = *me->sched_cont;
         sched       = sched.resume();
      }

      if (_interrupted)
         throw shutdown_exception{};
   }

   // ── Cross-thread notify (only triggers kevent when receiver is blocked) ──

   template <typename Engine>
   void basic_scheduler<Engine>::notifyIfPolling()
   {
      _io.triggerUserEvent(_index);
   }

   template <typename Engine>
   void basic_scheduler<Engine>::wake(Fiber* fiber)
   {
      assert(fiber && fiber->home_sched);
      basic_scheduler* target = fiber->home_sched;

      target->_wake_list.push(fiber);

      // Same-thread wake: the exchange at the top of the run loop will
      // pick this up — no kevent trigger needed.
      // Cross-thread wake: must trigger EVFILT_USER to wake the target
      // from a potential blocking kevent.
      if (target != t_current)
         target->notifyIfPolling();
   }

   template <typename Engine>
   void basic_scheduler<Engine>::postTask(TaskSlotHeader* slot)
   {
      _task_list.push(slot);

      notifyIfPolling();
   }

   // ── Strand waiter handoff helper ─────────────────────────────────────────
   //
   // After strand::release() returns a fiber that has just been promoted
   // to _active, schedule it locally.  Replicates the inline logic in the
   // run loop (strand-release branch) so external callers (strand::sync)
   // can do the same.
   template <typename Engine>
   void basic_scheduler<Engine>::promoteStrandWaiter(Fiber* fiber)
   {
      fiber->home_sched = this;
      fiber->state      = FiberState::Ready;
      fiber->posted_num = _posted_counter++;
      addToReadyQueue(fiber);
   }

   // ── Wake routing helper ──────────────────────────────────────────────────
   //
   // Used by pollAndUnblock (I/O and timer completions) and wakePoolWaiter
   // to put a fiber back into execution after it was released from its
   // strand on the blocking-yield path.  Re-enters the home strand if set,
   // so strand serialization is preserved across I/O.
   template <typename Engine>
   void basic_scheduler<Engine>::resumeBlockedFiber(Fiber* fiber)
   {
      if (fiber->home_strand)
      {
         Fiber* local = fiber->home_strand->enqueue(fiber);
         if (local)
         {
            local->state      = FiberState::Ready;
            local->posted_num = _posted_counter++;
            addToReadyQueue(local);
         }
         else
         {
            // Queued in strand wait list — Parked is the canonical
            // "waiting for strand turn" state (same as strand::post).
            fiber->state = FiberState::Parked;
         }
      }
      else
      {
         fiber->state      = FiberState::Ready;
         fiber->posted_num = _posted_counter++;
         addToReadyQueue(fiber);
      }
   }

   // ── Drain helpers ────────────────────────────────────────────────────────

   template <typename Engine>
   void basic_scheduler<Engine>::drainWakeList()
   {
      Fiber* batch = _wake_list.drain();
      if (!batch)
         return;

      // Cross-thread work arrived — boost spin budget for next idle
      _spin_budget = std::min(std::max(_spin_budget, 64) * 2, max_spin);

      Fiber* reversed = decltype(_wake_list)::reverse(batch);

      // Route a woken fiber: strand fibers re-enter their strand,
      // standalone fibers go directly to the ready queue.
      //
      // Guard: only process fibers that are still Parked.  A fiber
      // can appear in the wake list while no longer Parked if the
      // scheduler's shutdown code already set it to Ready (race
      // between cross-thread wake() and local shutdown scan).
      // Re-enqueueing such a fiber causes a double-resume → crash.
      auto routeWokenFiber = [this](Fiber* f) {
         f->next_wake = nullptr;
         if (f->state != FiberState::Parked)
            return;
         if (f->home_strand)
         {
            Fiber* local = f->home_strand->enqueue(f);
            if (local)
            {
               local->state      = FiberState::Ready;
               local->posted_num = _posted_counter++;
               addToReadyQueue(local);
            }
         }
         else
         {
            f->state      = FiberState::Ready;
            f->posted_num = _posted_counter++;
            addToReadyQueue(f);
         }
      };

      // Single-fiber wake: promote to LIFO slot for request-response
      // cache locality (Tokio pattern).  Only for non-strand fibers.
      if (!reversed->next_wake && !_lifo_slot && !reversed->home_strand)
      {
         reversed->next_wake  = nullptr;
         if (reversed->state != FiberState::Parked)
            return;
         reversed->state      = FiberState::Ready;
         reversed->posted_num = _posted_counter++;
         _lifo_slot           = reversed;
         return;
      }

      // Enqueue all woken fibers
      while (reversed)
      {
         Fiber* next = reversed->next_wake;
         routeWokenFiber(reversed);
         reversed = next;
      }
   }

   template <typename Engine>
   void basic_scheduler<Engine>::drainTaskList()
   {
      TaskSlotHeader* batch = _task_list.drain();
      if (!batch)
         return;

      _spin_budget = std::min(std::max(_spin_budget, 64) * 2, max_spin);

      TaskSlotHeader* reversed = decltype(_task_list)::reverse(batch);

      // Execute each task.
      //
      // Cache heap_owned and consumed before run(): for stack-allocated
      // slots (e.g. thread::call()), run() wakes the caller via a
      // promise.  The caller may return and free the stack frame before
      // we get back here.  By caching these flags first, we avoid
      // touching the slot after run() for non-heap, pre-consumed slots.
      while (reversed)
      {
         TaskSlotHeader* next       = reversed->next;
         bool            is_heap    = reversed->heap_owned;
         bool            pre_consumed = reversed->consumed.load(std::memory_order_relaxed);

         if (reversed->run)
         {
            void* payload = reversed + 1;
            reversed->run(payload);
         }

         // After run(), the slot may no longer be valid for
         // stack-allocated slots (pre_consumed == true).
         // Only touch heap-owned slots (we own them) or
         // SendQueue slots that need their consumed flag set.
         if (is_heap)
         {
            if (reversed->destroy)
            {
               void* payload = reversed + 1;
               reversed->destroy(payload);
            }
            delete[] reinterpret_cast<char*>(reversed);
         }
         else if (!pre_consumed)
         {
            reversed->consumed.store(true, std::memory_order_release);
         }
         reversed = next;
      }
   }

   template <typename Engine>
   void basic_scheduler<Engine>::drainWorkList()
   {
      // Exchange-drain the work list — each item becomes its own fiber.
      // Concurrency self-sizes: if a callable blocks, the scheduler
      // picks up the next ready fiber (which may be another work item).
      // Fiber pool reuse keeps the common case cheap.
      WorkItem* batch = _work_list.drain();
      if (!batch)
         return;

      _spin_budget = std::min(std::max(_spin_budget, 64) * 2, max_spin);

      WorkItem* reversed = decltype(_work_list)::reverse(batch);

      // Spawn a fiber for each work item
      while (reversed)
      {
         WorkItem* item = reversed;
         reversed       = reversed->next;
         item->next     = nullptr;

         spawnFiber([this, item]() {
            try
            {
               item->run(item->payload);
            }
            catch (...)
            {
               // Fire-and-forget: swallow exceptions from callables.
               // invoke() callables catch internally and store in the
               // promise, so exceptions here are unexpected leakage.
            }
            if (item->dtor)
               item->dtor(item->payload);
            item->run  = nullptr;
            item->dtor = nullptr;
            bool was_pool = (item >= &_work_pool[0] &&
                             item < &_work_pool[work_pool_size]);
            return_work_slots(item);
            if (was_pool)
               wakePoolWaiter();
         });
      }
   }

   // ── Pool wait list helpers ──────────────────────────────────────────────

   template <typename Engine>
   void basic_scheduler<Engine>::wakePoolWaiter()
   {
      // Pop the first waiter whose timer hasn't already fired.
      // Fibers that timed out have state != Sleeping (pollAndUnblock
      // already set them to Ready).  Skip and discard those.
      while (_pool_wait_head)
      {
         Fiber* waiter   = _pool_wait_head;
         _pool_wait_head = waiter->next_blocked;
         if (!_pool_wait_head)
            _pool_wait_tail = nullptr;
         waiter->next_blocked = nullptr;

         if (waiter->state == FiberState::Sleeping)
         {
            // Re-enter home strand if applicable — the strand was
            // released when the fiber went Sleeping on the pool wait.
            // resumeBlockedFiber transitions state out of Sleeping,
            // which also signals to pollAndUnblock that the timer
            // should not re-fire this fiber.
            resumeBlockedFiber(waiter);
            return;
         }
         // else: timer already woke this fiber — try next waiter
      }
   }

   template <typename Engine>
   void basic_scheduler<Engine>::removeFromPoolWait(Fiber* fiber)
   {
      if (!_pool_wait_head)
         return;

      if (_pool_wait_head == fiber)
      {
         _pool_wait_head = fiber->next_blocked;
         if (!_pool_wait_head)
            _pool_wait_tail = nullptr;
         fiber->next_blocked = nullptr;
         return;
      }

      Fiber* prev = _pool_wait_head;
      while (prev->next_blocked)
      {
         if (prev->next_blocked == fiber)
         {
            prev->next_blocked = fiber->next_blocked;
            if (_pool_wait_tail == fiber)
               _pool_wait_tail = prev;
            fiber->next_blocked = nullptr;
            return;
         }
         prev = prev->next_blocked;
      }
   }

   template <typename Engine>
   void basic_scheduler<Engine>::pollAndUnblock(bool blocking)
   {
      auto now = std::chrono::steady_clock::now();

      // Wake sleeping fibers whose time has come.
      // Re-enters the home strand if the fiber is strand-bound — the
      // strand was released when the fiber went Sleeping.
      for (auto& fiber : _fibers)
      {
         if (fiber->state == FiberState::Sleeping && fiber->wake_time <= now)
         {
            resumeBlockedFiber(fiber.get());
         }
      }

      // If we just woke some fibers, don't block on poll
      bool any_ready = false;
      for (auto& q : _ready_queues)
         if (!q.empty())
            any_ready = true;
      if (_lifo_slot)
         any_ready = true;
      if (any_ready)
         blocking = false;

      // Compute poll timeout
      std::optional<std::chrono::milliseconds> timeout;
      if (!blocking)
      {
         timeout = std::chrono::milliseconds{0};
      }
      else
      {
         auto nearest = std::chrono::steady_clock::time_point::max();
         for (auto& fiber : _fibers)
         {
            if (fiber->state == FiberState::Sleeping && fiber->wake_time < nearest)
               nearest = fiber->wake_time;
         }

         if (nearest != std::chrono::steady_clock::time_point::max())
         {
            auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(nearest - now);
            timeout    = std::max(delta, std::chrono::milliseconds{1});
         }
         else
         {
            timeout = std::chrono::seconds{1};
         }
      }

      IoEvent events[64];
      int     n = _io.poll(events, timeout);

      for (int i = 0; i < n; ++i)
      {
         if (events[i].events & UserWake)
            continue;  // user event -- processed via drain

         for (auto& fiber : _fibers)
         {
            if (fiber->state == FiberState::Blocked &&
                fiber->blocked_fd == events[i].real_fd &&
                (fiber->blocked_events & events[i].events))
            {
               // Unregister fd events on this scheduler's io_engine
               // before routing the fiber: after I/O release, the
               // fiber may resume on a different scheduler via strand
               // re-entry, and that scheduler's _io isn't the one that
               // owns this fd registration.
               _io.removeFdEvents(fiber->blocked_fd, fiber->blocked_events);
               fiber->blocked_fd     = invalid_real_fd;
               fiber->blocked_events = {};
               // Re-enter home strand (if any) — the strand was
               // released when the fiber went Blocked on the fd.
               resumeBlockedFiber(fiber.get());
               break;
            }
         }
      }
   }

   template <typename Engine>
   Fiber* basic_scheduler<Engine>::popFromReadyQueues()
   {
      for (auto& q : _ready_queues)
      {
         if (!q.empty())
         {
            Fiber* f = q.front();
            q.pop_front();
            return f;
         }
      }
      return nullptr;
   }

   template <typename Engine>
   void basic_scheduler<Engine>::addToReadyQueue(Fiber* fiber)
   {
      uint8_t prio = std::min<uint8_t>(fiber->priority, 2);
      _ready_queues[prio].push_back(fiber);
   }

   // ── Explicit instantiation ────────────────────────────────────────────────

   static_assert(io_engine<detail::PlatformEngine>,
                 "PlatformEngine must satisfy the io_engine concept");
   template class basic_scheduler<detail::PlatformEngine>;

   // ── spin_yield_lock implementation (needs Scheduler::current) ──────────

   void spin_yield_lock::lock() noexcept
   {
      constexpr int spin_limit = 32;

      for (int i = 0; i < spin_limit; ++i)
      {
         if (try_lock())
            return;
#if defined(__x86_64__)
         __builtin_ia32_pause();
#elif defined(__aarch64__)
         asm volatile("yield" ::: "memory");
#endif
      }

      // Spin limit exceeded -- yield fiber if possible, else keep spinning
      while (!try_lock())
      {
         auto& sched = Scheduler::current();
         if (sched.currentFiber())
         {
            sched.yieldCurrentFiber();
         }
         else
         {
#if defined(__x86_64__)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            asm volatile("yield" ::: "memory");
#endif
         }
      }
   }

   // ── wake_fiber (non-template, called from fiber_promise) ──────────────

   void wake_fiber(Fiber* f)
   {
      Scheduler::wake(f);
   }

}  // namespace psiber
