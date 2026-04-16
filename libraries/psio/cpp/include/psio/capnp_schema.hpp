// capnp_schema.hpp — Generate Cap'n Proto .capnp schema text from PSIO_REFLECT'd C++ types
//
// capnp_schema<T>() returns a std::string containing valid .capnp IDL text
// whose wire layout matches capnp_layout<T> (and thus capnp_pack/capnp_view).
//
// Supported types:
//   - Scalars: bool, int8-64, uint8-64, float32/64
//   - Text (std::string), Data (std::vector<uint8_t>)
//   - Nested reflected structs
//   - List(T) via std::vector<T>
//   - Unions via std::variant (capnp unnamed union)
//   - Enums via PSIO_REFLECT_ENUM
//   - Void via std::monostate
//   - Default values from C++ member initializers

#pragma once

#include <psio/capnp_view.hpp>

#include <cstdint>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>

namespace psio
{
   namespace capnp_detail
   {
      // ── Deterministic schema ID from type name ────────────────────────────
      //
      // Cap'n Proto requires a unique 64-bit ID per schema file/struct.
      // We use FNV-1a of the type name, with the top bit set (capnp IDs must
      // have bit 63 set to distinguish from reserved IDs).

      inline uint64_t capnp_schema_id(std::string_view name)
      {
         uint64_t h = 14695981039346656037ULL;
         for (char c : name)
         {
            h ^= static_cast<uint8_t>(c);
            h *= 1099511628211ULL;
         }
         return h | (1ULL << 63);  // ensure top bit is set
      }

      // ── Format a uint64_t as 0xHEX ────────────────────────────────────────

      inline std::string hex_id(uint64_t id)
      {
         char buf[32];
         std::snprintf(buf, sizeof(buf), "0x%016llx",
                       static_cast<unsigned long long>(id));
         return buf;
      }

      // ── Convert snake_case to camelCase ─────────────────────────────────
      //
      // Cap'n Proto requires identifiers in camelCase (no underscores).

      inline std::string to_camel_case(std::string_view name)
      {
         std::string result;
         result.reserve(name.size());
         bool next_upper = false;
         for (size_t i = 0; i < name.size(); ++i)
         {
            if (name[i] == '_')
            {
               next_upper = true;
            }
            else
            {
               if (next_upper && !result.empty())
               {
                  result += static_cast<char>(
                      (name[i] >= 'a' && name[i] <= 'z') ? name[i] - 'a' + 'A' : name[i]);
               }
               else
               {
                  result += name[i];
               }
               next_upper = false;
            }
         }
         return result;
      }

      // ── Map C++ type to capnp type name ───────────────────────────────────

      template <typename T>
      std::string capnp_type_name();

      template <>
      inline std::string capnp_type_name<bool>()
      {
         return "Bool";
      }
      template <>
      inline std::string capnp_type_name<int8_t>()
      {
         return "Int8";
      }
      template <>
      inline std::string capnp_type_name<int16_t>()
      {
         return "Int16";
      }
      template <>
      inline std::string capnp_type_name<int32_t>()
      {
         return "Int32";
      }
      template <>
      inline std::string capnp_type_name<int64_t>()
      {
         return "Int64";
      }
      template <>
      inline std::string capnp_type_name<uint8_t>()
      {
         return "UInt8";
      }
      template <>
      inline std::string capnp_type_name<uint16_t>()
      {
         return "UInt16";
      }
      template <>
      inline std::string capnp_type_name<uint32_t>()
      {
         return "UInt32";
      }
      template <>
      inline std::string capnp_type_name<uint64_t>()
      {
         return "UInt64";
      }
      template <>
      inline std::string capnp_type_name<float>()
      {
         return "Float32";
      }
      template <>
      inline std::string capnp_type_name<double>()
      {
         return "Float64";
      }
      template <>
      inline std::string capnp_type_name<std::string>()
      {
         return "Text";
      }
      template <>
      inline std::string capnp_type_name<std::monostate>()
      {
         return "Void";
      }

      // vector<uint8_t> → Data, vector<T> → List(T)
      template <typename T>
         requires is_vector<T>::value
      std::string capnp_type_name_vec()
      {
         using E = typename is_vector<T>::element_type;
         if constexpr (std::is_same_v<E, uint8_t>)
            return "Data";
         else
            return "List(" + capnp_type_name<E>() + ")";
      }

