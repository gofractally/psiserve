#pragma once

// Generate WIT (WebAssembly Interface Types) from PSIO_REFLECT-annotated C++ types.
//
//   struct my_exports {
//      std::string greet(std::string name);
//      uint32_t add(uint32_t a, uint32_t b);
//   };
//   PSIO_REFLECT(my_exports, method(greet, name), method(add, a, b))
//
//   auto world = psio::generate_wit<my_exports>();
//   auto text  = psio::generate_wit_text<my_exports>();
//
// Type mapping:
//   uint8_t/int8_t     → u8/s8          std::string        → string
//   uint16_t/int16_t   → u16/s16        std::vector<T>     → list<T>
//   uint32_t/int32_t   → u32/s32        std::optional<T>   → option<T>
//   uint64_t/int64_t   → u64/s64        PSIO_REFLECT struct → record { ... }
//   float/double       → f32/f64
//   bool               → bool

#include <psio/wit_types.hpp>

#include <psio/reflect.hpp>

#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace psio {

   namespace detail {

      // ── Type index encoding for primitives ──

      template<typename T>
      struct wit_prim_map { static constexpr bool has_value = false; };

      #define PSIO_WIT_PRIM_MAP(CPP_TYPE, WIT_PRIM) \
         template<> struct wit_prim_map<CPP_TYPE> { \
            static constexpr bool has_value = true; \
            static constexpr wit_prim value = WIT_PRIM; \
         };

      PSIO_WIT_PRIM_MAP(bool,     wit_prim::bool_)
      PSIO_WIT_PRIM_MAP(uint8_t,  wit_prim::u8)
      PSIO_WIT_PRIM_MAP(int8_t,   wit_prim::s8)
      PSIO_WIT_PRIM_MAP(uint16_t, wit_prim::u16)
      PSIO_WIT_PRIM_MAP(int16_t,  wit_prim::s16)
      PSIO_WIT_PRIM_MAP(uint32_t, wit_prim::u32)
      PSIO_WIT_PRIM_MAP(int32_t,  wit_prim::s32)
      PSIO_WIT_PRIM_MAP(uint64_t, wit_prim::u64)
      PSIO_WIT_PRIM_MAP(int64_t,  wit_prim::s64)
      PSIO_WIT_PRIM_MAP(float,    wit_prim::f32)
      PSIO_WIT_PRIM_MAP(double,   wit_prim::f64)
      PSIO_WIT_PRIM_MAP(char,     wit_prim::char_)

      #undef PSIO_WIT_PRIM_MAP

      // ── Detect std::string ──
      template<typename T> struct is_std_string : std::false_type {};
      template<> struct is_std_string<std::string> : std::true_type {};

      // ── Detect std::vector<T> ──
      template<typename T> struct is_std_vector : std::false_type {};
      template<typename T> struct is_std_vector<std::vector<T>> : std::true_type {};
      template<typename T> struct vector_element {};
      template<typename T> struct vector_element<std::vector<T>> { using type = T; };

      // ── Detect std::optional<T> ──
      template<typename T> struct is_std_optional : std::false_type {};
      template<typename T> struct is_std_optional<std::optional<T>> : std::true_type {};
      template<typename T> struct optional_element {};
      template<typename T> struct optional_element<std::optional<T>> { using type = T; };

      // ── Detect PSIO_REFLECT-ed struct ──
      template<typename T>
      concept WitReflected = Reflected<T> && reflect<T>::is_struct;

      // ── WIT generation context ──
      // Accumulates types as they are discovered, deduplicating by std::type_index.

      struct wit_gen_ctx {
         wit_world                                    world;
         std::unordered_map<std::type_index, int32_t> type_cache;

         template<typename T>
         int32_t resolve_type() {
            using U = std::remove_cvref_t<T>;

            // 1. Primitive
            if constexpr (wit_prim_map<U>::has_value) {
               return wit_prim_idx(wit_prim_map<U>::value);
            }
            // 2. std::string → string primitive
            else if constexpr (is_std_string<U>::value) {
               return wit_prim_idx(wit_prim::string_);
            }
            // 3. std::vector<T> → list<T>
            else if constexpr (is_std_vector<U>::value) {
               auto key = std::type_index(typeid(U));
               auto it = type_cache.find(key);
               if (it != type_cache.end()) return it->second;

               using Elem = typename vector_element<U>::type;
               int32_t elem_idx = resolve_type<Elem>();

               wit_type_def td;
               td.kind = static_cast<uint8_t>(wit_type_kind::list_);
               td.element_type_idx = elem_idx;
               int32_t idx = add_type(std::move(td));
               type_cache[key] = idx;
               return idx;
            }
            // 4. std::optional<T> → option<T>
            else if constexpr (is_std_optional<U>::value) {
               auto key = std::type_index(typeid(U));
               auto it = type_cache.find(key);
               if (it != type_cache.end()) return it->second;

               using Elem = typename optional_element<U>::type;
               int32_t elem_idx = resolve_type<Elem>();

               wit_type_def td;
               td.kind = static_cast<uint8_t>(wit_type_kind::option_);
               td.element_type_idx = elem_idx;
               int32_t idx = add_type(std::move(td));
               type_cache[key] = idx;
               return idx;
            }
            // 5. PSIO_REFLECT struct → record
            else if constexpr (WitReflected<U>) {
               auto key = std::type_index(typeid(U));
               auto it = type_cache.find(key);
               if (it != type_cache.end()) return it->second;

               // Reserve slot to handle recursive types
               int32_t idx = static_cast<int32_t>(world.types.size());
               type_cache[key] = idx;
               world.types.push_back(wit_type_def{});

               wit_type_def td;
               td.name = std::string(reflect<U>::name);
               td.kind = static_cast<uint8_t>(wit_type_kind::record_);

               // Iterate data members
               size_t field_i = 0;
               apply_members(
                  (typename reflect<U>::data_members*)nullptr,
                  [&](auto... ptrs) {
                     auto process = [&](auto ptr) {
                        using MType = MemberPtrType<decltype(ptr)>;
                        using ValType = std::remove_cvref_t<typename MType::ValueType>;
                        const char* name = reflect<U>::data_member_names[field_i];
                        int32_t type_idx = resolve_type<ValType>();
                        td.fields.push_back({std::string(name), type_idx});
                        field_i++;
                     };
                     (process(ptrs), ...);
                  }
               );

               world.types[idx] = std::move(td);
               return idx;
            }
            else {
               // Unknown type — map to u32 as fallback
               return wit_prim_idx(wit_prim::u32);
            }
         }

         int32_t add_type(wit_type_def&& td) {
            int32_t idx = static_cast<int32_t>(world.types.size());
            world.types.push_back(std::move(td));
            return idx;
         }

         // ── Generate functions from a reflected type ──

         template<typename T>
         void generate_functions(wit_interface& iface) {
            using R = reflect<T>;
            size_t method_i = 0;
            apply_members(
               (typename R::member_functions*)nullptr,
               [&](auto... ptrs) {
                  auto process = [&](auto ptr) {
                     using MType = MemberPtrType<decltype(ptr)>;

                     // Get method name and parameter names
                     auto& names = R::member_function_names[method_i];
                     auto it = names.begin();
                     std::string func_name(*it);
                     ++it;

                     wit_func func;
                     func.name = to_kebab_case(func_name);

                     // Process parameters with names
                     size_t param_i = 0;
                     forEachType(typename MType::ArgTypes{},
                        [&](auto* type_ptr) {
                           using ArgType = std::remove_cvref_t<std::remove_pointer_t<decltype(type_ptr)>>;
                           const char* pname = (it != names.end()) ? *it++ : "";
                           int32_t type_idx = resolve_type<ArgType>();
                           func.params.push_back({to_kebab_case(std::string(pname)), type_idx});
                           param_i++;
                        }
                     );

                     // Return type
                     if constexpr (!std::is_void_v<typename MType::ReturnType>) {
                        using RetType = std::remove_cvref_t<typename MType::ReturnType>;
                        int32_t type_idx = resolve_type<RetType>();
                        func.results.push_back({"", type_idx});
                     }

                     world.funcs.push_back(std::move(func));
                     uint32_t func_idx = static_cast<uint32_t>(world.funcs.size() - 1);
                     iface.func_idxs.push_back(func_idx);
                     method_i++;
                  };
                  (process(ptrs), ...);
               }
            );
         }

         // ── Convert snake_case/camelCase to kebab-case ──

         static std::string to_kebab_case(const std::string& s) {
            std::string result;
            for (size_t i = 0; i < s.size(); i++) {
               char c = s[i];
               if (c == '_') {
                  if (!result.empty() && result.back() != '-')
                     result += '-';
               } else if (std::isupper(static_cast<unsigned char>(c))) {
                  if (!result.empty() && result.back() != '-')
                     result += '-';
                  result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
               } else {
                  result += c;
               }
            }
            return result;
         }
      };

      // ── WIT text generation from a wit_world ──

      inline std::string wit_type_name(const wit_world& world, int32_t type_idx) {
         if (is_prim_idx(type_idx)) {
            auto p = idx_to_prim(type_idx);
            switch (p) {
               case wit_prim::bool_:   return "bool";
               case wit_prim::u8:      return "u8";
               case wit_prim::s8:      return "s8";
               case wit_prim::u16:     return "u16";
               case wit_prim::s16:     return "s16";
               case wit_prim::u32:     return "u32";
               case wit_prim::s32:     return "s32";
               case wit_prim::u64:     return "u64";
               case wit_prim::s64:     return "s64";
               case wit_prim::f32:     return "f32";
               case wit_prim::f64:     return "f64";
               case wit_prim::char_:   return "char";
               case wit_prim::string_: return "string";
            }
            return "u32"; // fallback
         }

         auto idx = static_cast<size_t>(type_idx);
         if (idx >= world.types.size()) return "u32";
         auto& td = world.types[idx];

         switch (static_cast<wit_type_kind>(td.kind)) {
            case wit_type_kind::list_:
               return "list<" + wit_type_name(world, td.element_type_idx) + ">";
            case wit_type_kind::option_:
               return "option<" + wit_type_name(world, td.element_type_idx) + ">";
            case wit_type_kind::result_:
               return "result<" + wit_type_name(world, td.element_type_idx) + ", " +
                      wit_type_name(world, td.error_type_idx) + ">";
            case wit_type_kind::tuple_: {
               std::string s = "tuple<";
               for (size_t i = 0; i < td.fields.size(); i++) {
                  if (i > 0) s += ", ";
                  s += wit_type_name(world, td.fields[i].type_idx);
               }
               s += ">";
               return s;
            }
            default:
               return td.name.empty() ? "u32" : td.name;
         }
      }

      inline void wit_emit_func(std::ostringstream& os, const wit_world& world,
                                 const wit_func& func, const std::string& indent) {
         os << indent << func.name << ": func(";
         for (size_t i = 0; i < func.params.size(); i++) {
            if (i > 0) os << ", ";
            os << func.params[i].name << ": " << wit_type_name(world, func.params[i].type_idx);
         }
         os << ")";
         if (!func.results.empty()) {
            if (func.results.size() == 1 && func.results[0].name.empty()) {
               os << " -> " << wit_type_name(world, func.results[0].type_idx);
            } else {
               os << " -> (";
               for (size_t i = 0; i < func.results.size(); i++) {
                  if (i > 0) os << ", ";
                  if (!func.results[i].name.empty())
                     os << func.results[i].name << ": ";
                  os << wit_type_name(world, func.results[i].type_idx);
               }
               os << ")";
            }
         }
         os << ";\n";
      }

      inline void wit_emit_type(std::ostringstream& os, const wit_world& world,
                                 const wit_type_def& td, const std::string& indent) {
         switch (static_cast<wit_type_kind>(td.kind)) {
            case wit_type_kind::record_:
               if (td.fields.empty() && td.element_type_idx != 0) {
                  // type alias
                  os << indent << "type " << td.name << " = "
                     << wit_type_name(world, td.element_type_idx) << ";\n";
                  return;
               }
               os << indent << "record " << td.name << " {\n";
               for (auto& f : td.fields)
                  os << indent << "  " << f.name << ": " << wit_type_name(world, f.type_idx) << ",\n";
               os << indent << "}\n";
               break;
            case wit_type_kind::variant_:
               os << indent << "variant " << td.name << " {\n";
               for (auto& f : td.fields) {
                  os << indent << "  " << f.name;
                  if (f.type_idx != 0)
                     os << "(" << wit_type_name(world, f.type_idx) << ")";
                  os << ",\n";
               }
               os << indent << "}\n";
               break;
            case wit_type_kind::enum_:
               os << indent << "enum " << td.name << " {\n";
               for (auto& f : td.fields)
                  os << indent << "  " << f.name << ",\n";
               os << indent << "}\n";
               break;
            case wit_type_kind::flags_:
               os << indent << "flags " << td.name << " {\n";
               for (auto& f : td.fields)
                  os << indent << "  " << f.name << ",\n";
               os << indent << "}\n";
               break;
            default:
               break; // list/option/result/tuple are inline, not emitted as declarations
         }
      }

   } // namespace detail

   // =========================================================================
   // Public API
   // =========================================================================

   /// Generate a wit_world from PSIO_REFLECT-annotated C++ types.
   /// Exports are methods of the Exports type.
   /// Imports (if provided) are methods of the Imports type.
   template<typename Exports, typename Imports = void>
   wit_world generate_wit(const std::string& world_name = "") {
      detail::wit_gen_ctx ctx;
      ctx.world.name = world_name.empty() ? std::string(reflect<Exports>::name) : world_name;

      // Generate exports
      wit_interface exp_iface;
      exp_iface.name = "";
      ctx.generate_functions<Exports>(exp_iface);
      if (!exp_iface.func_idxs.empty())
         ctx.world.exports.push_back(std::move(exp_iface));

      // Generate imports
      if constexpr (!std::is_void_v<Imports>) {
         wit_interface imp_iface;
         imp_iface.name = "";
         ctx.generate_functions<Imports>(imp_iface);
         if (!imp_iface.func_idxs.empty())
            ctx.world.imports.push_back(std::move(imp_iface));
      }

      return ctx.world;
   }

   /// Generate WIT text from a wit_world.
   inline std::string wit_to_text(const wit_world& world) {
      std::ostringstream os;

      // Emit named types
      for (auto& td : world.types) {
         if (!td.name.empty()) {
            detail::wit_emit_type(os, world, td, "  ");
         }
      }

      os << "world " << (world.name.empty() ? "unnamed" : world.name) << " {\n";

      // Emit types used by this world
      for (auto& td : world.types) {
         if (!td.name.empty()) {
            detail::wit_emit_type(os, world, td, "  ");
         }
      }

      // Emit imports
      for (auto& iface : world.imports) {
         for (auto func_idx : iface.func_idxs) {
            if (func_idx < world.funcs.size()) {
               os << "  import ";
               detail::wit_emit_func(os, world, world.funcs[func_idx], "");
            }
         }
      }

      // Emit exports
      for (auto& iface : world.exports) {
         for (auto func_idx : iface.func_idxs) {
            if (func_idx < world.funcs.size()) {
               os << "  export ";
               detail::wit_emit_func(os, world, world.funcs[func_idx], "");
            }
         }
      }

      os << "}\n";
      return os.str();
   }

   /// Generate WIT text directly from PSIO_REFLECT-annotated C++ types.
   template<typename Exports, typename Imports = void>
   std::string generate_wit_text(const std::string& world_name = "") {
      auto world = generate_wit<Exports, Imports>(world_name);
      return wit_to_text(world);
   }

} // namespace psio
