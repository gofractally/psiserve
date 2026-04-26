#pragma once

// Phase C: Schema IR → WIT text emitter.
//
// Walks a `psio1::schema_types::Schema` (populated by SchemaBuilder in
// Phase B) and produces the equivalent `.wit` source text. Lets
// psiserve hand a WIT file to external component-model tooling
// (wit-bindgen, wasmtime, etc.) for any world reflected from C++.
//
// Supported IR coverage (v1):
//
//   Envelope: Package, Use, Interface, World
//   Types:    Int (s/u × 8/16/32/64), Float (f32/f64), bool, char,
//             string (as `string`), List (`list<T>`), Option
//             (`option<T>`), Tuple (`tuple<...>`), named Type refs,
//             Object/Struct (as `record`), Variant (as `variant`),
//             Resource (as `resource`).
//
//   Skipped for v1 (emitted as `/* TODO ... */` so output is still
//   readable): Array (fixed-size), FracPack wrappers, Custom.
//
// Name mangling:
//
//   WIT identifiers are lowercase-kebab-case. The emitter converts
//   C++ names with a simple rule: lowercase the whole thing, swap `_`
//   for `-`, and split adjacent lowercase→uppercase transitions
//   (PascalCase → pascal-case). Preserves already-kebab inputs.
//
// Not handled here: WIT → Schema parsing. That's a separate artifact
// if ever needed.

#include <string>

namespace psio1::schema_types
{
   struct Schema;

   /// Emit the full WIT text representation of `schema`.
   /// Output ends with a trailing newline.
   std::string emit_wit(const Schema& schema);
}  // namespace psio1::schema_types
