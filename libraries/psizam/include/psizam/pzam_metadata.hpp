#pragma once

// Conversion between runtime module struct (types.hpp) and
// serializable pzam_module_metadata (pzam_format.hpp).
//
// extract_metadata(module) → pzam_module_metadata   (at compile time)
// restore_module(metadata) → module                 (at load time)

#include <psizam/pzam_format.hpp>
#include <psizam/types.hpp>

#include <cstring>
#include <span>

namespace psizam {

   // =========================================================================
   // LEB128 encoding helpers
   // =========================================================================

   namespace detail {

      inline void write_leb128_u32(std::vector<uint8_t>& out, uint32_t val) {
         do {
            uint8_t byte = val & 0x7f;
            val >>= 7;
            if (val) byte |= 0x80;
            out.push_back(byte);
         } while (val);
      }

      inline void write_leb128_i32(std::vector<uint8_t>& out, int32_t val) {
         bool more = true;
         while (more) {
            uint8_t byte = val & 0x7f;
            val >>= 7;
            if ((val == 0 && !(byte & 0x40)) || (val == -1 && (byte & 0x40)))
               more = false;
            else
               byte |= 0x80;
            out.push_back(byte);
         }
      }

      inline void write_leb128_i64(std::vector<uint8_t>& out, int64_t val) {
         bool more = true;
         while (more) {
            uint8_t byte = val & 0x7f;
            val >>= 7;
            if ((val == 0 && !(byte & 0x40)) || (val == -1 && (byte & 0x40)))
               more = false;
            else
               byte |= 0x80;
            out.push_back(byte);
         }
      }

      inline uint32_t read_leb128_u32(const uint8_t* data, size_t& pos) {
         uint32_t val = 0;
         unsigned shift = 0;
         uint8_t byte;
         do {
            byte = data[pos++];
            val |= static_cast<uint32_t>(byte & 0x7f) << shift;
            shift += 7;
         } while (byte & 0x80);
         return val;
      }

      inline int32_t read_leb128_i32(const uint8_t* data, size_t& pos) {
         int32_t val = 0;
         unsigned shift = 0;
         uint8_t byte;
         do {
            byte = data[pos++];
            val |= static_cast<int32_t>(byte & 0x7f) << shift;
            shift += 7;
         } while (byte & 0x80);
         if (shift < 32 && (byte & 0x40))
            val |= -(static_cast<int32_t>(1) << shift);
         return val;
      }

      inline int64_t read_leb128_i64(const uint8_t* data, size_t& pos) {
         int64_t val = 0;
         unsigned shift = 0;
         uint8_t byte;
         do {
            byte = data[pos++];
            val |= static_cast<int64_t>(byte & 0x7f) << shift;
            shift += 7;
         } while (byte & 0x80);
         if (shift < 64 && (byte & 0x40))
            val |= -(static_cast<int64_t>(1) << shift);
         return val;
      }

   } // namespace detail

   // =========================================================================
   // init_expr ↔ raw bytes
   // =========================================================================

   /// Serialize an init_expr to raw WASM constant expression bytes.
   /// context_type is needed for ref.null to encode the reference type byte.
   inline std::vector<uint8_t> serialize_init_expr(const init_expr& ie,
                                                    uint8_t context_type = 0) {
      // Extended expressions already have raw bytes (including end opcode)
      if (!ie.raw_expr.empty())
         return ie.raw_expr;

      std::vector<uint8_t> result;
      result.push_back(ie.opcode);

      switch (ie.opcode) {
         case opcodes::i32_const:
            detail::write_leb128_i32(result, ie.value.i32);
            break;
         case opcodes::i64_const:
            detail::write_leb128_i64(result, ie.value.i64);
            break;
         case opcodes::f32_const: {
            auto* bytes = reinterpret_cast<const uint8_t*>(&ie.value.f32);
            result.insert(result.end(), bytes, bytes + 4);
            break;
         }
         case opcodes::f64_const: {
            auto* bytes = reinterpret_cast<const uint8_t*>(&ie.value.f64);
            result.insert(result.end(), bytes, bytes + 8);
            break;
         }
         case opcodes::vector_prefix:
            // v128.const: vector_prefix + varuint(12) + 16 bytes
            detail::write_leb128_u32(result, detail::vec_opcodes::v128_const);
            {
               auto* bytes = reinterpret_cast<const uint8_t*>(&ie.value.v128);
               result.insert(result.end(), bytes, bytes + 16);
            }
            break;
         case opcodes::get_global:
            detail::write_leb128_u32(result, static_cast<uint32_t>(ie.value.i32));
            break;
         case opcodes::ref_null:
            result.push_back(context_type ? context_type : types::funcref);
            break;
         case opcodes::ref_func:
            detail::write_leb128_u32(result, static_cast<uint32_t>(ie.value.i32));
            break;
         default:
            break;
      }

      result.push_back(opcodes::end);
      return result;
   }

