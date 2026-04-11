#include <psiserve/log.hpp>
#include <psiserve/scheduler.hpp>

#include <boost/context/protected_fixedsize_stack.hpp>

#include <algorithm>

namespace psiserve
{
   static constexpr std::size_t fiber_native_stack_size = 512 * 1024;  // 512KB per fiber

   Scheduler::Scheduler(std::unique_ptr<IoEngine> io)
      : _io(std::move(io))
   {
   }

   void Scheduler::setProcess(Process* proc)
   {
      _proc = proc;
      if (proc)
         log::set_active_logger(log::create_logger(proc->name));
   }

   void Scheduler::spawnFiber(std::function<void()> entry)
   {
      auto fiber = std::make_unique<Fiber>();
      Fiber* fp  = fiber.get();
      fp->id     = _next_id++;

      PSI_DEBUG("Spawning fiber {}", fp->id);

      // Create the fiber's native stack and continuation.
      // callcc immediately enters the lambda; the fiber yields back right away.
      fp->cont = ctx::callcc(
         std::allocator_arg,
         ctx::protected_fixedsize_stack(fiber_native_stack_size),
         [this, fp, entry = std::move(entry)](ctx::continuation&& sched)
         {
            // Save the scheduler's continuation (lives on this fiber's native stack)
            fp->sched_cont = &sched;

            // Yield immediately — wait for the scheduler to run us
            sched = sched.resume();

            // Scheduler resumed us — run the entry function
            entry();

            // Fiber completed
            fp->state = FiberState::Done;
            PSI_DEBUG("Fiber {} completed", fp->id);
            return std::move(sched);
         }
      );

      // callcc returned: fiber yielded after initial setup
      _ready.push_back(fp);
      _fibers.push_back(std::move(fiber));
   }

   void Scheduler::run()
   {
      PSI_DEBUG("Scheduler starting with {} fibers", _fibers.size());

      while (true)
      {
         // Poll I/O and move blocked fibers to ready queue
         pollAndUnblock(/*blocking=*/false);

         if (!_ready.empty())
         {
            // Pick next ready fiber
            Fiber* fiber = _ready.front();
            _ready.pop_front();

            _current      = fiber;
            fiber->state  = FiberState::Running;

            // Switch to the fiber (suspends the scheduler)
            fiber->cont = fiber->cont.resume();

            _current = nullptr;
            // Fiber yielded or completed — state was set by yield() or the lambda
         }
         else
         {
            // Check if any fibers are still alive
            bool any_alive = std::any_of(_fibers.begin(), _fibers.end(),
               [](const auto& f) {
                  return f->state != FiberState::Done;
               });

            if (!any_alive)
               break;

            // All living fibers are blocked — wait for I/O
            pollAndUnblock(/*blocking=*/true);
         }

         // Clean up completed fibers
         std::erase_if(_fibers,
            [](const auto& f) { return f->state == FiberState::Done; });
      }

      PSI_DEBUG("Scheduler finished — all fibers completed");
   }

   void Scheduler::yield(RealFd fd, EventKind events)
   {
      _current->state          = FiberState::Blocked;
      _current->blocked_fd     = fd;
      _current->blocked_events = events;
      _io->addFd(fd, events);

      // Suspend this fiber, return to the scheduler's run() loop
      auto& sched = *_current->sched_cont;
      sched = sched.resume();

      // Scheduler resumed us — the fd is ready.
      // Only remove the specific events we registered, not all filters.
      // This prevents removing a EVFILT_READ that another fiber depends on
      // when we only registered EVFILT_WRITE (or vice versa).
      _io->removeFdEvents(fd, events);
   }

   void Scheduler::sleep(std::chrono::milliseconds duration)
   {
      _current->state     = FiberState::Sleeping;
      _current->wake_time = std::chrono::steady_clock::now() + duration;

      // Suspend this fiber, return to the scheduler's run() loop
      auto& sched = *_current->sched_cont;
      sched = sched.resume();
   }

   void Scheduler::pollAndUnblock(bool blocking)
   {
      auto now = std::chrono::steady_clock::now();

      // Wake sleeping fibers whose time has come
      for (auto& fiber : _fibers)
      {
         if (fiber->state == FiberState::Sleeping && fiber->wake_time <= now)
         {
            fiber->state = FiberState::Ready;
            _ready.push_back(fiber.get());
         }
      }

      // If we just woke some fibers, don't block on poll
      if (!_ready.empty())
         blocking = false;

      // Compute poll timeout: use nearest sleeping fiber's wake time
      std::optional<std::chrono::milliseconds> timeout;
      if (!blocking)
      {
         timeout = std::chrono::milliseconds{0};
      }
      else
      {
         // Find the nearest sleeping fiber
         auto nearest = std::chrono::steady_clock::time_point::max();
         for (auto& fiber : _fibers)
         {
            if (fiber->state == FiberState::Sleeping && fiber->wake_time < nearest)
               nearest = fiber->wake_time;
         }

         if (nearest != std::chrono::steady_clock::time_point::max())
         {
            auto delta = std::chrono::duration_cast<std::chrono::milliseconds>(nearest - now);
            timeout = std::max(delta, std::chrono::milliseconds{1});
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
         // Find the blocked fiber waiting on this fd
         for (auto& fiber : _fibers)
         {
            if (fiber->state == FiberState::Blocked &&
                fiber->blocked_fd == events[i].real_fd &&
                (fiber->blocked_events & events[i].events))
            {
               fiber->state          = FiberState::Ready;
               fiber->blocked_fd     = invalid_real_fd;
               fiber->blocked_events = {};
               _ready.push_back(fiber.get());
               break;
            }
         }
      }
   }

}  // namespace psiserve
