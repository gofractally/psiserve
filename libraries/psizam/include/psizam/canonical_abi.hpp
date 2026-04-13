#pragma once

// Canonical ABI — lowering/lifting for the Component Model.
//
// Converts between high-level WIT types (strings, records, lists, variants)
// and flat WASM primitives (i32, i64, f32, f64) + linear memory layout.
//
// Key functions:
//   flatten()    — WIT type → flat primitive sequence
//   layout_of()  — WIT type → {size, alignment}
//   lower()      — dynamic_value + WIT func → native_value[] for WASM call
//   lift()       — native_value[] + WIT func → dynamic_value from WASM return
//   store()      — dynamic_value → bytes in linear memory
//   load()       — bytes from linear memory → dynamic_value
//
// Spec reference: https://github.com/WebAssembly/component-model/blob/main/design/mvp/CanonicalABI.md

#include <psizam/wit_types.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace psizam {

   // ---- Limits ----

   static constexpr size_t MAX_FLAT_PARAMS  = 16;
   static constexpr size_t MAX_FLAT_RESULTS = 1;

   // ---- Flat type (WASM core types) ----

   enum class flat_type : uint8_t {
      i32 = 0,
      i64 = 1,
      f32 = 2,
      f64 = 3,
   };

   // ---- Native value (untyped 64-bit slot) ----

   union native_value {
      uint32_t i32;
      uint64_t i64;
      float    f32;
      double   f64;
   };

   // ---- Layout (size + alignment in linear memory) ----

   struct wit_layout {
      uint32_t size  = 0;
      uint32_t align = 1;
   };

   // ---- Dynamic value ----
   // Runtime tagged value matching WIT's type system.

   class dynamic_value {
    public:
      enum class kind : uint8_t {
         none, bool_, u8, s8, u16, s16, u32, s32, u64, s64, f32, f64,
         char_, string_, list_, record_, variant_, option_, result_,
         enum_, flags_,
      };

      // Constructors
      dynamic_value() : kind_(kind::none) {}

      static dynamic_value make_bool(bool v) {
         dynamic_value d; d.kind_ = kind::bool_; d.u64_ = v ? 1 : 0; return d;
      }
      static dynamic_value make_u8(uint8_t v) {
         dynamic_value d; d.kind_ = kind::u8; d.u64_ = v; return d;
      }
      static dynamic_value make_s8(int8_t v) {
         dynamic_value d; d.kind_ = kind::s8; d.i64_ = v; return d;
      }
      static dynamic_value make_u16(uint16_t v) {
         dynamic_value d; d.kind_ = kind::u16; d.u64_ = v; return d;
      }
      static dynamic_value make_s16(int16_t v) {
         dynamic_value d; d.kind_ = kind::s16; d.i64_ = v; return d;
      }
      static dynamic_value make_u32(uint32_t v) {
         dynamic_value d; d.kind_ = kind::u32; d.u64_ = v; return d;
      }
      static dynamic_value make_s32(int32_t v) {
         dynamic_value d; d.kind_ = kind::s32; d.i64_ = v; return d;
      }
      static dynamic_value make_u64(uint64_t v) {
         dynamic_value d; d.kind_ = kind::u64; d.u64_ = v; return d;
      }
      static dynamic_value make_s64(int64_t v) {
         dynamic_value d; d.kind_ = kind::s64; d.i64_ = v; return d;
      }
      static dynamic_value make_f32(float v) {
         dynamic_value d; d.kind_ = kind::f32; d.f32_ = v; return d;
      }
      static dynamic_value make_f64(double v) {
         dynamic_value d; d.kind_ = kind::f64; d.f64_ = v; return d;
      }
      static dynamic_value make_char(uint32_t v) {
         dynamic_value d; d.kind_ = kind::char_; d.u64_ = v; return d;
      }
      static dynamic_value make_string(std::string v) {
         dynamic_value d; d.kind_ = kind::string_; d.str_ = std::move(v); return d;
      }
      static dynamic_value make_list(std::vector<dynamic_value> v) {
         dynamic_value d; d.kind_ = kind::list_; d.children_ = std::move(v); return d;
      }
      static dynamic_value make_record(std::vector<dynamic_value> fields) {
         dynamic_value d; d.kind_ = kind::record_; d.children_ = std::move(fields); return d;
      }
      static dynamic_value make_variant(uint32_t disc, dynamic_value payload) {
         dynamic_value d;
         d.kind_ = kind::variant_;
         d.u64_ = disc;
         d.children_.push_back(std::move(payload));
         return d;
      }
      static dynamic_value make_option(std::optional<dynamic_value> v) {
         if (v) return make_variant(1, std::move(*v));
         else   return make_variant(0, dynamic_value{});
      }
      static dynamic_value make_enum(uint32_t v) {
         dynamic_value d; d.kind_ = kind::enum_; d.u64_ = v; return d;
      }
      static dynamic_value make_flags(std::vector<uint32_t> bits) {
         dynamic_value d;
         d.kind_ = kind::flags_;
         for (auto b : bits) d.children_.push_back(make_u32(b));
         return d;
      }

      // Accessors
      kind type() const { return kind_; }
      bool        as_bool()   const { return u64_ != 0; }
      uint8_t     as_u8()     const { return static_cast<uint8_t>(u64_); }
      int8_t      as_s8()     const { return static_cast<int8_t>(i64_); }
      uint16_t    as_u16()    const { return static_cast<uint16_t>(u64_); }
      int16_t     as_s16()    const { return static_cast<int16_t>(i64_); }
      uint32_t    as_u32()    const { return static_cast<uint32_t>(u64_); }
      int32_t     as_s32()    const { return static_cast<int32_t>(i64_); }
      uint64_t    as_u64()    const { return u64_; }
      int64_t     as_s64()    const { return i64_; }
      float       as_f32()    const { return f32_; }
      double      as_f64()    const { return f64_; }
      uint32_t    as_char()   const { return static_cast<uint32_t>(u64_); }
      const std::string& as_string() const { return str_; }
      const std::vector<dynamic_value>& as_list() const { return children_; }
      const std::vector<dynamic_value>& fields()  const { return children_; }
      uint32_t discriminant() const { return static_cast<uint32_t>(u64_); }
      const dynamic_value& payload() const { return children_.at(0); }
      uint32_t    as_enum()   const { return static_cast<uint32_t>(u64_); }

    private:
      kind                         kind_ = kind::none;
      uint64_t                     u64_  = 0;
      int64_t                      i64_  = 0;
      float                        f32_  = 0;
      double                       f64_  = 0;
      std::string                  str_;
      std::vector<dynamic_value>   children_;
   };

   // ---- Memory interface ----
   // Abstracts access to WASM linear memory for the canonical ABI.

   struct canonical_memory {
      uint8_t* base   = nullptr;  // pointer to linear memory start
      size_t   length = 0;        // current memory size in bytes

      // cabi_realloc: allocate memory in WASM module's linear memory.
      // This is a function pointer that calls the module's cabi_realloc export.
      // signature: cabi_realloc(old_ptr, old_size, align, new_size) -> ptr
      using realloc_fn = uint32_t(*)(void* host, uint32_t old_ptr, uint32_t old_size,
                                     uint32_t align, uint32_t new_size);
      realloc_fn realloc = nullptr;
      void*      host    = nullptr;

      uint32_t alloc(uint32_t align, uint32_t size) {
         if (!realloc)
            throw std::runtime_error("canonical_abi: no cabi_realloc available");
         return realloc(host, 0, 0, align, size);
      }

      void store_u8(uint32_t offset, uint8_t v) {
         check(offset, 1);
         base[offset] = v;
      }
      void store_u16(uint32_t offset, uint16_t v) {
         check(offset, 2);
         std::memcpy(base + offset, &v, 2);
      }
      void store_u32(uint32_t offset, uint32_t v) {
         check(offset, 4);
         std::memcpy(base + offset, &v, 4);
      }
      void store_u64(uint32_t offset, uint64_t v) {
         check(offset, 8);
         std::memcpy(base + offset, &v, 8);
      }
      void store_f32(uint32_t offset, float v) {
         check(offset, 4);
         std::memcpy(base + offset, &v, 4);
      }
      void store_f64(uint32_t offset, double v) {
         check(offset, 8);
         std::memcpy(base + offset, &v, 8);
      }
      void store_bytes(uint32_t offset, const void* src, uint32_t len) {
         check(offset, len);
         std::memcpy(base + offset, src, len);
      }

      uint8_t  load_u8(uint32_t offset) const { check(offset, 1); return base[offset]; }
      uint16_t load_u16(uint32_t offset) const {
         check(offset, 2); uint16_t v; std::memcpy(&v, base + offset, 2); return v;
      }
      uint32_t load_u32(uint32_t offset) const {
         check(offset, 4); uint32_t v; std::memcpy(&v, base + offset, 4); return v;
      }
      uint64_t load_u64(uint32_t offset) const {
         check(offset, 8); uint64_t v; std::memcpy(&v, base + offset, 8); return v;
      }
      float load_f32(uint32_t offset) const {
         check(offset, 4); float v; std::memcpy(&v, base + offset, 4); return v;
      }
      double load_f64(uint32_t offset) const {
         check(offset, 8); double v; std::memcpy(&v, base + offset, 8); return v;
      }

    private:
      void check(uint32_t offset, uint32_t size) const {
         if (offset + size > length)
            throw std::runtime_error("canonical_abi: out of bounds memory access");
      }
   };

   // =========================================================================
   // Canonical ABI core operations
   // =========================================================================

   /// Compute the linear memory layout (size, alignment) for a WIT type.
   inline wit_layout layout_of(const pzam_wit_world& world, int32_t type_idx) {
      if (is_prim_idx(type_idx)) {
         auto p = idx_to_prim(type_idx);
         switch (p) {
            case wit_prim::bool_:   return {1, 1};
            case wit_prim::u8:     case wit_prim::s8:  return {1, 1};
            case wit_prim::u16:    case wit_prim::s16: return {2, 2};
            case wit_prim::u32:    case wit_prim::s32: return {4, 4};
            case wit_prim::u64:    case wit_prim::s64: return {8, 8};
            case wit_prim::f32:    return {4, 4};
            case wit_prim::f64:    return {8, 8};
            case wit_prim::char_:  return {4, 4};
            case wit_prim::string_: return {8, 4}; // ptr(i32) + len(i32)
         }
      }

      auto idx = static_cast<size_t>(type_idx);
      if (idx >= world.types.size()) return {4, 4};
      auto& td = world.types[idx];

      switch (static_cast<wit_type_kind>(td.kind)) {
         case wit_type_kind::record_: {
            uint32_t size = 0;
            uint32_t align = 1;
            for (auto& f : td.fields) {
               auto fl = layout_of(world, f.type_idx);
               align = std::max(align, fl.align);
               size = (size + fl.align - 1) & ~(fl.align - 1); // pad
               size += fl.size;
            }
            size = (size + align - 1) & ~(align - 1); // trailing pad
            return {size, align};
         }
         case wit_type_kind::variant_: {
            uint32_t disc_size = 4; // i32 discriminant
            uint32_t max_size = 0;
            uint32_t max_align = 4;
            for (auto& c : td.fields) {
               if (c.type_idx != 0) {
                  auto cl = layout_of(world, c.type_idx);
                  max_size = std::max(max_size, cl.size);
                  max_align = std::max(max_align, cl.align);
               }
            }
            uint32_t payload_offset = (disc_size + max_align - 1) & ~(max_align - 1);
            uint32_t total = payload_offset + max_size;
            total = (total + max_align - 1) & ~(max_align - 1);
            return {total, max_align};
         }
         case wit_type_kind::enum_:
            return {4, 4}; // i32 discriminant
         case wit_type_kind::flags_: {
            uint32_t n = static_cast<uint32_t>(td.fields.size());
            uint32_t words = (n + 31) / 32;
            return {words * 4, 4};
         }
         case wit_type_kind::list_:
            return {8, 4}; // ptr(i32) + len(i32)
         case wit_type_kind::option_:
            // Despecialized to variant with 2 cases: none (no payload) | some(T)
            return layout_of(world, type_idx); // shouldn't recurse here
         case wit_type_kind::result_:
            return {12, 4}; // disc(i32) + max(ok_size, err_size), conservative
         case wit_type_kind::tuple_: {
            uint32_t size = 0;
            uint32_t align = 1;
            for (auto& f : td.fields) {
               auto fl = layout_of(world, f.type_idx);
               align = std::max(align, fl.align);
               size = (size + fl.align - 1) & ~(fl.align - 1);
               size += fl.size;
            }
            size = (size + align - 1) & ~(align - 1);
            return {size, align};
         }
      }
      return {4, 4};
   }

   /// Compute the flat type sequence for a WIT type (used for function signatures).
   inline std::vector<flat_type> flatten(const pzam_wit_world& world, int32_t type_idx) {
      if (is_prim_idx(type_idx)) {
         auto p = idx_to_prim(type_idx);
         switch (p) {
            case wit_prim::bool_:
            case wit_prim::u8:  case wit_prim::s8:
            case wit_prim::u16: case wit_prim::s16:
            case wit_prim::u32: case wit_prim::s32:
            case wit_prim::char_:
               return {flat_type::i32};
            case wit_prim::u64: case wit_prim::s64:
               return {flat_type::i64};
            case wit_prim::f32:
               return {flat_type::f32};
            case wit_prim::f64:
               return {flat_type::f64};
            case wit_prim::string_:
               return {flat_type::i32, flat_type::i32}; // ptr, len
         }
      }

      auto idx = static_cast<size_t>(type_idx);
      if (idx >= world.types.size()) return {flat_type::i32};
      auto& td = world.types[idx];

      switch (static_cast<wit_type_kind>(td.kind)) {
         case wit_type_kind::record_:
         case wit_type_kind::tuple_: {
            std::vector<flat_type> result;
            for (auto& f : td.fields) {
               auto sub = flatten(world, f.type_idx);
               result.insert(result.end(), sub.begin(), sub.end());
            }
            return result;
         }
         case wit_type_kind::variant_: {
            // discriminant + join of all case payloads
            std::vector<flat_type> result = {flat_type::i32};
            size_t max_flat = 0;
            for (auto& c : td.fields) {
               if (c.type_idx != 0) {
                  auto sub = flatten(world, c.type_idx);
                  max_flat = std::max(max_flat, sub.size());
               }
            }
            // Join: use i64 for each slot (widest)
            for (size_t i = 0; i < max_flat; i++)
               result.push_back(flat_type::i64);
            return result;
         }
         case wit_type_kind::enum_:
            return {flat_type::i32};
         case wit_type_kind::flags_: {
            uint32_t n = static_cast<uint32_t>(td.fields.size());
            uint32_t words = (n + 31) / 32;
            std::vector<flat_type> result(words, flat_type::i32);
            return result;
         }
         case wit_type_kind::list_:
            return {flat_type::i32, flat_type::i32}; // ptr, len
         case wit_type_kind::option_: {
            // disc(i32) + flatten(element)
            std::vector<flat_type> result = {flat_type::i32};
            auto sub = flatten(world, td.element_type_idx);
            for (size_t i = 0; i < sub.size(); i++)
               result.push_back(flat_type::i64);
            return result;
         }
         case wit_type_kind::result_: {
            std::vector<flat_type> result = {flat_type::i32};
            auto ok_flat  = flatten(world, td.element_type_idx);
            auto err_flat = flatten(world, td.error_type_idx);
            size_t max_flat = std::max(ok_flat.size(), err_flat.size());
            for (size_t i = 0; i < max_flat; i++)
               result.push_back(flat_type::i64);
            return result;
         }
      }
      return {flat_type::i32};
   }

   // ---- Store: dynamic_value → linear memory ----

   inline void canonical_store(const pzam_wit_world& world, int32_t type_idx,
                                const dynamic_value& val, canonical_memory& mem,
                                uint32_t offset) {
      if (is_prim_idx(type_idx)) {
         auto p = idx_to_prim(type_idx);
         switch (p) {
            case wit_prim::bool_:   mem.store_u8(offset, val.as_bool() ? 1 : 0); return;
            case wit_prim::u8:      mem.store_u8(offset, val.as_u8()); return;
            case wit_prim::s8:      mem.store_u8(offset, static_cast<uint8_t>(val.as_s8())); return;
            case wit_prim::u16:     mem.store_u16(offset, val.as_u16()); return;
            case wit_prim::s16:     mem.store_u16(offset, static_cast<uint16_t>(val.as_s16())); return;
            case wit_prim::u32:     mem.store_u32(offset, val.as_u32()); return;
            case wit_prim::s32:     mem.store_u32(offset, static_cast<uint32_t>(val.as_s32())); return;
            case wit_prim::u64:     mem.store_u64(offset, val.as_u64()); return;
            case wit_prim::s64:     mem.store_u64(offset, static_cast<uint64_t>(val.as_s64())); return;
            case wit_prim::f32:     mem.store_f32(offset, val.as_f32()); return;
            case wit_prim::f64:     mem.store_f64(offset, val.as_f64()); return;
            case wit_prim::char_:   mem.store_u32(offset, val.as_char()); return;
            case wit_prim::string_: {
               auto& s = val.as_string();
               uint32_t ptr = mem.alloc(1, static_cast<uint32_t>(s.size()));
               mem.store_bytes(ptr, s.data(), static_cast<uint32_t>(s.size()));
               mem.store_u32(offset, ptr);
               mem.store_u32(offset + 4, static_cast<uint32_t>(s.size()));
               return;
            }
         }
         return;
      }

      auto idx = static_cast<size_t>(type_idx);
      if (idx >= world.types.size()) return;
      auto& td = world.types[idx];

      switch (static_cast<wit_type_kind>(td.kind)) {
         case wit_type_kind::record_:
         case wit_type_kind::tuple_: {
            uint32_t pos = offset;
            for (size_t i = 0; i < td.fields.size() && i < val.fields().size(); i++) {
               auto fl = layout_of(world, td.fields[i].type_idx);
               pos = (pos + fl.align - 1) & ~(fl.align - 1);
               canonical_store(world, td.fields[i].type_idx, val.fields()[i], mem, pos);
               pos += fl.size;
            }
            return;
         }
         case wit_type_kind::enum_:
            mem.store_u32(offset, val.as_enum());
            return;
         case wit_type_kind::flags_: {
            auto& bits = val.as_list();
            for (size_t i = 0; i < bits.size(); i++)
               mem.store_u32(offset + static_cast<uint32_t>(i * 4), bits[i].as_u32());
            return;
         }
         case wit_type_kind::list_: {
            auto& elems = val.as_list();
            auto el = layout_of(world, td.element_type_idx);
            uint32_t total_size = static_cast<uint32_t>(elems.size()) * el.size;
            uint32_t ptr = mem.alloc(el.align, total_size);
            for (size_t i = 0; i < elems.size(); i++) {
               canonical_store(world, td.element_type_idx, elems[i], mem,
                               ptr + static_cast<uint32_t>(i) * el.size);
            }
            mem.store_u32(offset, ptr);
            mem.store_u32(offset + 4, static_cast<uint32_t>(elems.size()));
            return;
         }
         case wit_type_kind::variant_:
         case wit_type_kind::option_:
         case wit_type_kind::result_: {
            mem.store_u32(offset, val.discriminant());
            if (!val.payload().fields().empty() || val.payload().type() != dynamic_value::kind::none) {
               // Find payload type
               int32_t payload_type = 0;
               if (static_cast<wit_type_kind>(td.kind) == wit_type_kind::variant_) {
                  uint32_t disc = val.discriminant();
                  if (disc < td.fields.size()) payload_type = td.fields[disc].type_idx;
               } else if (static_cast<wit_type_kind>(td.kind) == wit_type_kind::option_) {
                  if (val.discriminant() == 1) payload_type = td.element_type_idx;
               } else { // result
                  payload_type = (val.discriminant() == 0) ? td.element_type_idx : td.error_type_idx;
               }
               if (payload_type != 0) {
                  auto pl = layout_of(world, payload_type);
                  uint32_t payload_offset = (4 + pl.align - 1) & ~(pl.align - 1);
                  canonical_store(world, payload_type, val.payload(), mem, offset + payload_offset);
               }
            }
            return;
         }
      }
   }

   // ---- Load: linear memory → dynamic_value ----

   inline dynamic_value canonical_load(const pzam_wit_world& world, int32_t type_idx,
                                        const canonical_memory& mem, uint32_t offset) {
      if (is_prim_idx(type_idx)) {
         auto p = idx_to_prim(type_idx);
         switch (p) {
            case wit_prim::bool_:   return dynamic_value::make_bool(mem.load_u8(offset) != 0);
            case wit_prim::u8:      return dynamic_value::make_u8(mem.load_u8(offset));
            case wit_prim::s8:      return dynamic_value::make_s8(static_cast<int8_t>(mem.load_u8(offset)));
            case wit_prim::u16:     return dynamic_value::make_u16(mem.load_u16(offset));
            case wit_prim::s16:     return dynamic_value::make_s16(static_cast<int16_t>(mem.load_u16(offset)));
            case wit_prim::u32:     return dynamic_value::make_u32(mem.load_u32(offset));
            case wit_prim::s32:     return dynamic_value::make_s32(static_cast<int32_t>(mem.load_u32(offset)));
            case wit_prim::u64:     return dynamic_value::make_u64(mem.load_u64(offset));
            case wit_prim::s64:     return dynamic_value::make_s64(static_cast<int64_t>(mem.load_u64(offset)));
            case wit_prim::f32:     return dynamic_value::make_f32(mem.load_f32(offset));
            case wit_prim::f64:     return dynamic_value::make_f64(mem.load_f64(offset));
            case wit_prim::char_:   return dynamic_value::make_char(mem.load_u32(offset));
            case wit_prim::string_: {
               uint32_t ptr = mem.load_u32(offset);
               uint32_t len = mem.load_u32(offset + 4);
               return dynamic_value::make_string(
                  std::string(reinterpret_cast<const char*>(mem.base + ptr), len));
            }
         }
      }

      auto idx = static_cast<size_t>(type_idx);
      if (idx >= world.types.size()) return {};
      auto& td = world.types[idx];

      switch (static_cast<wit_type_kind>(td.kind)) {
         case wit_type_kind::record_:
         case wit_type_kind::tuple_: {
            std::vector<dynamic_value> fields;
            uint32_t pos = offset;
            for (auto& f : td.fields) {
               auto fl = layout_of(world, f.type_idx);
               pos = (pos + fl.align - 1) & ~(fl.align - 1);
               fields.push_back(canonical_load(world, f.type_idx, mem, pos));
               pos += fl.size;
            }
            return dynamic_value::make_record(std::move(fields));
         }
         case wit_type_kind::enum_:
            return dynamic_value::make_enum(mem.load_u32(offset));
         case wit_type_kind::flags_: {
            uint32_t words = (static_cast<uint32_t>(td.fields.size()) + 31) / 32;
            std::vector<uint32_t> bits;
            for (uint32_t i = 0; i < words; i++)
               bits.push_back(mem.load_u32(offset + i * 4));
            return dynamic_value::make_flags(std::move(bits));
         }
         case wit_type_kind::list_: {
            uint32_t ptr = mem.load_u32(offset);
            uint32_t len = mem.load_u32(offset + 4);
            auto el = layout_of(world, td.element_type_idx);
            std::vector<dynamic_value> elems;
            for (uint32_t i = 0; i < len; i++) {
               elems.push_back(canonical_load(world, td.element_type_idx, mem,
                                               ptr + i * el.size));
            }
            return dynamic_value::make_list(std::move(elems));
         }
         case wit_type_kind::variant_: {
            uint32_t disc = mem.load_u32(offset);
            int32_t payload_type = 0;
            if (disc < td.fields.size()) payload_type = td.fields[disc].type_idx;
            dynamic_value payload;
            if (payload_type != 0) {
               auto pl = layout_of(world, payload_type);
               uint32_t payload_offset = (4 + pl.align - 1) & ~(pl.align - 1);
               payload = canonical_load(world, payload_type, mem, offset + payload_offset);
            }
            return dynamic_value::make_variant(disc, std::move(payload));
         }
         case wit_type_kind::option_: {
            uint32_t disc = mem.load_u32(offset);
            if (disc == 0) return dynamic_value::make_option(std::nullopt);
            auto el = layout_of(world, td.element_type_idx);
            uint32_t payload_offset = (4 + el.align - 1) & ~(el.align - 1);
            auto payload = canonical_load(world, td.element_type_idx, mem, offset + payload_offset);
            return dynamic_value::make_option(std::move(payload));
         }
         case wit_type_kind::result_: {
            uint32_t disc = mem.load_u32(offset);
            int32_t payload_type = (disc == 0) ? td.element_type_idx : td.error_type_idx;
            dynamic_value payload;
            if (payload_type != 0) {
               auto pl = layout_of(world, payload_type);
               uint32_t payload_offset = (4 + pl.align - 1) & ~(pl.align - 1);
               payload = canonical_load(world, payload_type, mem, offset + payload_offset);
            }
            return dynamic_value::make_variant(disc, std::move(payload));
         }
      }
      return {};
   }

   // ---- Lower: dynamic_value → flat native_value[] for function call ----

   inline void canonical_lower_prim(int32_t type_idx, const dynamic_value& val,
                                     std::vector<native_value>& out) {
      auto p = idx_to_prim(type_idx);
      native_value nv = {};
      switch (p) {
         case wit_prim::bool_:   nv.i32 = val.as_bool() ? 1 : 0; break;
         case wit_prim::u8:      nv.i32 = val.as_u8(); break;
         case wit_prim::s8:      nv.i32 = static_cast<uint32_t>(static_cast<int32_t>(val.as_s8())); break;
         case wit_prim::u16:     nv.i32 = val.as_u16(); break;
         case wit_prim::s16:     nv.i32 = static_cast<uint32_t>(static_cast<int32_t>(val.as_s16())); break;
         case wit_prim::u32:     nv.i32 = val.as_u32(); break;
         case wit_prim::s32:     nv.i32 = static_cast<uint32_t>(val.as_s32()); break;
         case wit_prim::u64:     nv.i64 = val.as_u64(); break;
         case wit_prim::s64:     nv.i64 = static_cast<uint64_t>(val.as_s64()); break;
         case wit_prim::f32:     nv.f32 = val.as_f32(); break;
         case wit_prim::f64:     nv.f64 = val.as_f64(); break;
         case wit_prim::char_:   nv.i32 = val.as_char(); break;
         case wit_prim::string_: {
            // string lowering happens during the store phase — here we just
            // note that it needs ptr/len (handled by canonical_lower)
            return;
         }
      }
      out.push_back(nv);
   }

   /// Lower function arguments from dynamic_value[] to native_value[].
   /// If flat args exceed MAX_FLAT_PARAMS, spills to linear memory.
   inline std::vector<native_value> canonical_lower(
      const pzam_wit_world& world, const wit_func& func,
      std::span<const dynamic_value> args, canonical_memory& mem)
   {
      // Compute total flat args
      std::vector<flat_type> flat_params;
      for (auto& p : func.params) {
         auto fp = flatten(world, p.type_idx);
         flat_params.insert(flat_params.end(), fp.begin(), fp.end());
      }

      std::vector<native_value> result;

      if (flat_params.size() <= MAX_FLAT_PARAMS) {
         // Flat lowering: each arg becomes 1+ native values
         for (size_t i = 0; i < func.params.size() && i < args.size(); i++) {
            int32_t type_idx = func.params[i].type_idx;
            auto& val = args[i];

            if (is_prim_idx(type_idx)) {
               auto p = idx_to_prim(type_idx);
               if (p == wit_prim::string_) {
                  // Lower string: copy to linear memory, pass ptr+len
                  auto& s = val.as_string();
                  uint32_t ptr = mem.alloc(1, static_cast<uint32_t>(s.size()));
                  mem.store_bytes(ptr, s.data(), static_cast<uint32_t>(s.size()));
                  native_value nv_ptr = {}; nv_ptr.i32 = ptr;
                  native_value nv_len = {}; nv_len.i32 = static_cast<uint32_t>(s.size());
                  result.push_back(nv_ptr);
                  result.push_back(nv_len);
               } else {
                  canonical_lower_prim(type_idx, val, result);
               }
            } else {
               // Complex type: store to memory, pass pointer
               auto tl = layout_of(world, type_idx);
               auto flat = flatten(world, type_idx);
               if (flat.size() <= MAX_FLAT_PARAMS - result.size()) {
                  // Still fits flat — for now, spill to memory for simplicity
                  uint32_t ptr = mem.alloc(tl.align, tl.size);
                  canonical_store(world, type_idx, val, mem, ptr);
                  native_value nv = {}; nv.i32 = ptr;
                  result.push_back(nv);
               } else {
                  uint32_t ptr = mem.alloc(tl.align, tl.size);
                  canonical_store(world, type_idx, val, mem, ptr);
                  native_value nv = {}; nv.i32 = ptr;
                  result.push_back(nv);
               }
            }
         }
      } else {
         // Spill all args to a tuple in linear memory
         // Calculate total layout
         uint32_t total_size = 0;
         uint32_t max_align = 1;
         for (auto& p : func.params) {
            auto tl = layout_of(world, p.type_idx);
            max_align = std::max(max_align, tl.align);
            total_size = (total_size + tl.align - 1) & ~(tl.align - 1);
            total_size += tl.size;
         }
         uint32_t ptr = mem.alloc(max_align, total_size);
         uint32_t pos = 0;
         for (size_t i = 0; i < func.params.size() && i < args.size(); i++) {
            auto tl = layout_of(world, func.params[i].type_idx);
            pos = (pos + tl.align - 1) & ~(tl.align - 1);
            canonical_store(world, func.params[i].type_idx, args[i], mem, ptr + pos);
            pos += tl.size;
         }
         native_value nv = {}; nv.i32 = ptr;
         result.push_back(nv);
      }

      return result;
   }

   /// Lift function results from native_value[] to dynamic_value.
   /// For single-return functions (the common case), returns the single result.
   inline dynamic_value canonical_lift(
      const pzam_wit_world& world, const wit_func& func,
      std::span<const native_value> results, const canonical_memory& mem)
   {
      if (func.results.empty())
         return {};

      int32_t type_idx = func.results[0].type_idx;

      if (is_prim_idx(type_idx)) {
         auto p = idx_to_prim(type_idx);
         if (results.empty()) return {};
         auto& nv = results[0];
         switch (p) {
            case wit_prim::bool_:   return dynamic_value::make_bool(nv.i32 != 0);
            case wit_prim::u8:      return dynamic_value::make_u8(static_cast<uint8_t>(nv.i32));
            case wit_prim::s8:      return dynamic_value::make_s8(static_cast<int8_t>(nv.i32));
            case wit_prim::u16:     return dynamic_value::make_u16(static_cast<uint16_t>(nv.i32));
            case wit_prim::s16:     return dynamic_value::make_s16(static_cast<int16_t>(nv.i32));
            case wit_prim::u32:     return dynamic_value::make_u32(nv.i32);
            case wit_prim::s32:     return dynamic_value::make_s32(static_cast<int32_t>(nv.i32));
            case wit_prim::u64:     return dynamic_value::make_u64(nv.i64);
            case wit_prim::s64:     return dynamic_value::make_s64(static_cast<int64_t>(nv.i64));
            case wit_prim::f32:     return dynamic_value::make_f32(nv.f32);
            case wit_prim::f64:     return dynamic_value::make_f64(nv.f64);
            case wit_prim::char_:   return dynamic_value::make_char(nv.i32);
            case wit_prim::string_: {
               // Result is ptr+len in first two slots, or spilled to memory
               if (results.size() >= 2) {
                  uint32_t ptr = results[0].i32;
                  uint32_t len = results[1].i32;
                  return dynamic_value::make_string(
                     std::string(reinterpret_cast<const char*>(mem.base + ptr), len));
               }
               // Spilled: single i32 is pointer to (ptr, len) in memory
               uint32_t ret_ptr = nv.i32;
               uint32_t str_ptr = mem.load_u32(ret_ptr);
               uint32_t str_len = mem.load_u32(ret_ptr + 4);
               return dynamic_value::make_string(
                  std::string(reinterpret_cast<const char*>(mem.base + str_ptr), str_len));
            }
         }
      }

      // Complex return type: result is a pointer to the value in linear memory
      if (!results.empty()) {
         uint32_t ptr = results[0].i32;
         return canonical_load(world, type_idx, mem, ptr);
      }

      return {};
   }

} // namespace psizam
