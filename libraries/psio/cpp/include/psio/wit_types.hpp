#pragma once

// WIT (WebAssembly Interface Types) data structures.
//
// These mirror the Component Model's type system: records, variants, enums,
// flags, lists, options, results, tuples, and function signatures.
//
// Type references use index-based encoding:
//   - type_idx >= 0  → index into wit_world::types[]
//   - type_idx < 0   → primitive: static_cast<wit_prim>(-(type_idx + 1))
//
// Helpers wit_prim_idx() and idx_to_prim() convert between the two.

#include <psio/reflect.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace psio {

   // ---- Primitive types ----

   enum class wit_prim : uint8_t {
      bool_   = 0,
      u8      = 1,
      s8      = 2,
      u16     = 3,
      s16     = 4,
      u32     = 5,
      s32     = 6,
      u64     = 7,
      s64     = 8,
      f32     = 9,
      f64     = 10,
      char_   = 11,
      string_ = 12,
   };

   /// Encode a primitive as a negative type index.
   constexpr int32_t wit_prim_idx(wit_prim p) { return -(static_cast<int32_t>(p) + 1); }

   /// Decode a negative type index back to a primitive.
   constexpr wit_prim idx_to_prim(int32_t idx) { return static_cast<wit_prim>(-(idx + 1)); }

   /// True if the type index refers to a primitive (negative).
   constexpr bool is_prim_idx(int32_t idx) { return idx < 0; }

   /// Sentinel: "no type" for variant cases without payloads
   /// and result types without ok/err.
   constexpr int32_t WIT_NO_TYPE = INT32_MIN;

   // ---- Compound type definitions ----

   enum class wit_type_kind : uint8_t {
      record_  = 0,
      variant_ = 1,
      enum_    = 2,
      flags_   = 3,
      list_    = 4,
      option_  = 5,
      result_  = 6,
      tuple_   = 7,
   };

   /// A named field within a record, variant case, function param/result,
   /// or label within an enum/flags.
   struct wit_named_type {
      std::string name;
      int32_t     type_idx = 0;  // negative = wit_prim, non-negative = index into types[]
   };
   PSIO_REFLECT(wit_named_type, name, type_idx)

   /// A compound type definition (record, variant, enum, flags, list, etc.).
   struct wit_type_def {
      std::string                 name;
      uint8_t                     kind = 0;   // wit_type_kind
      std::vector<wit_named_type> fields;     // record fields, variant cases, enum/flag labels, tuple elements
      int32_t                     element_type_idx = 0;  // list/option element; result ok type
      int32_t                     error_type_idx   = 0;  // result err type (0 = none)
   };
   PSIO_REFLECT(wit_type_def, name, kind, fields, element_type_idx, error_type_idx)

   // ---- Functions ----

   /// A WIT function signature with optional link to a WASM core export.
   struct wit_func {
      std::string                 name;
      std::vector<wit_named_type> params;
      std::vector<wit_named_type> results;
      uint32_t                    core_func_idx = UINT32_MAX;  // WASM export index, or UINT32_MAX if unlinked
   };
   PSIO_REFLECT(wit_func, name, params, results, core_func_idx)

   // ---- Interfaces ----

   /// A named group of types and functions (WIT interface).
   struct wit_interface {
      std::string              name;
      std::vector<uint32_t>    type_idxs;  // indices into world.types[]
      std::vector<uint32_t>    func_idxs;  // indices into world.funcs[]
   };
   PSIO_REFLECT(wit_interface, name, type_idxs, func_idxs)

   // ---- World (top-level container) ----

   /// Complete WIT world definition.
   /// Contains both the raw .wit source text and pre-parsed structures
   /// for efficient runtime query.
   struct wit_world {
      std::string                   package;     // e.g. "test:inventory@1.0.0"
      std::string                   name;        // world name
      std::string                   wit_source;  // raw .wit text for tooling
      std::vector<wit_type_def>     types;       // all type definitions
      std::vector<wit_func>         funcs;       // all function signatures
      std::vector<wit_interface>    exports;
      std::vector<wit_interface>    imports;
   };
   PSIO_REFLECT(wit_world, package, name, wit_source, types, funcs, exports, imports)

   // Backward-compat alias for psizam consumers
   using pzam_wit_world = wit_world;

} // namespace psio
