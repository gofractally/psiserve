#pragma once

#include <quill/LogMacros.h>
#include <quill/Logger.h>

namespace psiserve::log
{
   /// Start the quill backend thread and create the system logger.
   /// Must be called once before any logging.  Safe to call multiple times
   /// (subsequent calls are no-ops).
   void init();

   /// The system logger — used for infrastructure messages (scheduler,
   /// I/O engine, runtime lifecycle).  Always available after init().
   quill::Logger* system_logger();

   /// Create a named logger for a specific process/instance.
   /// The name appears in log output for routing and filtering.
   /// Returns an existing logger if one with this name already exists.
   quill::Logger* create_logger(const char* name);

   /// Set the thread-local active logger.  Called by the scheduler when
   /// a process begins executing on this thread.
   void set_active_logger(quill::Logger* logger);

   /// Clear the thread-local active logger (reverts to system logger).
   /// Called by the scheduler when a process blocks or yields.
   void clear_active_logger();

   /// Returns the thread-local active logger, or the system logger if
   /// no process is currently executing on this thread.
   quill::Logger* active_logger();

   /// Set the OS and quill thread name for the calling thread.
   /// Shows up in log output via %(thread_name).
   void set_thread_name(const char* name);

}  // namespace psiserve::log

// ── PSI logging macros ───────────────────────────────────────────────────────
//
// Usage:
//   PSI_INFO("listening on port {}", port);
//   PSI_DEBUG("accepted fd={} from {}", vfd, peer_addr);
//
// These grab the thread-local active logger automatically.  The level check
// happens BEFORE argument evaluation — disabled levels cost nothing.

#define PSI_TRACE(fmt, ...) \
   QUILL_LOG_TRACE_L1(psiserve::log::active_logger(), fmt, ##__VA_ARGS__)

#define PSI_DEBUG(fmt, ...) \
   QUILL_LOG_DEBUG(psiserve::log::active_logger(), fmt, ##__VA_ARGS__)

#define PSI_INFO(fmt, ...) \
   QUILL_LOG_INFO(psiserve::log::active_logger(), fmt, ##__VA_ARGS__)

#define PSI_WARN(fmt, ...) \
   QUILL_LOG_WARNING(psiserve::log::active_logger(), fmt, ##__VA_ARGS__)

#define PSI_ERROR(fmt, ...) \
   QUILL_LOG_ERROR(psiserve::log::active_logger(), fmt, ##__VA_ARGS__)

#define PSI_FATAL(fmt, ...) \
   QUILL_LOG_CRITICAL(psiserve::log::active_logger(), fmt, ##__VA_ARGS__)

// ── Explicit-logger variants ─────────────────────────────────────────────────
//
// For the rare case where you need to log to a specific logger rather than
// the thread-local active one (e.g., logging about a process you're not
// currently executing).

#define PSI_TRACE_TO(logger, fmt, ...) \
   QUILL_LOG_TRACE_L1(logger, fmt, ##__VA_ARGS__)

#define PSI_DEBUG_TO(logger, fmt, ...) \
   QUILL_LOG_DEBUG(logger, fmt, ##__VA_ARGS__)

#define PSI_INFO_TO(logger, fmt, ...) \
   QUILL_LOG_INFO(logger, fmt, ##__VA_ARGS__)

#define PSI_WARN_TO(logger, fmt, ...) \
   QUILL_LOG_WARNING(logger, fmt, ##__VA_ARGS__)

#define PSI_ERROR_TO(logger, fmt, ...) \
   QUILL_LOG_ERROR(logger, fmt, ##__VA_ARGS__)

#define PSI_FATAL_TO(logger, fmt, ...) \
   QUILL_LOG_CRITICAL(logger, fmt, ##__VA_ARGS__)
