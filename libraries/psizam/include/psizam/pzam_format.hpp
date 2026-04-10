#pragma once

// .pzam file format — cached compiled WASM native code.
//
// Layout:
//   pzam_header                         (fixed size)
//   pzam_func_entry[num_functions]      (function offset/size table)
//   code_relocation[num_relocations]    (absolute address relocation table)
//   uint8_t code_blob[code_size]        (PIC-ready native code)
//
// The code blob contains machine code with absolute addresses at relocation
// sites. On load, apply_relocations() patches these with current-process
// function addresses. The code is then mmapped with PROT_EXEC.
//
// Cache invalidation: input_hash (SHA-256 of WASM bytes) + compiler_hash
// (compile-time constant derived from compiler version + options) must match.

#include <psizam/jit_reloc.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace psizam {

   static constexpr uint32_t PZAM_MAGIC   = 0x4d415a50; // "PZAM" in little-endian
   static constexpr uint32_t PZAM_VERSION = 1;

   /// Target architecture identifier.
   enum class pzam_arch : uint8_t {
      x86_64  = 1,
      aarch64 = 2,
   };

   /// Compile options stored in the cache header for validation.
   struct pzam_compile_opts {
      uint8_t  softfloat       = 0; // 1 if softfloat enabled
      uint8_t  async_backtrace = 0; // 1 if backtrace enabled
      uint8_t  stack_limit_is_bytes = 0;
      uint8_t  reserved        = 0;
   };

   /// .pzam file header.
   struct pzam_header {
      uint32_t         magic            = PZAM_MAGIC;
      uint32_t         format_version   = PZAM_VERSION;
#if defined(__x86_64__)
      pzam_arch        arch             = pzam_arch::x86_64;
#elif defined(__aarch64__)
      pzam_arch        arch             = pzam_arch::aarch64;
#else
      pzam_arch        arch             = {}; // no JIT
#endif
      uint8_t          reserved[3]      = {};
      pzam_compile_opts opts            = {};
      uint32_t         num_functions    = 0;
      uint32_t         num_relocations  = 0;
      uint32_t         code_size        = 0;
      uint32_t         max_stack        = 0; // module maximum_stack
      std::array<uint8_t, 32> input_hash  = {}; // SHA-256 of WASM input
      std::array<uint8_t, 32> compiler_hash = {}; // SHA-256 of compiler identity
   };
   static_assert(sizeof(pzam_header) == 96, "pzam_header must be 96 bytes");

   /// Per-function entry in the .pzam function table.
   struct pzam_func_entry {
      uint32_t code_offset;  // offset within code blob
      uint32_t code_size;    // size in bytes
      uint32_t stack_size;   // operand stack slots needed
      uint32_t reserved = 0;
   };
   static_assert(sizeof(pzam_func_entry) == 16, "pzam_func_entry must be 16 bytes");

   /// Serialize compiled module to .pzam format.
   /// Returns the serialized bytes.
   inline std::vector<uint8_t> pzam_save(
         const pzam_header& hdr,
         std::span<const pzam_func_entry> funcs,
         std::span<const code_relocation> relocs,
         std::span<const uint8_t> code_blob) {

      size_t total = sizeof(pzam_header)
                   + funcs.size() * sizeof(pzam_func_entry)
                   + relocs.size() * sizeof(code_relocation)
                   + code_blob.size();

      std::vector<uint8_t> out(total);
      uint8_t* p = out.data();

      std::memcpy(p, &hdr, sizeof(hdr));
      p += sizeof(hdr);

      std::memcpy(p, funcs.data(), funcs.size() * sizeof(pzam_func_entry));
      p += funcs.size() * sizeof(pzam_func_entry);

      std::memcpy(p, relocs.data(), relocs.size() * sizeof(code_relocation));
      p += relocs.size() * sizeof(code_relocation);

      std::memcpy(p, code_blob.data(), code_blob.size());

      return out;
   }

   /// Validate and parse a .pzam file header.
   /// Returns true if the header is valid and compatible.
   inline bool pzam_validate_header(const pzam_header& hdr,
                                     const std::array<uint8_t, 32>& expected_input_hash,
                                     const std::array<uint8_t, 32>& expected_compiler_hash) {
      if (hdr.magic != PZAM_MAGIC) return false;
      if (hdr.format_version != PZAM_VERSION) return false;

#if defined(__x86_64__)
      if (hdr.arch != pzam_arch::x86_64) return false;
#elif defined(__aarch64__)
      if (hdr.arch != pzam_arch::aarch64) return false;
#else
      return false; // no JIT on this platform
#endif

      if (hdr.input_hash != expected_input_hash) return false;
      if (hdr.compiler_hash != expected_compiler_hash) return false;

      return true;
   }

   /// Parse a .pzam byte buffer into its components.
   /// Returns false if the buffer is too small or malformed.
   struct pzam_parsed {
      const pzam_header*       header;
      std::span<const pzam_func_entry>  funcs;
      std::span<const code_relocation>  relocs;
      std::span<const uint8_t>          code_blob;
   };

   inline bool pzam_parse(std::span<const uint8_t> data, pzam_parsed& out) {
      if (data.size() < sizeof(pzam_header)) return false;

      out.header = reinterpret_cast<const pzam_header*>(data.data());
      const uint8_t* p = data.data() + sizeof(pzam_header);
      size_t remaining = data.size() - sizeof(pzam_header);

      // Function table
      size_t funcs_bytes = out.header->num_functions * sizeof(pzam_func_entry);
      if (remaining < funcs_bytes) return false;
      out.funcs = { reinterpret_cast<const pzam_func_entry*>(p), out.header->num_functions };
      p += funcs_bytes;
      remaining -= funcs_bytes;

      // Relocation table
      size_t relocs_bytes = out.header->num_relocations * sizeof(code_relocation);
      if (remaining < relocs_bytes) return false;
      out.relocs = { reinterpret_cast<const code_relocation*>(p), out.header->num_relocations };
      p += relocs_bytes;
      remaining -= relocs_bytes;

      // Code blob
      if (remaining < out.header->code_size) return false;
      out.code_blob = { p, out.header->code_size };

      return true;
   }

} // namespace psizam
