#pragma once

// psizam/detail/wit_section_finder.hpp — Step 6 of psizam-runtime-api-maturation.
//
// Walks the WASM custom sections of a raw module looking for a
// `component-wit:NAME` custom section payload. Used by
// `runtime::bind(consumer, provider, name)` to discover the WIT
// interface declaration for the named consumer-side import.
//
// Custom-section format (WASM core spec):
//   section_id    : u8       == 0  (custom section)
//   section_size  : LEB128
//   name_size     : LEB128
//   name_bytes    : name_size bytes
//   payload_bytes : remainder of section
//
// All offsets are byte offsets from the start of the WASM module.
// The walker tolerates malformed sections by stopping the scan at the
// first bounds-check failure rather than throwing — callers fall back
// to "no WIT section found" semantics.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace psizam::detail {

// Read a single LEB128 unsigned 32-bit value, advancing `cursor`.
// Bounds-checked: returns false (and leaves cursor unchanged) if the
// LEB128 would overrun `end`, or if it requires more than 5 bytes.
inline bool read_leb128_u32_bounded(const uint8_t*& cursor,
                                    const uint8_t*  end,
                                    uint32_t&       out) noexcept
{
   const uint8_t* start = cursor;
   uint32_t result = 0;
   uint32_t shift  = 0;
   for (int i = 0; i < 5; ++i) {
      if (cursor >= end) { cursor = start; return false; }
      const uint8_t b = *cursor++;
      result |= uint32_t(b & 0x7Fu) << shift;
      if ((b & 0x80u) == 0) { out = result; return true; }
      shift += 7u;
   }
   cursor = start;
   return false;
}

// Locate the first `component-wit:<iface_name>` custom section in the
// WASM module bytes. On success, fills `out_payload` with the payload
// span (within `module_bytes`) and returns true. On no-match, returns
// false and leaves `out_payload` empty.
inline bool find_component_wit_section(std::span<const uint8_t> module_bytes,
                                       std::string_view         iface_name,
                                       std::span<const uint8_t>& out_payload) noexcept
{
   const std::string section_name_full =
      std::string{"component-wit:"} + std::string{iface_name};

   const uint8_t* p   = module_bytes.data();
   const uint8_t* end = module_bytes.data() + module_bytes.size();

   // Skip 8-byte WASM header: magic ("\0asm") + version (1).
   if (module_bytes.size() < 8) return false;
   p += 8;

   while (p < end) {
      const uint8_t section_id = *p++;
      uint32_t section_size = 0;
      if (!read_leb128_u32_bounded(p, end, section_size)) return false;
      const uint8_t* section_payload = p;
      const uint8_t* section_end     = p + section_size;
      if (section_end > end) return false;
      p = section_end;

      if (section_id != 0) continue;   // not a custom section

      // Custom section: read name then check prefix.
      const uint8_t* cur = section_payload;
      uint32_t name_size = 0;
      if (!read_leb128_u32_bounded(cur, section_end, name_size)) continue;
      if (cur + name_size > section_end) continue;

      std::string_view name{reinterpret_cast<const char*>(cur), name_size};
      cur += name_size;

      if (name == section_name_full) {
         out_payload = std::span<const uint8_t>{
            cur, static_cast<std::size_t>(section_end - cur)};
         return true;
      }
   }
   return false;
}

} // namespace psizam::detail