      // Enum → underlying capnp enum name
      template <typename T>
         requires std::is_enum_v<T>
      std::string capnp_type_name_enum()
      {
         return std::string(reflect<T>::name.c_str());
      }

      // Reflected struct → struct name
      template <typename T>
         requires(Reflected<T> && !std::is_enum_v<T> && !std::is_arithmetic_v<T>)
      std::string capnp_type_name_struct()
      {
         return std::string(reflect<T>::name.c_str());
      }

      // General dispatcher
      template <typename T>
      std::string capnp_type_name()
      {
         if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, int8_t> ||
                       std::is_same_v<T, int16_t> || std::is_same_v<T, int32_t> ||
                       std::is_same_v<T, int64_t> || std::is_same_v<T, uint8_t> ||
                       std::is_same_v<T, uint16_t> || std::is_same_v<T, uint32_t> ||
                       std::is_same_v<T, uint64_t> || std::is_same_v<T, float> ||
                       std::is_same_v<T, double> || std::is_same_v<T, std::string> ||
                       std::is_same_v<T, std::monostate>)
         {
            // Use explicit specializations above
            if constexpr (std::is_same_v<T, bool>)
               return "Bool";
            else if constexpr (std::is_same_v<T, int8_t>)
               return "Int8";
            else if constexpr (std::is_same_v<T, int16_t>)
               return "Int16";
            else if constexpr (std::is_same_v<T, int32_t>)
               return "Int32";
            else if constexpr (std::is_same_v<T, int64_t>)
               return "Int64";
            else if constexpr (std::is_same_v<T, uint8_t>)
               return "UInt8";
            else if constexpr (std::is_same_v<T, uint16_t>)
               return "UInt16";
            else if constexpr (std::is_same_v<T, uint32_t>)
               return "UInt32";
            else if constexpr (std::is_same_v<T, uint64_t>)
               return "UInt64";
            else if constexpr (std::is_same_v<T, float>)
               return "Float32";
            else if constexpr (std::is_same_v<T, double>)
               return "Float64";
            else if constexpr (std::is_same_v<T, std::string>)
               return "Text";
            else
               return "Void";
         }
         else if constexpr (is_vector<T>::value)
            return capnp_type_name_vec<T>();
         else if constexpr (std::is_enum_v<T>)
            return capnp_type_name_enum<T>();
         else if constexpr (Reflected<T>)
            return capnp_type_name_struct<T>();
         else
            static_assert(!sizeof(T*), "unsupported type for capnp schema generation");
      }

      // ── Format a default value for capnp schema ───────────────────────────

      template <typename T>
      std::string capnp_default_str(T val)
      {
         if constexpr (std::is_same_v<T, bool>)
         {
            return val ? "true" : "false";
         }
         else if constexpr (std::is_floating_point_v<T>)
         {
            std::ostringstream oss;
            oss << val;
            return oss.str();
         }
         else if constexpr (std::is_signed_v<T>)
         {
            return std::to_string(val);
         }
         else if constexpr (std::is_unsigned_v<T>)
         {
            return std::to_string(val);
         }
         else if constexpr (std::is_enum_v<T>)
         {
            using U = std::underlying_type_t<T>;
            // Try to get the enum label via reflection
            if constexpr (Reflected<T>)
            {
               auto label = reflect<T>::to_string(val);
               if (label)
                  return std::string(label);
            }
            return std::to_string(static_cast<U>(val));
         }
         else
         {
            return {};
         }
      }

      // ── Collect dependent types (nested structs, enums) ───────────────────

      template <typename T>
      void collect_deps(std::set<std::string>& seen, std::vector<std::string>& out);

      // Forward declare the struct schema emitter for recursion
      template <typename T>
         requires(Reflected<T> && reflect<T>::is_struct)
      std::string emit_struct_schema(std::set<std::string>& seen,
                                     std::vector<std::string>& deps);

