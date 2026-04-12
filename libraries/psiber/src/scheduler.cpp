#include <psiber/scheduler.hpp>
#include <psiber/spin_lock.hpp>

#include <boost/context/protected_fixedsize_stack.hpp>

#include <algorithm>
#include <cassert>

namespace psiber
{
   namespace
   {
      /// Thrown inside a fiber when the scheduler is shutting down.
      /// Forces the fiber to unwind its stack and exit cleanly.
      struct shutdown_exception {};
   }  // namespace

   static constexpr std::size_t fiber_native_stack_size = 512 * 1024;  // 512KB per fiber

   static thread_local Scheduler* t_current = nullptr;

   Scheduler* Scheduler::current() { return t_current; }

   Scheduler::Scheduler(std::unique_ptr<IoEngine> io, uint32_t index)
      : _index(index), _io(std::move(io))
   {
   }

   void Scheduler::spawnFiber(std::function<void()> entry)
   {
      auto  fiber = std::make_unique<Fiber>();
      Fiber* fp   = fiber.get();
      fp->id         = _next_id++;
      fp->home_sched = this;

      // Create the fiber's native stack and continuation.
      fp->cont = ctx::callcc(
         std::allocator_arg,
         ctx::protected_fixedsize_stack(fiber_native_stack_size),
         [this, fp, entry = std::move(entry)](ctx::continuation&& sched)
         {
            fp->sched_cont = &sched;
            // Yield immediately -- wait for the scheduler to run us
            sched = sched.resume();

            try
            {
               entry();
            }
            catch (const shutdown_exception&)
            {
               // Clean shutdown
            }
            catch (...)
            {
               // Prevent exceptions from escaping the continuation
            }

            fp->state = FiberState::Done;
            return std::move(sched);
         }
      );

      fp->posted_num = _posted_counter++;
      addToReadyQueue(fp);
      _fibers.push_back(std::move(fiber));
   }

   void Scheduler::run()
   {
      t_current = this;
      _io->registerUserEvent(_index);

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
            _current     = fiber;
            fiber->state = FiberState::Running;

            fiber->cont = fiber->cont.resume();

            _current = nullptr;
         }
         else
         {
            // Check if any fibers are still alive
            bool any_alive = std::any_of(_fibers.begin(), _fibers.end(),
               [](const auto& f) { return f->state != FiberState::Done; });

            if (!any_alive)
               break;

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
            // _run_state is already 0 (scheduling), so senders will trigger
            // EVFILT_USER to wake us from the blocking kevent.
            pollAndUnblock(/*blocking=*/true);
         }

         // Clean up completed fibers
         std::erase_if(_fibers,
            [](const auto& f) { return f->state == FiberState::Done; });
      }

      t_current = nullptr;
   }

   void Scheduler::yield(RealFd fd, EventKind events)
   {
      _current->state          = FiberState::Blocked;
      _current->blocked_fd     = fd;
      _current->blocked_events = events;
      _io->addFd(fd, events);

      auto& sched = *_current->sched_cont;
      sched       = sched.resume();

      if (_interrupted)
         throw shutdown_exception{};

      _io->removeFdEvents(fd, events);
   }

   void Scheduler::sleep(std::chrono::milliseconds duration)
   {
      _current->state     = FiberState::Sleeping;
      _current->wake_time = std::chrono::steady_clock::now() + duration;

      auto& sched = *_current->sched_cont;
      sched       = sched.resume();

      if (_interrupted)
         throw shutdown_exception{};
   }

   void Scheduler::parkCurrentFiber()
   {
      assert(_current && "parkCurrentFiber called with no current fiber");
      _current->state = FiberState::Parked;

      auto& sched = *_current->sched_cont;
      sched       = sched.resume();

      if (_interrupted)
         throw shutdown_exception{};
   }

   void Scheduler::registerSignal(int signo)
   {
      _io->registerSignal(signo);
   }

   void Scheduler::waitForSignal(int signo)
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

   void Scheduler::yieldCurrentFiber()
   {
      assert(_current && "yieldCurrentFiber called with no current fiber");
      _current->state      = FiberState::Ready;
      _current->posted_num = _posted_counter++;
      addToReadyQueue(_current);

      auto& sched = *_current->sched_cont;
      sched       = sched.resume();

      if (_interrupted)
         throw shutdown_exception{};
   }

   // ── Cross-thread notify (only triggers kevent when receiver is blocked) ──

   void Scheduler::notifyIfPolling()
   {
      _io->triggerUserEvent(_index);
   }

   void Scheduler::wake(Fiber* fiber)
   {
      assert(fiber && fiber->home_sched);
      Scheduler* target = fiber->home_sched;

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

   void Scheduler::post(std::function<void()> fn)
   {
      auto* item = new WorkItem{nullptr, std::move(fn)};

      // CAS-push onto the work list
      WorkItem* old_head = _work_head.load(std::memory_order_relaxed);
      do
      {
         item->next = old_head;
      } while (!_work_head.compare_exchange_weak(
         old_head, item, std::memory_order_release, std::memory_order_relaxed));

      notifyIfPolling();
   }

   void Scheduler::postTask(TaskSlotHeader* slot)
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

   void Scheduler::drainWakeList()
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

      // Single-fiber wake: promote to LIFO slot for request-response
      // cache locality (Tokio pattern).
      if (!reversed->next_wake && !_lifo_slot)
      {
         reversed->next_wake  = nullptr;
         reversed->state      = FiberState::Ready;
         reversed->posted_num = _posted_counter++;
         _lifo_slot           = reversed;
         return;
      }

      // Enqueue all woken fibers into ready queues
      while (reversed)
      {
         Fiber* next          = reversed->next_wake;
         reversed->next_wake  = nullptr;
         reversed->state      = FiberState::Ready;
         reversed->posted_num = _posted_counter++;
         addToReadyQueue(reversed);
         reversed = next;
      }
   }

   void Scheduler::drainTaskList()
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
         reversed->consumed.store(true, std::memory_order_release);
         reversed = next;
      }
   }

   void Scheduler::drainWorkList()
   {
      WorkItem* batch = _work_head.exchange(nullptr, std::memory_order_acquire);
      if (!batch)
         return;

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

      // Execute and delete each work item
      while (reversed)
      {
         WorkItem* next = reversed->next;
         reversed->fn();
         delete reversed;
         reversed = next;
      }
   }

   void Scheduler::pollAndUnblock(bool blocking)
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
      int     n = _io->poll(events, timeout);

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

   Fiber* Scheduler::popFromReadyQueues()
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

   void Scheduler::addToReadyQueue(Fiber* fiber)
   {
      uint8_t prio = std::min<uint8_t>(fiber->priority, 2);
      _ready_queues[prio].push_back(fiber);
   }

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

}  // namespace psiber
