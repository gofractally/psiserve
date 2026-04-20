#pragma once

#include <cstdint>
#include <span>

namespace psio
{
   using bytes_view         = std::span<const uint8_t>;
   using mutable_bytes_view = std::span<uint8_t>;
}  // namespace psio