      // Emit enum schema
      template <typename T>
         requires(Reflected<T> && std::is_enum_v<T>)
      std::string emit_enum_schema()
      {
         std::string name(reflect<T>::name.c_str());
         std::string result;
         result += "enum " + name + " @" + hex_id(capnp_schema_id(name)) + " {\n";
         for (size_t i = 0; i < reflect<T>::count; ++i)
         {
            // capnp enum values use camelCase by convention, but we use
            // the exact C++ label to maintain round-trip fidelity
            result += "  ";
            // Lowercase the first character for capnp style
            std::string label = reflect<T>::labels[i];
            if (!label.empty() && label[0] >= 'A' && label[0] <= 'Z')
               label[0] = label[0] - 'A' + 'a';
            result += label + " @" + std::to_string(i) + ";\n";
         }
         result += "}\n";
         return result;
      }

      // Collect type dependencies for a single field type
      template <typename F>
      void collect_field_deps(std::set<std::string>& seen, std::vector<std::string>& deps)
      {
         if constexpr (is_vector<F>::value)
         {
            using E = typename is_vector<F>::element_type;
            collect_field_deps<E>(seen, deps);
         }
         else if constexpr (std::is_enum_v<F> && Reflected<F>)
         {
            std::string name(reflect<F>::name.c_str());
            if (seen.insert(name).second)
               deps.push_back(emit_enum_schema<F>());
         }
         else if constexpr (Reflected<F> && !std::is_enum_v<F> && !std::is_arithmetic_v<F>)
         {
            std::string name(reflect<F>::name.c_str());
            if (seen.find(name) == seen.end())
               deps.push_back(emit_struct_schema<F>(seen, deps));
         }
      }

      // Collect deps from a variant's alternatives
      template <typename V, size_t... Js>
      void collect_variant_deps(std::set<std::string>& seen,
                                std::vector<std::string>& deps,
                                std::index_sequence<Js...>)
      {
         (collect_field_deps<std::variant_alternative_t<Js, V>>(seen, deps), ...);
      }

      // ── Emit struct schema ────────────────────────────────────────────────

