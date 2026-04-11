#pragma once

// .pzam file format — cached compiled WASM native code, serialized with fracpack.
//
// Uses psio fracpack for extensibility and zero-copy access:
//   - New fields can be added without breaking old readers
//   - psio::view<pzam_file> accesses code_blob directly from mmap'd data
//   - psio::fracpack_validate<pzam_file>(data) catches corruption
//
// Cache invalidation: input_hash (FNV of WASM bytes) + compiler_hash
// (compile-time constant derived from compiler version + options) must match.

#include <psizam/jit_reloc.hpp>

#include <psio/fracpack.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace psizam {

   static constexpr uint32_t PZAM_MAGIC   = 0x4d415a50; // "PZAM" in little-endian
   static constexpr uint32_t PZAM_VERSION = 2;           // v2: fracpack format

   /// Target architecture identifier.
   enum class pzam_arch : uint8_t {
      x86_64  = 1,
      aarch64 = 2,
   };

   /// Per-function entry in the compiled module.
   struct pzam_func_entry {
      uint32_t code_offset  = 0;  // offset within code blob
      uint32_t code_size    = 0;  // size in bytes
      uint32_t stack_size   = 0;  // operand stack slots needed
   };
   PSIO_REFLECT(pzam_func_entry, definitionWillNotChange(), code_offset, code_size, stack_size)

   /// Relocation entry for address patching at load time.
   struct pzam_relocation {
      uint32_t code_offset  = 0;  // byte offset within code blob
      uint16_t symbol       = 0;  // reloc_symbol cast to uint16_t
      uint8_t  type         = 0;  // reloc_type (0 = abs64 for backward compat)
      int32_t  addend       = 0;  // relocation addend
   };
   PSIO_REFLECT(pzam_relocation, code_offset, symbol, type, addend)

   /// Compile options stored in the cache for validation.
   struct pzam_compile_opts {
      uint8_t  softfloat            = 0;
      uint8_t  async_backtrace      = 0;
      uint8_t  stack_limit_is_bytes = 0;
   };
   PSIO_REFLECT(pzam_compile_opts, definitionWillNotChange(), softfloat, async_backtrace, stack_limit_is_bytes)

   /// Top-level .pzam file structure.
   struct pzam_file {
      uint32_t                       magic           = PZAM_MAGIC;
      uint32_t                       format_version  = PZAM_VERSION;
      uint8_t                        arch            = 0;  // pzam_arch
      pzam_compile_opts              opts            = {};
      uint32_t                       max_stack       = 0;
      std::array<uint8_t, 32>        input_hash      = {};
      std::array<uint8_t, 32>        compiler_hash   = {};
      std::vector<pzam_func_entry>   functions;
      std::vector<pzam_relocation>   relocations;
      std::vector<uint8_t>           code_blob;       // PIC native machine code
   };
   PSIO_REFLECT(pzam_file, magic, format_version, arch, opts, max_stack,
                input_hash, compiler_hash, functions, relocations, code_blob)

   /// Serialize a pzam_file to fracpack bytes.
   inline std::vector<char> pzam_save(const pzam_file& file) {
      return psio::convert_to_frac(file);
   }

   /// Deserialize a pzam_file from fracpack bytes.
   /// Throws on validation failure.
   inline pzam_file pzam_load(std::span<const char> data) {
      return psio::from_frac<pzam_file>(data);
   }

   /// Validate a .pzam buffer without deserializing.
   inline bool pzam_validate(std::span<const char> data) {
      return psio::fracpack_validate_compatible<pzam_file>(data);
   }

} // namespace psizam
