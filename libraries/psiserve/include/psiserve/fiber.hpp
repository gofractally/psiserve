#pragma once

// Fiber and FiberState now live in psiber — this header re-exports.

#include <psiber/fiber.hpp>

namespace psiserve
{
   using psiber::Fiber;
   using psiber::FiberState;
}  // namespace psiserve
