#pragma once

// Encode WIT (WebAssembly Interface Types) to Component Model binary format.
//
// Takes a wit_world (produced by generate_wit<T>()) and serializes it to the
// standard component-type custom section binary encoding. This is the same
// format that wasm-tools and wit-component produce.
//
// Usage:
//   auto world = psio::generate_wit<MyExports>("test:pkg@1.0.0");
//   auto binary = psio::encode_wit_binary(world);
//   // binary is a std::vector<uint8_t> ready for a component-type custom section
//
// Or directly from types:
//   auto binary = psio::generate_wit_binary<MyExports>("test:pkg@1.0.0");

#include <psio/wit_gen.hpp>
#include <psio/wit_types.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace psio {

   namespace detail {

      // Component Model binary opcodes
      namespace cm {
         // Section IDs
         constexpr uint8_t section_custom       = 0x00;
         constexpr uint8_t section_type          = 0x07;
         constexpr uint8_t section_export        = 0x0b;

         // Container type constructors
         constexpr uint8_t type_component        = 0x41;
         constexpr uint8_t type_instance          = 0x42;
         constexpr uint8_t type_func              = 0x40;

         // Defined value type constructors (negative SLEB128)
         constexpr uint8_t def_record             = 0x72;
         constexpr uint8_t def_variant             = 0x71;
         constexpr uint8_t def_list                = 0x70;
         constexpr uint8_t def_tuple               = 0x6f;
         constexpr uint8_t def_flags               = 0x6e;
         constexpr uint8_t def_enum_               = 0x6d;
         constexpr uint8_t def_option              = 0x6b;  // wasm-tools uses 0x6b
         constexpr uint8_t def_result              = 0x6a;

         // Primitive value types (negative SLEB128)
         constexpr uint8_t prim_bool               = 0x7f;
         constexpr uint8_t prim_s8                 = 0x7e;
         constexpr uint8_t prim_u8                 = 0x7d;
         constexpr uint8_t prim_s16                = 0x7c;
         constexpr uint8_t prim_u16                = 0x7b;
         constexpr uint8_t prim_s32                = 0x7a;
         constexpr uint8_t prim_u32                = 0x79;
         constexpr uint8_t prim_s64                = 0x78;
         constexpr uint8_t prim_u64                = 0x77;
         constexpr uint8_t prim_f32                = 0x76;
         constexpr uint8_t prim_f64                = 0x75;
         constexpr uint8_t prim_char               = 0x74;
         constexpr uint8_t prim_string             = 0x73;

         // Instance/component item tags
         constexpr uint8_t item_type_def           = 0x01;
         constexpr uint8_t item_import             = 0x03;
         constexpr uint8_t item_export             = 0x04;

         // Export sort
         constexpr uint8_t sort_func               = 0x01;
         constexpr uint8_t sort_type               = 0x03;
         constexpr uint8_t sort_component          = 0x04;
         constexpr uint8_t sort_instance            = 0x05;

         // Extern name discriminant
         constexpr uint8_t name_kebab              = 0x00;

         // Type bound
         constexpr uint8_t bound_eq                = 0x00;

         // Function result
         constexpr uint8_t result_single           = 0x00;
         constexpr uint8_t result_named            = 0x01;

         // Option encoding (used in variant cases and result types)
         constexpr uint8_t option_none             = 0x00;
         constexpr uint8_t option_some             = 0x01;
      } // namespace cm

      class wit_binary_writer {
         std::vector<uint8_t> buf_;

      public:
         void emit_byte(uint8_t b) { buf_.push_back(b); }

         void emit_uleb128(uint32_t val) {
            do {
               uint8_t b = val & 0x7f;
               val >>= 7;
               if (val != 0) b |= 0x80;
               buf_.push_back(b);
            } while (val != 0);
         }

         void emit_string(std::string_view s) {
            emit_uleb128(static_cast<uint32_t>(s.size()));
            buf_.insert(buf_.end(), s.begin(), s.end());
         }

         void emit_bytes(const std::vector<uint8_t>& data) {
            buf_.insert(buf_.end(), data.begin(), data.end());
         }

         size_t size() const { return buf_.size(); }
         std::vector<uint8_t> take() { return std::move(buf_); }
         const std::vector<uint8_t>& data() const { return buf_; }
      };

      // Map from wit_prim to Component Model binary opcode
      inline uint8_t prim_to_cm_byte(wit_prim p) {
         switch (p) {
            case wit_prim::bool_:   return cm::prim_bool;
            case wit_prim::u8:      return cm::prim_u8;
            case wit_prim::s8:      return cm::prim_s8;
            case wit_prim::u16:     return cm::prim_u16;
            case wit_prim::s16:     return cm::prim_s16;
            case wit_prim::u32:     return cm::prim_u32;
            case wit_prim::s32:     return cm::prim_s32;
            case wit_prim::u64:     return cm::prim_u64;
            case wit_prim::s64:     return cm::prim_s64;
            case wit_prim::f32:     return cm::prim_f32;
            case wit_prim::f64:     return cm::prim_f64;
            case wit_prim::char_:   return cm::prim_char;
            case wit_prim::string_: return cm::prim_string;
         }
         return cm::prim_u32; // fallback
      }

      // Encode a wit_world into Component Model binary for embedding
      // as a component-type custom section.
      //
      // The encoding builds the instance type for each interface, wraps it
      // in nested component types, and produces a complete Component binary.
      class wit_cm_encoder {
         const wit_world& world_;

         // Map from world.types[] index → instance type index (after flattening)
         struct instance_encoding {
            std::vector<uint8_t> bytes;
            uint32_t item_count = 0;
         };

         // Emit a valtype reference: primitive byte or type index
         void emit_valtype(wit_binary_writer& w, int32_t type_idx,
                           const std::unordered_map<int32_t, uint32_t>& remap) const {
            if (is_prim_idx(type_idx)) {
               w.emit_byte(prim_to_cm_byte(idx_to_prim(type_idx)));
            } else {
               auto it = remap.find(type_idx);
               uint32_t idx = (it != remap.end()) ? it->second : static_cast<uint32_t>(type_idx);
               w.emit_uleb128(idx);
            }
         }

         // Emit a defined type (record, list, option, etc.)
         void emit_defined_type(wit_binary_writer& w, const wit_type_def& td,
                                const std::unordered_map<int32_t, uint32_t>& remap) const {
            auto kind = static_cast<wit_type_kind>(td.kind);
            switch (kind) {
               case wit_type_kind::record_:
                  w.emit_byte(cm::def_record);
                  w.emit_uleb128(static_cast<uint32_t>(td.fields.size()));
                  for (auto& f : td.fields) {
                     w.emit_string(f.name);
                     emit_valtype(w, f.type_idx, remap);
                  }
                  break;
               case wit_type_kind::list_:
                  w.emit_byte(cm::def_list);
                  emit_valtype(w, td.element_type_idx, remap);
                  break;
               case wit_type_kind::option_:
                  w.emit_byte(cm::def_option);
                  emit_valtype(w, td.element_type_idx, remap);
                  break;
               case wit_type_kind::result_:
                  w.emit_byte(cm::def_result);
                  // ok type: option(valtype)
                  if (td.element_type_idx == WIT_NO_TYPE) {
                     w.emit_byte(cm::option_none);
                  } else {
                     w.emit_byte(cm::option_some);
                     emit_valtype(w, td.element_type_idx, remap);
                  }
                  // err type: option(valtype)
                  if (td.error_type_idx == WIT_NO_TYPE) {
                     w.emit_byte(cm::option_none);
                  } else {
                     w.emit_byte(cm::option_some);
                     emit_valtype(w, td.error_type_idx, remap);
                  }
                  break;
               case wit_type_kind::variant_:
                  w.emit_byte(cm::def_variant);
                  w.emit_uleb128(static_cast<uint32_t>(td.fields.size()));
                  for (auto& f : td.fields) {
                     w.emit_string(f.name);
                     // Each case: option(valtype) + option(refines)
                     if (f.type_idx == WIT_NO_TYPE) {
                        w.emit_byte(cm::option_none);
                     } else {
                        w.emit_byte(cm::option_some);
                        emit_valtype(w, f.type_idx, remap);
                     }
                     w.emit_byte(cm::option_none);  // refines: none
                  }
                  break;
               case wit_type_kind::enum_:
                  w.emit_byte(cm::def_enum_);
                  w.emit_uleb128(static_cast<uint32_t>(td.fields.size()));
                  for (auto& f : td.fields) {
                     w.emit_string(f.name);
                  }
                  break;
               case wit_type_kind::flags_:
                  w.emit_byte(cm::def_flags);
                  w.emit_uleb128(static_cast<uint32_t>(td.fields.size()));
                  for (auto& f : td.fields) {
                     w.emit_string(f.name);
                  }
                  break;
               case wit_type_kind::tuple_:
                  w.emit_byte(cm::def_tuple);
                  w.emit_uleb128(static_cast<uint32_t>(td.fields.size()));
                  for (auto& f : td.fields) {
                     emit_valtype(w, f.type_idx, remap);
                  }
                  break;
            }
         }

         // Emit a func type
         void emit_func_type(wit_binary_writer& w, const wit_func& func,
                             const std::unordered_map<int32_t, uint32_t>& remap) const {
            w.emit_byte(cm::type_func);
            // Parameters
            w.emit_uleb128(static_cast<uint32_t>(func.params.size()));
            for (auto& p : func.params) {
               w.emit_string(p.name);
               emit_valtype(w, p.type_idx, remap);
            }
            // Results
            if (func.results.size() == 1 && func.results[0].name.empty()) {
               // Single unnamed result
               w.emit_byte(cm::result_single);
               emit_valtype(w, func.results[0].type_idx, remap);
            } else {
               // Named results (or empty = void)
               w.emit_byte(cm::result_named);
               w.emit_uleb128(static_cast<uint32_t>(func.results.size()));
               for (auto& r : func.results) {
                  w.emit_string(r.name);
                  emit_valtype(w, r.type_idx, remap);
               }
            }
         }

         // Emit an export name (discriminant + string)
         void emit_extern_name(wit_binary_writer& w, std::string_view name) const {
            w.emit_byte(cm::name_kebab);
            w.emit_string(name);
         }

         // Ensure a type is emitted, recursively emitting dependencies first.
         void ensure_emitted(int32_t type_idx,
                             wit_binary_writer& w,
                             std::unordered_map<int32_t, uint32_t>& remap,
                             uint32_t& next_type_idx,
                             uint32_t& item_count) const {
            if (type_idx == WIT_NO_TYPE) return;
            if (is_prim_idx(type_idx)) return;
            if (remap.count(type_idx)) return;

            auto idx = static_cast<size_t>(type_idx);
            if (idx >= world_.types.size()) return;

            auto& td = world_.types[idx];
            auto kind = static_cast<wit_type_kind>(td.kind);

            // Ensure dependencies are emitted first
            switch (kind) {
               case wit_type_kind::record_:
                  for (auto& f : td.fields)
                     ensure_emitted(f.type_idx, w, remap, next_type_idx, item_count);
                  break;
               case wit_type_kind::list_:
               case wit_type_kind::option_:
                  ensure_emitted(td.element_type_idx, w, remap, next_type_idx, item_count);
                  break;
               case wit_type_kind::result_:
                  ensure_emitted(td.element_type_idx, w, remap, next_type_idx, item_count);
                  ensure_emitted(td.error_type_idx, w, remap, next_type_idx, item_count);
                  break;
               case wit_type_kind::variant_:
                  for (auto& f : td.fields)
                     if (f.type_idx != WIT_NO_TYPE)
                        ensure_emitted(f.type_idx, w, remap, next_type_idx, item_count);
                  break;
               case wit_type_kind::tuple_:
                  for (auto& f : td.fields)
                     ensure_emitted(f.type_idx, w, remap, next_type_idx, item_count);
                  break;
               default:
                  break;
            }

            if (remap.count(type_idx)) return;  // dependency resolution may have emitted this

            w.emit_byte(cm::item_type_def);
            emit_defined_type(w, td, remap);
            uint32_t def_idx = next_type_idx++;
            item_count++;

            if (!td.name.empty()) {
               w.emit_byte(cm::item_export);
               emit_extern_name(w, td.name);
               w.emit_byte(cm::sort_type);
               w.emit_byte(cm::bound_eq);
               w.emit_uleb128(def_idx);
               remap[type_idx] = next_type_idx++;
               item_count++;
            } else {
               remap[type_idx] = def_idx;
            }
         }

         // Encode an interface as an instance type body.
         // Returns the instance bytes and item count.
         //
         // Emission order (matching wit-component):
         //   1. Named record types with their deps (records + primitive anon types)
         //   2. For each function: remaining anonymous types, then func type + export
         instance_encoding encode_interface(const wit_interface& iface) const {
            std::unordered_map<int32_t, uint32_t> remap;
            uint32_t next_type_idx = 0;
            uint32_t item_count = 0;
            wit_binary_writer w;

            // Phase 1: Emit all named record types (and their dependencies)
            // in the order they appear in the interface's type list
            for (auto type_idx : iface.type_idxs) {
               ensure_emitted(static_cast<int32_t>(type_idx), w, remap,
                              next_type_idx, item_count);
            }

            // Phase 2: Emit functions, lazily emitting remaining anonymous types
            for (auto func_idx : iface.func_idxs) {
               if (func_idx >= world_.funcs.size()) continue;
               auto& func = world_.funcs[func_idx];

               for (auto& p : func.params)
                  ensure_emitted(p.type_idx, w, remap, next_type_idx, item_count);
               for (auto& r : func.results)
                  ensure_emitted(r.type_idx, w, remap, next_type_idx, item_count);

               w.emit_byte(cm::item_type_def);
               emit_func_type(w, func, remap);
               uint32_t func_type_idx = next_type_idx++;
               item_count++;

               w.emit_byte(cm::item_export);
               emit_extern_name(w, func.name);
               w.emit_byte(cm::sort_func);
               w.emit_uleb128(func_type_idx);
               item_count++;
            }

            return {w.take(), item_count};
         }

         // Build the fully qualified interface name: "namespace:package/interface@version"
         std::string qualified_interface_name(const wit_interface& iface) const {
            // package is like "test:inventory@1.0.0"
            // interface name is like "inventory-api"
            // result: "test:inventory/inventory-api@1.0.0"
            auto& pkg = world_.package;
            auto at_pos = pkg.find('@');
            if (at_pos != std::string::npos) {
               return pkg.substr(0, at_pos) + "/" + iface.name + pkg.substr(at_pos);
            }
            return pkg + "/" + iface.name;
         }

         // Build the fully qualified world name
         std::string qualified_world_name() const {
            auto& pkg = world_.package;
            auto at_pos = pkg.find('@');
            if (at_pos != std::string::npos) {
               return pkg.substr(0, at_pos) + "/" + world_.name + pkg.substr(at_pos);
            }
            return pkg + "/" + world_.name;
         }

      public:
         explicit wit_cm_encoder(const wit_world& world) : world_(world) {}

         // Encode to the Component Model binary format used in component-type
         // custom sections. Produces the same encoding as wit-component::metadata::encode().
         std::vector<uint8_t> encode() const {
            wit_binary_writer out;

            // Component header
            out.emit_byte(0x00); out.emit_byte(0x61); out.emit_byte(0x73); out.emit_byte(0x6d);
            out.emit_byte(0x0d); out.emit_byte(0x00); out.emit_byte(0x01); out.emit_byte(0x00);

            // Custom section: wit-component-encoding (version 4, UTF-8)
            {
               wit_binary_writer cs;
               cs.emit_string("wit-component-encoding");
               cs.emit_byte(0x04);  // version
               cs.emit_byte(0x00);  // encoding: UTF-8
               out.emit_byte(cm::section_custom);
               out.emit_uleb128(static_cast<uint32_t>(cs.size()));
               out.emit_bytes(cs.data());
            }

            // Build instances for all import and export interfaces
            // Structure: COMPONENT → COMPONENT → INSTANCE(s) → types/exports
            if (world_.exports.empty() && world_.imports.empty()) return out.take();

            // Pre-encode all interfaces
            struct iface_entry {
               const wit_interface* iface;
               instance_encoding    enc;
               bool                 is_import;
            };
            std::vector<iface_entry> entries;
            for (auto& imp : world_.imports)
               entries.push_back({&imp, encode_interface(imp), true});
            for (auto& exp : world_.exports)
               entries.push_back({&exp, encode_interface(exp), false});

            // Inner component item count: 2 per interface (instance def + import/export)
            uint32_t inner_item_count = static_cast<uint32_t>(entries.size() * 2);

            // Type section: 1 type = outer COMPONENT
            {
               wit_binary_writer type_sec;
               type_sec.emit_uleb128(1);  // 1 type

               // Outer component (2 items: inner component + world export)
               type_sec.emit_byte(cm::type_component);
               type_sec.emit_uleb128(2);

               // Item 0: inner component
               type_sec.emit_byte(cm::item_type_def);
               type_sec.emit_byte(cm::type_component);
               type_sec.emit_uleb128(inner_item_count);

               // Emit each interface: instance type def + import/export
               uint32_t inner_type_idx = 0;
               for (auto& entry : entries) {
                  // Instance type definition
                  type_sec.emit_byte(cm::item_type_def);
                  type_sec.emit_byte(cm::type_instance);
                  type_sec.emit_uleb128(entry.enc.item_count);
                  type_sec.emit_bytes(entry.enc.bytes);

                  // Import or export referencing the instance
                  if (entry.is_import) {
                     type_sec.emit_byte(cm::item_import);
                     emit_extern_name(type_sec, qualified_interface_name(*entry.iface));
                     type_sec.emit_byte(cm::sort_instance);
                     type_sec.emit_uleb128(inner_type_idx);
                  } else {
                     type_sec.emit_byte(cm::item_export);
                     emit_extern_name(type_sec, qualified_interface_name(*entry.iface));
                     type_sec.emit_byte(cm::sort_instance);
                     type_sec.emit_uleb128(inner_type_idx);
                  }
                  inner_type_idx++;
               }

               // Outer item 1: export world as component(0)
               type_sec.emit_byte(cm::item_export);
               emit_extern_name(type_sec, qualified_world_name());
               type_sec.emit_byte(cm::sort_component);
               type_sec.emit_uleb128(0);

               // Write the type section
               out.emit_byte(cm::section_type);
               out.emit_uleb128(static_cast<uint32_t>(type_sec.size()));
               out.emit_bytes(type_sec.data());
            }

            // Export section: export world name as type(0)
            {
               wit_binary_writer exp_sec;
               exp_sec.emit_uleb128(1);  // 1 export
               emit_extern_name(exp_sec, world_.name);
               exp_sec.emit_byte(cm::sort_type);
               exp_sec.emit_uleb128(0);  // type index 0
               exp_sec.emit_byte(0x00);  // no explicit type annotation

               out.emit_byte(cm::section_export);
               out.emit_uleb128(static_cast<uint32_t>(exp_sec.size()));
               out.emit_bytes(exp_sec.data());
            }

            // Custom section: producers
            {
               wit_binary_writer cs;
               cs.emit_string("producers");
               // producers format: vec of (field_name, vec of (name, version))
               cs.emit_uleb128(1);         // 1 field
               cs.emit_string("processed-by");
               cs.emit_uleb128(1);         // 1 entry
               cs.emit_string("psio-wit-gen");
               cs.emit_string("1.0.0");

               out.emit_byte(cm::section_custom);
               out.emit_uleb128(static_cast<uint32_t>(cs.size()));
               out.emit_bytes(cs.data());
            }

            return out.take();
         }
      };

   } // namespace detail

   // =========================================================================
   // Public API
   // =========================================================================

   /// Encode a wit_world to Component Model binary format.
   /// Returns bytes suitable for a component-type custom section.
   inline std::vector<uint8_t> encode_wit_binary(const wit_world& world) {
      detail::wit_cm_encoder encoder(world);
      return encoder.encode();
   }

   /// Generate Component Model binary directly from PSIO_REFLECT-annotated C++ types.
   template<typename Exports, typename Imports = void>
   std::vector<uint8_t> generate_wit_binary(const std::string& package,
                                            const std::string& world_name = "") {
      auto world = generate_wit<Exports, Imports>(package, world_name);
      return encode_wit_binary(world);
   }

} // namespace psio
