#include <psiserve/log.hpp>

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/sinks/ConsoleSink.h>

#include <pthread.h>

namespace psiserve::log
{
   static quill::Logger* s_system_logger = nullptr;

   // Thread-local active logger — set by the scheduler when a process
   // begins executing on this thread, cleared when it blocks/yields.
   static thread_local quill::Logger* t_active_logger = nullptr;

   static std::shared_ptr<quill::Sink> make_console_sink()
   {
      quill::ConsoleSinkConfig cfg;
      cfg.set_stream("stderr");
      return quill::Frontend::create_or_get_sink<quill::ConsoleSink>("stderr", cfg);
   }

   static quill::PatternFormatterOptions log_pattern()
   {
      quill::PatternFormatterOptions p;
      p.format_pattern =
         "%(time) [%(log_level:<8)] [%(logger:<20)] [%(thread_name:<12)] "
         "%(short_source_location:<28) %(message)";
      return p;
   }

   void init()
   {
      if (s_system_logger)
         return;

      s_system_logger = quill::Frontend::create_or_get_logger(
         "system", make_console_sink(), log_pattern());

      quill::BackendOptions backend_opts;
      quill::Backend::start(backend_opts);
   }

   quill::Logger* system_logger()
   {
      return s_system_logger;
   }

   quill::Logger* create_logger(const char* name)
   {
      return quill::Frontend::create_or_get_logger(
         name, make_console_sink(), log_pattern());
   }

   void set_active_logger(quill::Logger* logger)
   {
      t_active_logger = logger;
   }

   void clear_active_logger()
   {
      t_active_logger = nullptr;
   }

   quill::Logger* active_logger()
   {
      return t_active_logger ? t_active_logger : s_system_logger;
   }

   void set_thread_name(const char* name)
   {
#if defined(__APPLE__)
      pthread_setname_np(name);
#else
      pthread_setname_np(pthread_self(), name);
#endif
   }

}  // namespace psiserve::log
