#include <psiber/scheduler.hpp>
#include <psiber/detail/io_engine_kqueue.hpp>
#include <psiber/reactor.hpp>
#include <psiber/spin_lock.hpp>

#include <algorithm>
#include <cassert>

namespace psiber
{
   using Fiber          = detail::Fiber;
   using FiberState     = detail::FiberState;
   using TaskSlotHeader = detail::TaskSlotHeader;

   static thread_local Scheduler* t_current = nullptr;

   template <typename Engine>
   basic_scheduler<Engine>* basic_scheduler<Engine>::current() { return static_cast<basic_scheduler*>(t_current); }

   template <typename Engine>
   basic_scheduler<Engine>::basic_scheduler(uint32_t index)
      : _index(index),
        _work_pool(std::make_unique<WorkItem[]>(work_pool_size))
   {
      // Chain all pool items into the freelist
      for (uint32_t i = 0; i < work_pool_size; ++i)
         _work_pool[i].next = (i + 1 < work_pool_size) ? &_work_pool[i + 1] : nullptr;
      _work_free = &_work_pool[0];
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
      t_current = this;
      _io.registerUserEvent(_index);

      // Spawn the drain fiber — a daemon that executes work items
      // in fiber context so callables can use post(), sleep(), etc.
      spawnFiber([this]() {
         while (true)
         {
            WorkItem* batch = _work_head.exchange(nullptr, std::memory_order_acquire);
            if (batch)
            {
               _spin_budget = std::min(std::max(_spin_budget, 64) * 2, max_spin);

               // Reverse for FIFO order
               WorkItem* reversed = nullptr;
               while (batch)
               {
                  WorkItem* next = batch->next;
                  batch->next    = reversed;
                  reversed       = batch;
                  batch          = next;
               }

               // Execute, destroy, and return each item to the freelist
               while (reversed)
               {
                  WorkItem* next = reversed->next;

                  _drain_executing = true;
                  try
                  {
                     reversed->run(reversed->payload);
                  }
                  catch (...)
                  {
                     // For noexcept post() callables, drain_yield_error
                     // calls std::terminate before reaching here.
                     // For invoke() callables, exceptions are caught by
                     // the run() callback and stored in the promise.
                     // This catch handles any unexpected leakage.
                  }
                  _drain_executing = false;
                  if (reversed->dtor)
                     reversed->dtor(reversed->payload);
                  reversed->run  = nullptr;
                  reversed->dtor = nullptr;

                  // Push back onto freelist (spin-locked)
                  _work_free_lock.lock();
                  reversed->next = _work_free;
                  _work_free     = reversed;
                  _work_free_lock.unlock();

                  reversed = next;
               }

               // Wake fibers waiting for pool space
               _work_space_lock.lock();
               Fiber* waiters = _work_space_wait_head;
               _work_space_wait_head = nullptr;
               _work_space_lock.unlock();

               while (waiters)
               {
                  Fiber* next = waiters->next_wake;
                  waiters->next_wake = nullptr;
                  basic_scheduler::wake(waiters);
                  waiters = next;
               }
            }

            // Check for more work before parking
            if (_work_head.load(std::memory_order_acquire))
               continue;

            parkCurrentFiber();
         }
      }, "drain");
      _drain_fiber = _fibers.back().get();
      _drain_fiber->daemon   = true;
      _drain_fiber->priority = 0;  // high priority — run before normal fibers

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

            fiber->cont = fiber->cont.resume();

            _current = nullptr;

            // Strand release: if a strand fiber parks or finishes,
            // release the strand so the next waiting fiber can run.
            // I/O-blocked and sleeping fibers stay "active" — they'll
            // resume on this thread when the event/timer fires.
            if (fiber->home_strand &&
                (fiber->state == FiberState::Parked ||
                 fiber->state == FiberState::Recyclable))
            {
               Fiber* next = fiber->home_strand->release();
               if (next)
               {
                  next->home_sched = this;
                  next->state      = FiberState::Ready;
                  next->posted_num = _posted_counter++;
                  addToReadyQueue(next);
               }
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
                  if (_wake_head.load(std::memory_order_acquire) ||
                      _task_head.load(std::memory_order_acquire) ||
                      _work_head.load(std::memory_order_acquire))
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
            f->cont  = f->cont.resume();
         }
      }

