#include <psiber/thread.hpp>
#include <psiber/io_engine_kqueue.hpp>

namespace psiber
{
   thread::thread(const char* /*name*/)
   {
      start(nullptr);
   }

   thread::thread(std::function<void()> entry, const char* /*name*/)
   {
      start(std::move(entry));
   }

   void thread::start(std::function<void()> entry)
   {
      auto io = std::make_unique<KqueueEngine>();
      _sched  = std::make_unique<Scheduler>(std::move(io), s_next_index++);

      // Keep-alive fiber: parks until quit() wakes it.
      // This prevents run() from exiting when user fibers complete.
      _sched->spawnFiber([this]() {
         _keepalive.store(_sched->currentFiber(), std::memory_order_release);
         _sched->parkCurrentFiber();
         // Woken by quit() → exit cleanly, allowing run() to finish
      });

      if (entry)
         _sched->spawnFiber(std::move(entry));

      _os_thread = std::thread([this]() { _sched->run(); });
   }

   thread::~thread()
   {
      quit();
   }

   void thread::quit()
   {
      if (_os_thread.joinable())
      {
         // Spin-wait for keepalive fiber to be set (microseconds at most —
         // it's the first fiber spawned, so it runs immediately on start)
         Fiber* ka;
         while (!(ka = _keepalive.load(std::memory_order_acquire)))
         {
#if defined(__x86_64__)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            asm volatile("yield" ::: "memory");
#endif
         }
         Scheduler::wake(ka);
         _os_thread.join();
      }
   }

   void thread::spawn(std::function<void()> fn)
   {
      _sched->post([this, f = std::move(fn)]() {
         _sched->spawnFiber(std::move(f));
      });
   }

}  // namespace psiber
