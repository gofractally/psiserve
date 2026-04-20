#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace pcrypt
{

using sha256_digest = std::array<uint8_t, 32>;

sha256_digest sha256(const void* data, size_t len);

inline sha256_digest sha256(std::span<const uint8_t> data)
{
   return sha256(data.data(), data.size());
}

inline sha256_digest sha256(std::span<const char> data)
{
   return sha256(data.data(), data.size());
}

}  // namespace pcrypt