      t_current = nullptr;
   }

   template <typename Engine>
   void basic_scheduler<Engine>::checkNotDrainFiber() const
   {
      if (_drain_executing) [[unlikely]]
         throw drain_yield_error{};
   }

   template <typename Engine>
   void basic_scheduler<Engine>::yield(RealFd fd, EventKind events)
   {
      checkNotDrainFiber();
      _current->state          = FiberState::Blocked;
      _current->blocked_fd     = fd;
      _current->blocked_events = events;
      _io.addFd(fd, events);

      auto& sched = *_current->sched_cont;
      sched       = sched.resume();

      if (_interrupted)
         throw shutdown_exception{};

      _io.removeFdEvents(fd, events);
   }

   template <typename Engine>
   void basic_scheduler<Engine>::sleep(std::chrono::milliseconds duration)
   {
      checkNotDrainFiber();
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
      checkNotDrainFiber();
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
      checkNotDrainFiber();
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
      checkNotDrainFiber();
      _current->state      = FiberState::Ready;
      _current->posted_num = _posted_counter++;
      addToReadyQueue(_current);

      auto& sched = *_current->sched_cont;
      sched       = sched.resume();

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

      // CAS-push onto the target scheduler's wake list
      Fiber* old_head = target->_wake_head.load(std::memory_order_relaxed);
      do
      {
         fiber->next_wake = old_head;
      } while (!target->_wake_head.compare_exchange_weak(
         old_head, fiber, std::memory_order_release, std::memory_order_relaxed));

      // Same-thread wake: the drain at the top of the run loop will
      // pick this up — no kevent trigger needed.
      // Cross-thread wake: must trigger EVFILT_USER to wake the target
      // from a potential blocking kevent.
      if (target != t_current)
         target->notifyIfPolling();
   }

   template <typename Engine>
   void basic_scheduler<Engine>::postTask(TaskSlotHeader* slot)
   {
      // CAS-push onto the task intake list
      TaskSlotHeader* old_head = _task_head.load(std::memory_order_relaxed);
      do
      {
         slot->next = old_head;
      } while (!_task_head.compare_exchange_weak(
         old_head, slot, std::memory_order_release, std::memory_order_relaxed));

      notifyIfPolling();
   }

   // ── Drain helpers ────────────────────────────────────────────────────────

   template <typename Engine>
   void basic_scheduler<Engine>::drainWakeList()
   {
      Fiber* batch = _wake_head.exchange(nullptr, std::memory_order_acquire);
      if (!batch)
         return;

      // Cross-thread work arrived — boost spin budget for next idle
      _spin_budget = std::min(std::max(_spin_budget, 64) * 2, max_spin);

      // Reverse the list to restore FIFO order (MPSC stack is LIFO)
      Fiber* reversed = nullptr;
      while (batch)
      {
         Fiber* next      = batch->next_wake;
         batch->next_wake = reversed;
         reversed         = batch;
         batch            = next;
      }

      // Route a woken fiber: strand fibers re-enter their strand,
      // standalone fibers go directly to the ready queue.
      auto routeWokenFiber = [this](Fiber* f) {
         f->next_wake = nullptr;
         if (f->home_strand)
         {
            // Re-enter the strand.  If the fiber becomes active and
            // the strand has no reactor, enqueue returns the fiber
            // for us to handle locally.
            Fiber* local = f->home_strand->enqueue(f);
            if (local)
            {
               local->state      = FiberState::Ready;
               local->posted_num = _posted_counter++;
               addToReadyQueue(local);
            }
            // else: strand posted to reactor, or fiber is waiting
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
      TaskSlotHeader* batch = _task_head.exchange(nullptr, std::memory_order_acquire);
      if (!batch)
         return;

      _spin_budget = std::min(std::max(_spin_budget, 64) * 2, max_spin);

      // Reverse for FIFO order
      TaskSlotHeader* reversed = nullptr;
      while (batch)
      {
         TaskSlotHeader* next = batch->next;
         batch->next          = reversed;
         reversed             = batch;
         batch                = next;
      }

      // Execute each task
      while (reversed)
      {
         TaskSlotHeader* next = reversed->next;
         if (reversed->run)
         {
            void* payload = reversed + 1;
            reversed->run(payload);
         }
         if (reversed->heap_owned)
         {
            // Heap-allocated slot: destroy payload and free the block.
            // No consumed signal — nobody is waiting on it.
            if (reversed->destroy)
            {
               void* payload = reversed + 1;
               reversed->destroy(payload);
            }
            delete[] reinterpret_cast<char*>(reversed);
         }
         else
         {
            reversed->consumed.store(true, std::memory_order_release);
         }
         reversed = next;
      }
   }

   template <typename Engine>
   void basic_scheduler<Engine>::drainWorkList()
   {
      // Poke the drain fiber if work is pending and it's parked.
      // The drain fiber does the actual exchange + execute + return cycle
      // in fiber context, so callables have full fiber capabilities.
      if (_work_head.load(std::memory_order_acquire) &&
          _drain_fiber && _drain_fiber->state == FiberState::Parked)
      {
         _drain_fiber->state      = FiberState::Ready;
         _drain_fiber->posted_num = _posted_counter++;
         addToReadyQueue(_drain_fiber);
      }
   }

   template <typename Engine>
   void basic_scheduler<Engine>::pollAndUnblock(bool blocking)
   {
      auto now = std::chrono::steady_clock::now();

      // Wake sleeping fibers whose time has come
      for (auto& fiber : _fibers)
      {
         if (fiber->state == FiberState::Sleeping && fiber->wake_time <= now)
         {
            fiber->state      = FiberState::Ready;
            fiber->posted_num = _posted_counter++;
            addToReadyQueue(fiber.get());
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
               fiber->state          = FiberState::Ready;
               fiber->blocked_fd     = invalid_real_fd;
               fiber->blocked_events = {};
               fiber->posted_num     = _posted_counter++;
               addToReadyQueue(fiber.get());
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
         Scheduler* sched = Scheduler::current();
         if (sched && sched->currentFiber())
         {
            sched->yieldCurrentFiber();
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