   /// Deserialize raw WASM constant expression bytes into an init_expr.
   inline init_expr deserialize_init_expr(std::span<const uint8_t> raw) {
      init_expr ie{};
      if (raw.empty()) return ie;

      size_t pos = 0;
      ie.opcode = raw[pos++];

      switch (ie.opcode) {
         case opcodes::i32_const:
            ie.value.i32 = detail::read_leb128_i32(raw.data(), pos);
            break;
         case opcodes::i64_const:
            ie.value.i64 = detail::read_leb128_i64(raw.data(), pos);
            break;
         case opcodes::f32_const:
            std::memcpy(&ie.value.f32, &raw[pos], 4);
            pos += 4;
            break;
         case opcodes::f64_const:
            std::memcpy(&ie.value.f64, &raw[pos], 8);
            pos += 8;
            break;
         case opcodes::vector_prefix:
            detail::read_leb128_u32(raw.data(), pos); // skip v128.const sub-opcode
            std::memcpy(&ie.value.v128, &raw[pos], 16);
            pos += 16;
            break;
         case opcodes::get_global:
            ie.value.i32 = static_cast<int32_t>(detail::read_leb128_u32(raw.data(), pos));
            break;
         case opcodes::ref_null:
            ie.value.i64 = 0;
            pos++; // skip type byte
            break;
         case opcodes::ref_func:
            ie.value.i32 = static_cast<int32_t>(detail::read_leb128_u32(raw.data(), pos));
            break;
         default:
            break;
      }

      // Check for extended constant expression (more instructions before end)
      if (pos < raw.size() && raw[pos] != opcodes::end) {
         ie.raw_expr.assign(raw.begin(), raw.end());
         ie.opcode = opcodes::i32_const; // placeholder, raw_expr triggers evaluate()
      }

      return ie;
   }

   inline init_expr deserialize_init_expr(const std::vector<uint8_t>& raw) {
      return deserialize_init_expr(std::span<const uint8_t>{raw.data(), raw.size()});
   }

   // =========================================================================
   // Limits conversion helpers
   // =========================================================================

   namespace detail {

      inline pzam_resizable_limits to_pzam(const resizable_limits& rl) {
         return { static_cast<uint8_t>(rl.flags), rl.initial, rl.maximum };
      }

      inline resizable_limits from_pzam(const pzam_resizable_limits& pl) {
         return { pl.has_maximum != 0, pl.initial, pl.maximum };
      }

   } // namespace detail

   // =========================================================================
   // extract_metadata — module → pzam_module_metadata
   // =========================================================================

