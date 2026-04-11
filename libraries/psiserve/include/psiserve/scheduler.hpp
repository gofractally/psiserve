#pragma once

#include <psiserve/fiber.hpp>
#include <psiserve/io_engine.hpp>
#include <psiserve/process.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace psiserve
{
   /// Cooperative fiber scheduler (single OS thread).
   ///
   /// Manages a set of fibers within one WASM process.  Each fiber runs
   /// until it makes a blocking host call (accept/read/write), at which
   /// point it yields to the scheduler via Boost.Context continuation
   /// switching.  The scheduler polls the I/O engine, unblocks fibers
   /// whose fds are ready, and resumes the next ready fiber.
   ///
   /// All fiber metadata is host-side — nothing is stored in WASM linear memory.
   class Scheduler
   {
     public:
      explicit Scheduler(std::unique_ptr<IoEngine> io);

      /// Set the process this scheduler manages.
      void setProcess(Process* proc);

      /// Access the I/O engine.
      IoEngine& io() { return *_io; }

      /// Create a new fiber that will execute `entry` on its own native stack.
      /// The fiber is created suspended and added to the ready queue.
      void spawnFiber(std::function<void()> entry);

      /// Main scheduler loop.  Runs until all fibers have completed.
      /// Polls I/O, unblocks ready fibers, and dispatches them.
      void run();

      /// Called by host functions when the current fiber must wait for I/O.
      /// Suspends the current fiber and returns to the scheduler.
      /// When this returns, the fd is ready.
      void yield(RealFd fd, EventKind events);

      /// The currently executing fiber (valid only while a fiber is running).
      Fiber* currentFiber() { return _current; }

     private:
      void pollAndUnblock(bool blocking);

      std::unique_ptr<IoEngine>            _io;
      Process*                             _proc    = nullptr;
      Fiber*                               _current = nullptr;
      std::deque<Fiber*>                   _ready;
      std::vector<std::unique_ptr<Fiber>>  _fibers;
      uint32_t                             _next_id = 0;
   };

}  // namespace psiserve