      template <typename T>
         requires(Reflected<T> && reflect<T>::is_struct)
      std::string emit_struct_schema(std::set<std::string>& seen,
                                     std::vector<std::string>& deps)
      {
         using R      = reflect<T>;
         using layout = capnp_layout<T>;

         std::string name(R::name.c_str());
         seen.insert(name);

         // First collect dependencies from all fields
         apply_members(
             (typename R::data_members*)nullptr,
             [&]<typename... Ms>(Ms... members)
             {
                size_t i = 0;
                (
                    [&]
                    {
                       using F = std::remove_cvref_t<decltype(std::declval<T>().*members)>;
                       if constexpr (is_variant_type<F>::value)
                       {
                          collect_variant_deps<F>(
                              seen, deps,
                              std::make_index_sequence<std::variant_size_v<F>>{});
                       }
                       else
                       {
                          collect_field_deps<F>(seen, deps);
                       }
                       ++i;
                    }(),
                    ...);
             });

         // Now emit this struct
         std::string result;
         result += "struct " + name + " @" + hex_id(capnp_schema_id(name)) + " {\n";

         // Track the ordinal index (capnp @N annotation).
         // Non-variant fields use one ordinal each.
         // Variant fields expand to N ordinals (one per alternative) plus
         // a discriminant ordinal allocated after all ordinals.
         //
         // We need two passes: first count total ordinals from non-variant
         // and variant-alternative fields, then emit the discriminant ordinals.

         // Compute ordinal assignments matching capnp_layout's slot allocation order.
         // capnp_layout processes fields in PSIO_REFLECT order, expanding variants
         // inline, then appends discriminant ordinals.

         struct ordinal_info
         {
            bool   is_variant        = false;
            size_t first_alt_ordinal = 0;  // for variants: ordinal of first alternative
            size_t alt_count         = 0;
            size_t disc_ordinal      = 0;  // discriminant ordinal (for variants)
            size_t field_ordinal     = 0;  // for non-variant fields
         };

         // Count ordinals
         size_t ordinals[64]  = {};
         bool   is_var[64]    = {};
         size_t alt_counts[64] = {};
         size_t num_fields     = 0;

         apply_members(
             (typename R::data_members*)nullptr,
             [&]<typename... Ms>(Ms... members)
             {
                size_t i = 0;
                (
                    [&]
                    {
                       using F = std::remove_cvref_t<decltype(std::declval<T>().*members)>;
                       if constexpr (is_variant_type<F>::value)
                       {
                          is_var[i]     = true;
                          alt_counts[i] = std::variant_size_v<F>;
                       }
                       ++i;
                    }(),
                    ...);
                num_fields = sizeof...(Ms);
             });

         // Assign ordinals matching capnp_layout order:
         // First pass: non-variant fields and variant alternatives
         size_t next_ordinal = 0;
         for (size_t i = 0; i < num_fields; ++i)
         {
            if (is_var[i])
            {
               ordinals[i] = next_ordinal;
               next_ordinal += alt_counts[i];
            }
            else
            {
               ordinals[i]  = next_ordinal;
               next_ordinal += 1;
            }
         }
         // Second pass: discriminant ordinals for variants
         size_t disc_ordinals[64] = {};
         for (size_t i = 0; i < num_fields; ++i)
         {
            if (is_var[i])
            {
               disc_ordinals[i] = next_ordinal++;
            }
         }

         // Now emit fields
         apply_members(
             (typename R::data_members*)nullptr,
             [&]<typename... Ms>(Ms... members)
             {
                size_t i = 0;
                (
                    [&]
                    {
                       using F =
                           std::remove_cvref_t<decltype(std::declval<T>().*members)>;
                       std::string field_name =
                           to_camel_case(R::data_member_names[i]);

                       if constexpr (is_variant_type<F>::value)
                       {
                          // Emit capnp unnamed union.
                          // The discriminant is implicit in capnp wire format
                          // (no explicit ordinal) -- capnp allocates a 16-bit
                          // slot after all ordinal fields automatically.
                          result += "  union {\n";

                          size_t alt_ord = ordinals[i];
                          [&]<size_t... Js>(std::index_sequence<Js...>)
                          {
                             (
                                 [&]
                                 {
                                    using A =
                                        std::variant_alternative_t<Js, F>;
                                    std::string alt_name =
                                        field_name + std::to_string(Js);
                                    std::string type_str =
                                        capnp_type_name<A>();
                                    result += "    " + alt_name + " @" +
                                              std::to_string(alt_ord + Js) +
                                              " :" + type_str + ";\n";
                                 }(),
                                 ...);
                          }(std::make_index_sequence<std::variant_size_v<F>>{});

                          result += "  }\n";
                       }
                       else
                       {
                          std::string type_str = capnp_type_name<F>();
                          result += "  " + field_name + " @" +
                                    std::to_string(ordinals[i]) + " :" + type_str;

                          // Emit default value if non-zero.
                          // Default-construct T and read the member directly.
                          if constexpr (is_data_type<F>())
                          {
                             T    obj{};
                             F    val = obj.*members;
                             bool has_default = false;
                             if constexpr (std::is_same_v<F, bool>)
                                has_default = val;
                             else if constexpr (std::is_floating_point_v<F>)
                                has_default = val != F(0);
                             else
                                has_default = val != F{};

                             if (has_default)
                                result += " = " + capnp_default_str(val);
                          }

                          result += ";\n";
                       }
                       ++i;
                    }(),
                    ...);
             });

         result += "}\n";
         return result;
      }
   }  // namespace capnp_detail

   // ── Public API ───────────────────────────────────────────────────────────

   /// Generate a complete .capnp schema file for type T and all its dependencies.
   template <typename T>
      requires(Reflected<T> && reflect<T>::is_struct)
   std::string capnp_schema()
   {
      std::set<std::string>    seen;
      std::vector<std::string> deps;

      std::string name(reflect<T>::name.c_str());
      uint64_t    file_id = capnp_detail::capnp_schema_id("file:" + name);

      std::string main_schema =
          capnp_detail::emit_struct_schema<T>(seen, deps);

      // Assemble: file ID, then dependencies, then main struct
      std::string result;
      result += "@" + capnp_detail::hex_id(file_id) + ";\n\n";

      for (auto& dep : deps)
      {
         result += dep;
         result += "\n";
      }

      result += main_schema;
      return result;
   }

}  // namespace psio