   /// Extract all module metadata needed for instantiation.
   /// Called at compile time after parsing, before discarding the .wasm.
   inline pzam_module_metadata extract_metadata(const module& mod) {
      pzam_module_metadata meta;

      // --- Types ---
      meta.types.reserve(mod.types.size());
      for (const auto& ft : mod.types) {
         pzam_func_type pft;
         pft.param_types.assign(ft.param_types.begin(), ft.param_types.end());
         pft.return_types.assign(ft.return_types.begin(), ft.return_types.end());
         meta.types.push_back(std::move(pft));
      }

      // --- Imports ---
      meta.imports.reserve(mod.imports.size());
      for (const auto& ie : mod.imports) {
         pzam_import_entry pie;
         pie.module_name.assign(ie.module_str.begin(), ie.module_str.end());
         pie.field_name.assign(ie.field_str.begin(), ie.field_str.end());
         pie.kind = static_cast<uint8_t>(ie.kind);
         switch (ie.kind) {
            case Function:
               pie.func_type_idx = ie.type.func_t;
               break;
            case Table:
               pie.table_type.element_type = ie.type.table_t.element_type;
               pie.table_type.limits = detail::to_pzam(ie.type.table_t.limits);
               break;
            case Memory:
               pie.memory_type.limits = detail::to_pzam(ie.type.mem_t.limits);
               break;
            case Global:
               pie.global_type.content_type = ie.type.global_t.content_type;
               pie.global_type.mutability = ie.type.global_t.mutability;
               break;
            default:
               break;
         }
         meta.imports.push_back(std::move(pie));
      }

      // --- Functions (type index per local function) ---
      meta.functions.assign(mod.functions.begin(), mod.functions.end());

      // --- Tables ---
      meta.tables.reserve(mod.tables.size());
      for (const auto& t : mod.tables) {
         pzam_table_type pt;
         pt.element_type = t.element_type;
         pt.limits = detail::to_pzam(t.limits);
         meta.tables.push_back(std::move(pt));
      }

      // --- Memories ---
      meta.memories.reserve(mod.memories.size());
      for (const auto& m : mod.memories) {
         pzam_memory_type pm;
         pm.limits = detail::to_pzam(m.limits);
         meta.memories.push_back(std::move(pm));
      }

      // --- Globals ---
      meta.globals.reserve(mod.globals.size());
      for (const auto& g : mod.globals) {
         pzam_global_variable pg;
         pg.type.content_type = g.type.content_type;
         pg.type.mutability = g.type.mutability;
         pg.init_expr = serialize_init_expr(g.init, g.type.content_type);
         meta.globals.push_back(std::move(pg));
      }

      // --- Exports ---
      meta.exports.reserve(mod.exports.size());
      for (const auto& e : mod.exports) {
         pzam_export_entry pe;
         pe.field_name.assign(e.field_str.begin(), e.field_str.end());
         pe.kind = static_cast<uint8_t>(e.kind);
         pe.index = e.index;
         meta.exports.push_back(std::move(pe));
      }

      // --- Element segments ---
      meta.elements.reserve(mod.elements.size());
      for (const auto& e : mod.elements) {
         pzam_elem_segment pe;
         pe.table_index = e.index;
         if (e.mode == elem_mode::active)
            pe.offset_expr = serialize_init_expr(e.offset, types::i32);
         pe.mode = static_cast<uint8_t>(e.mode);
         pe.elem_type = e.type;
         pe.elems.reserve(e.elems.size());
         for (const auto& te : e.elems) {
            pzam_elem_entry pee;
            pee.type = static_cast<uint8_t>(te.type);
            pee.index = te.index;
            pe.elems.push_back(pee);
         }
         meta.elements.push_back(std::move(pe));
      }

      // --- Data segments ---
      meta.data.reserve(mod.data.size());
      for (const auto& d : mod.data) {
         pzam_data_segment pd;
         pd.memory_index = d.index;
         if (!d.passive)
            pd.offset_expr = serialize_init_expr(d.offset, types::i32);
         pd.passive = d.passive ? uint8_t(1) : uint8_t(0);
         pd.data = d.data;
         meta.data.push_back(std::move(pd));
      }

      // --- Tags ---
      meta.tags.reserve(mod.tags.size());
      for (const auto& t : mod.tags) {
         pzam_tag_type pt;
         pt.attribute = t.attribute;
         pt.type_index = t.type_index;
         meta.tags.push_back(pt);
      }

      // --- Start function ---
      meta.start_function = mod.start;

      // --- Derived import counts ---
      meta.num_imported_functions = mod.get_imported_functions_size();
      meta.num_imported_tables    = mod.num_imported_tables;
      meta.num_imported_memories  = mod.num_imported_memories;
      meta.num_imported_globals   = mod.num_imported_globals;

      return meta;
   }

   // =========================================================================
   // restore_module — pzam_module_metadata → module
   // =========================================================================

