#pragma once

/// Platform-specific I/O engine selection.
///
/// Forward-declares the concrete engine class for this OS and
/// provides the PlatformEngine alias.  Only forward declarations —
/// no includes, no definitions.  This header is safe to include
/// from detail/fiber.hpp and other headers that need the type
/// name but not the full definition.

namespace psiber::detail
{
#if defined(__APPLE__) || defined(__FreeBSD__)
   class KqueueEngine;
   using PlatformEngine = KqueueEngine;
#elif defined(__linux__)
   class EpollEngine;
   using PlatformEngine = EpollEngine;
#else
#error "No I/O engine for this platform"
#endif

}  // namespace psiber::detail
