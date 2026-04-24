#pragma once

// .pzam file format — self-contained compiled WASM module.
//
// Contains both module metadata (types, imports, exports, data segments, etc.)
// and N compiled code sections (per architecture × optimization tier × options).
// Serialized with psio fracpack for zero-copy access via psio::view<pzam_file>.
//
// A .pzam file is fully self-contained — no .wasm file needed at runtime.
// Load path: mmap → validate_frac → view<pzam_file> → pick code section → execute.

#include <psizam/detail/jit_reloc.hpp>
#include <psizam/wit_types.hpp>

#include <psio/fracpack.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace psizam {

   static constexpr uint32_t PZAM_MAGIC   = 0x4d415a50; // "PZAM" in little-endian
   static constexpr uint32_t PZAM_VERSION = 3;

   // ---- Architecture ----

   enum class pzam_arch : uint8_t {
      x86_64  = 1,
      aarch64 = 2,
   };

   // ---- Optimization tier ----

   enum class pzam_opt_tier : uint8_t {
      jit1    = 0,
      jit2    = 1,
      llvm_O1 = 2,
      llvm_O2 = 3,
      llvm_O3 = 4,
   };

   // ---- Per-function entry in compiled code ----

   struct pzam_func_entry {
      uint32_t code_offset  = 0;  // offset within code blob
      uint32_t code_size    = 0;  // size in bytes
      uint32_t stack_size   = 0;  // operand stack slots needed
   };
   PSIO_REFLECT(pzam_func_entry, code_offset, code_size, stack_size)

   /// Relocation entry for address patching at load time.
   struct pzam_relocation {
      uint32_t code_offset  = 0;  // byte offset within code blob
      uint16_t symbol       = 0;  // reloc_symbol cast to uint16_t
      uint8_t  type         = 0;  // reloc_type
      int32_t  addend       = 0;  // relocation addend
   };
   PSIO_REFLECT(pzam_relocation, code_offset, symbol, type, addend)

   // ===========================================================================
   // Module metadata — everything needed to instantiate without a .wasm file
   // ===========================================================================

   struct pzam_func_type {
      std::vector<uint8_t> param_types;
      std::vector<uint8_t> return_types;
   };
   PSIO_REFLECT(pzam_func_type, param_types, return_types)

   struct pzam_resizable_limits {
      uint8_t  has_maximum = 0;
      uint32_t initial     = 0;
      uint32_t maximum     = 0;
   };
   PSIO_REFLECT(pzam_resizable_limits, has_maximum, initial, maximum)

   struct pzam_table_type {
      uint8_t               element_type = 0x70; // funcref
      pzam_resizable_limits limits;
   };
   PSIO_REFLECT(pzam_table_type, element_type, limits)

   struct pzam_memory_type {
      pzam_resizable_limits limits;
   };
   PSIO_REFLECT(pzam_memory_type, limits)

   struct pzam_global_type {
      uint8_t content_type = 0;
      uint8_t mutability   = 0;
   };
   PSIO_REFLECT(pzam_global_type, content_type, mutability)

   struct pzam_global_variable {
      pzam_global_type      type;
      std::vector<uint8_t>  init_expr;  // raw WASM const expression bytes
   };
   PSIO_REFLECT(pzam_global_variable, type, init_expr)

   struct pzam_import_entry {
      std::string  module_name;
      std::string  field_name;
      uint8_t      kind           = 0;  // external_kind
      uint32_t     func_type_idx  = 0;  // for Function imports
      pzam_table_type  table_type;      // for Table imports
      pzam_memory_type memory_type;     // for Memory imports
      pzam_global_type global_type;     // for Global imports
   };
   PSIO_REFLECT(pzam_import_entry, module_name, field_name, kind,
                func_type_idx, table_type, memory_type, global_type)

   struct pzam_export_entry {
      std::string  field_name;
      uint8_t      kind  = 0;
      uint32_t     index = 0;
   };
   PSIO_REFLECT(pzam_export_entry, field_name, kind, index)

   struct pzam_elem_entry {
      uint8_t  type  = 0;   // funcref/externref
      uint32_t index = 0;   // function index (or UINT32_MAX for ref.null)
   };
   PSIO_REFLECT(pzam_elem_entry, type, index)

   struct pzam_elem_segment {
      uint32_t                      table_index = 0;
      std::vector<uint8_t>          offset_expr;     // raw init_expr bytes (active only)
      uint8_t                       mode = 0;        // elem_mode (0=active, 1=passive, 2=declarative)
      uint8_t                       elem_type = 0x70;
      std::vector<pzam_elem_entry>  elems;
   };
   PSIO_REFLECT(pzam_elem_segment, table_index, offset_expr, mode, elem_type, elems)

   struct pzam_data_segment {
      uint32_t             memory_index = 0;
      std::vector<uint8_t> offset_expr;  // raw init_expr bytes (active only)
      uint8_t              passive = 0;
      std::vector<uint8_t> data;
   };
   PSIO_REFLECT(pzam_data_segment, memory_index, offset_expr, passive, data)

   struct pzam_tag_type {
      uint8_t  attribute  = 0;
      uint32_t type_index = 0;
   };
   PSIO_REFLECT(pzam_tag_type, attribute, type_index)

   /// WASM features required by this module.
   struct pzam_wasm_features {
      uint8_t simd                  = 0;
      uint8_t bulk_memory           = 0;
      uint8_t sign_ext              = 0;
      uint8_t nontrapping_fptoint   = 0;
      uint8_t tail_call             = 0;
      uint8_t multi_value           = 0;
      uint8_t reference_types       = 0;
      uint8_t extended_const        = 0;
      uint8_t threads               = 0;
      uint8_t multi_memory          = 0;
   };
   PSIO_REFLECT(pzam_wasm_features, simd, bulk_memory, sign_ext,
                nontrapping_fptoint, tail_call, multi_value,
                reference_types, extended_const, threads, multi_memory)

   /// Complete module metadata — sufficient to instantiate without .wasm.
   struct pzam_module_metadata {
      std::vector<pzam_func_type>       types;
      std::vector<pzam_import_entry>    imports;
      std::vector<uint32_t>             functions;   // type index per local function
      std::vector<pzam_table_type>      tables;
      std::vector<pzam_memory_type>     memories;
      std::vector<pzam_global_variable> globals;
      std::vector<pzam_export_entry>    exports;
      std::vector<pzam_elem_segment>    elements;
      std::vector<pzam_data_segment>    data;
      std::vector<pzam_tag_type>        tags;
      uint32_t                          start_function = UINT32_MAX;
      pzam_wasm_features                features_required;
      // Derived counts (for fast access without scanning imports)
      uint32_t num_imported_functions = 0;
      uint32_t num_imported_tables    = 0;
      uint32_t num_imported_memories  = 0;
      uint32_t num_imported_globals   = 0;
      // WIT (Component Model interface types) — optional, fracpack-compatible
      std::optional<pzam_wit_world>   wit;
   };
   PSIO_REFLECT(pzam_module_metadata,
      types, imports, functions, tables, memories, globals,
      exports, elements, data, tags, start_function, features_required,
      num_imported_functions, num_imported_tables,
      num_imported_memories, num_imported_globals, wit)

   // ===========================================================================
   // Code sections — N per file (arch × tier × instrumentation)
   // ===========================================================================

   /// Instrumentation flags baked into compiled code.
   struct pzam_instrumentation {
      uint8_t softfloat       = 0;
      uint8_t gas_metering    = 0;
      uint8_t yield_points    = 0;
      uint8_t debug_info      = 0;
      uint8_t async_backtrace = 0;
   };
   PSIO_REFLECT(pzam_instrumentation, softfloat, gas_metering,
                yield_points, debug_info, async_backtrace)

   /// Identity of the compiler that produced a code section.
   struct pzam_compiler_info {
      std::string              compiler_name;    // e.g. "psizam-jit2", "psizam-llvm"
      std::string              compiler_version; // semver or git hash
      std::array<uint8_t, 32>  compiler_hash = {};  // hash of compiler binary
   };
   PSIO_REFLECT(pzam_compiler_info, compiler_name, compiler_version, compiler_hash)

   /// Attestation: someone who vouches for a code section's correctness.
   struct pzam_attestation {
      std::array<uint8_t, 32> pubkey_hash = {};  // hash of attester's public key
      std::vector<uint8_t>    signature;          // ECC signature over code section content hash
   };
   PSIO_REFLECT(pzam_attestation, pubkey_hash, signature)

   /// One compiled code variant.
   /// All backends produce C ABI entry points — no legacy dispatch modes.
   struct pzam_code_section {
      uint8_t                        arch = 0;              // pzam_arch
      uint8_t                        opt_tier = 0;          // pzam_opt_tier
      pzam_instrumentation           instrumentation;
      uint8_t                        stack_limit_mode = 0;  // 0=frames, 1=bytes
      uint32_t                       page_size = 4096;
      uint32_t                       max_stack = 0;

      pzam_compiler_info             compiler;
      std::vector<pzam_attestation>  attestations;

      std::vector<pzam_func_entry>   functions;
      std::vector<pzam_relocation>   relocations;
      std::vector<uint8_t>           code_blob;
   };
   PSIO_REFLECT(pzam_code_section,
      arch, opt_tier, instrumentation, stack_limit_mode, page_size, max_stack,
      compiler, attestations, functions, relocations, code_blob)

   // ===========================================================================
   // Top-level .pzam file
   // ===========================================================================

   struct pzam_file {
      uint32_t                        magic          = PZAM_MAGIC;
      uint32_t                        format_version = PZAM_VERSION;
      std::array<uint8_t, 32>         input_hash     = {};  // hash of original .wasm
      pzam_module_metadata            metadata;
      std::vector<pzam_code_section>  code_sections;
   };
   PSIO_REFLECT(pzam_file, magic, format_version, input_hash,
                metadata, code_sections)

   // ===========================================================================
   // Serialization helpers
   // ===========================================================================

   /// Serialize a pzam_file to fracpack bytes.
   inline std::vector<char> pzam_save(const pzam_file& file) {
      return psio::convert_to_frac(file);
   }

   /// Deserialize a pzam_file from fracpack bytes.
   inline pzam_file pzam_load(std::span<const char> data) {
      return psio::from_frac<pzam_file>(data);
   }

   /// Validate a .pzam buffer without deserializing.
   inline bool pzam_validate(std::span<const char> data) {
      return psio::validate_frac_compatible<pzam_file>(data);
   }

} // namespace psizam