   /// Reconstruct a runtime module from serialized metadata.
   /// The returned module has no compiled code — that comes from the code section.
   inline module restore_module(const pzam_module_metadata& meta) {
      module mod;

      // --- Types ---
      mod.types.reserve(meta.types.size());
      for (const auto& pft : meta.types) {
         func_type ft;
         ft.form = types::func;
         ft.param_types.assign(pft.param_types.begin(), pft.param_types.end());
         ft.return_types.assign(pft.return_types.begin(), pft.return_types.end());
         ft.finalize_returns();
         ft.compute_sig_hash();
         mod.types.push_back(std::move(ft));
      }

      // --- Imports ---
      mod.imports.reserve(meta.imports.size());
      for (const auto& pie : meta.imports) {
         import_entry ie;
         ie.module_str.assign(pie.module_name.begin(), pie.module_name.end());
         ie.field_str.assign(pie.field_name.begin(), pie.field_name.end());
         ie.kind = static_cast<external_kind>(pie.kind);
         switch (ie.kind) {
            case Function:
               ie.type.func_t = pie.func_type_idx;
               break;
            case Table:
               ie.type.table_t.element_type = pie.table_type.element_type;
               ie.type.table_t.limits = detail::from_pzam(pie.table_type.limits);
               break;
            case Memory:
               ie.type.mem_t.limits = detail::from_pzam(pie.memory_type.limits);
               break;
            case Global:
               ie.type.global_t.content_type = pie.global_type.content_type;
               ie.type.global_t.mutability = pie.global_type.mutability;
               break;
            default:
               break;
         }
         mod.imports.push_back(std::move(ie));
      }

      // --- Functions (type index per local function) ---
      mod.functions.assign(meta.functions.begin(), meta.functions.end());

      // --- Tables ---
      mod.tables.reserve(meta.tables.size());
      for (const auto& pt : meta.tables) {
         table_type tt;
         tt.element_type = pt.element_type;
         tt.limits = detail::from_pzam(pt.limits);
         mod.tables.push_back(tt);
      }

      // --- Memories ---
      mod.memories.reserve(meta.memories.size());
      for (const auto& pm : meta.memories) {
         memory_type mt;
         mt.limits = detail::from_pzam(pm.limits);
         mod.memories.push_back(mt);
      }

      // --- Globals ---
      mod.globals.reserve(meta.globals.size());
      for (const auto& pg : meta.globals) {
         global_variable gv;
         gv.type.content_type = pg.type.content_type;
         gv.type.mutability = pg.type.mutability;
         gv.init = deserialize_init_expr(pg.init_expr);
         mod.globals.push_back(std::move(gv));
      }

      // --- Exports ---
      mod.exports.reserve(meta.exports.size());
      for (const auto& pe : meta.exports) {
         export_entry ee;
         ee.field_str.assign(pe.field_name.begin(), pe.field_name.end());
         ee.kind = static_cast<external_kind>(pe.kind);
         ee.index = pe.index;
         mod.exports.push_back(std::move(ee));
      }

      // --- Element segments ---
      mod.elements.reserve(meta.elements.size());
      for (const auto& pe : meta.elements) {
         elem_segment es;
         es.index = pe.table_index;
         es.mode = static_cast<elem_mode>(pe.mode);
         es.type = pe.elem_type;
         if (es.mode == elem_mode::active && !pe.offset_expr.empty())
            es.offset = deserialize_init_expr(pe.offset_expr);
         es.elems.reserve(pe.elems.size());
         for (const auto& pee : pe.elems) {
            table_entry te;
            te.type = pee.type;
            te.index = pee.index;
            te.code_ptr = nullptr;
            es.elems.push_back(te);
         }
         mod.elements.push_back(std::move(es));
      }

      // --- Data segments ---
      mod.data.reserve(meta.data.size());
      for (const auto& pd : meta.data) {
         data_segment ds;
         ds.index = pd.memory_index;
         ds.passive = pd.passive != 0;
         if (!ds.passive && !pd.offset_expr.empty())
            ds.offset = deserialize_init_expr(pd.offset_expr);
         ds.data = pd.data;
         mod.data.push_back(std::move(ds));
      }

      // --- Tags ---
      mod.tags.reserve(meta.tags.size());
      for (const auto& pt : meta.tags) {
         tag_type tt;
         tt.attribute = pt.attribute;
         tt.type_index = pt.type_index;
         mod.tags.push_back(tt);
      }

      // --- Start function ---
      mod.start = meta.start_function;

      // --- Derived counts ---
      mod.num_imported_tables   = meta.num_imported_tables;
      mod.num_imported_memories = meta.num_imported_memories;
      mod.num_imported_globals  = meta.num_imported_globals;

      // --- Set up import_functions and code vectors ---
      mod.import_functions.resize(meta.num_imported_functions);
      mod.code.resize(meta.functions.size());

      return mod;
   }

} // namespace psizam
