#pragma once

// psio/wit_constexpr.hpp — Constexpr WIT text generator for embedding
// in WASM custom sections at compile time.
//
// Walks PSIO1_REFLECT / PSIO1_INTERFACE metadata to produce WIT text
// entirely at compile time — no heap, no typeid, no runtime computation.
// The output is suitable for embedding via:
//
//   __attribute__((section(".custom_section.component-type:NAME"), used))
//   static constexpr auto _wit = psio1::constexpr_wit::interface_text<greeter>();
//
// The host linker reads this section to discover type signatures for
// module-to-module wiring.

#include <psio1/reflect.hpp>
#include <psio1/structural.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace psio1 {

// Forward — needed for owned<string, wit> detection
struct wit;
template <typename T, typename Fmt>
class owned;

namespace constexpr_wit {

// ═════════════════════════════════════════════════════════════════════
// Constexpr buffer — fixed-capacity string builder for compile-time use
// ═════════════════════════════════════════════════════════════════════

struct buffer {
   static constexpr std::size_t MAX = 8192;
   char     data[MAX] = {};
   unsigned len       = 0;

   constexpr void put(char c) { data[len++] = c; }

   constexpr void append(std::string_view sv) {
      for (char c : sv) data[len++] = c;
   }

   constexpr void append(const char* s) {
      while (*s) data[len++] = *s++;
   }

   constexpr std::string_view view() const { return {data, len}; }
};

// ═════════════════════════════════════════════════════════════════════
// Type detection traits (constexpr-friendly, no typeid)
// ═════════════════════════════════════════════════════════════════════

namespace detail {
   template <typename T> struct is_vector : std::false_type {};
   template <typename E, typename A>
   struct is_vector<std::vector<E, A>> : std::true_type { using elem = E; };

   template <typename T> struct is_optional : std::false_type {};
   template <typename T>
   struct is_optional<std::optional<T>> : std::true_type { using elem = T; };

   template <typename T> struct is_span : std::false_type {};
   template <typename T, std::size_t N>
   struct is_span<std::span<T, N>> : std::true_type { using elem = T; };

   template <typename T> struct is_wit_owned_string : std::false_type {};
   template <>
   struct is_wit_owned_string<psio1::owned<std::string, psio1::wit>> : std::true_type {};

   template <typename T> struct is_wit_owned_vector : std::false_type {};
   template <typename E>
   struct is_wit_owned_vector<psio1::owned<std::vector<E>, psio1::wit>> : std::true_type {
      using elem = E;
   };
}

// ═════════════════════════════════════════════════════════════════════
// Name mangling: C++ identifier → WIT kebab-case
// ═════════════════════════════════════════════════════════════════════

constexpr bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
constexpr bool is_lower(char c) { return c >= 'a' && c <= 'z'; }
constexpr char to_lower(char c) { return is_upper(c) ? (char)(c + 32) : c; }

constexpr void emit_kebab(buffer& buf, std::string_view name) {
   for (unsigned i = 0; i < name.size(); ++i) {
      char c = name[i];
      if (c == '_') {
         buf.put('-');
      } else if (is_upper(c) && i > 0 && is_lower(name[i - 1])) {
         buf.put('-');
         buf.put(to_lower(c));
      } else {
         buf.put(to_lower(c));
      }
   }
}

// ═════════════════════════════════════════════════════════════════════
// emit_wit_type<T> — append the WIT type name for C++ type T
// ═════════════════════════════════════════════════════════════════════

template <typename T>
constexpr void emit_wit_type(buffer& buf) {
   using U = std::remove_cvref_t<T>;

   if constexpr (std::is_same_v<U, bool>)          buf.append("bool");
   else if constexpr (std::is_same_v<U, uint8_t>)  buf.append("u8");
   else if constexpr (std::is_same_v<U, int8_t>)   buf.append("s8");
   else if constexpr (std::is_same_v<U, uint16_t>) buf.append("u16");
   else if constexpr (std::is_same_v<U, int16_t>)  buf.append("s16");
   else if constexpr (std::is_same_v<U, uint32_t>) buf.append("u32");
   else if constexpr (std::is_same_v<U, int32_t>)  buf.append("s32");
   else if constexpr (std::is_same_v<U, uint64_t>) buf.append("u64");
   else if constexpr (std::is_same_v<U, int64_t>)  buf.append("s64");
   else if constexpr (std::is_same_v<U, float>)    buf.append("f32");
   else if constexpr (std::is_same_v<U, double>)   buf.append("f64");
   else if constexpr (std::is_same_v<U, std::string> ||
                      std::is_same_v<U, std::string_view> ||
                      detail::is_wit_owned_string<U>::value)
      buf.append("string");
   else if constexpr (detail::is_vector<U>::value) {
      buf.append("list<");
      emit_wit_type<typename detail::is_vector<U>::elem>(buf);
      buf.put('>');
   }
   else if constexpr (detail::is_wit_owned_vector<U>::value) {
      buf.append("list<");
      emit_wit_type<typename detail::is_wit_owned_vector<U>::elem>(buf);
      buf.put('>');
   }
   else if constexpr (detail::is_span<U>::value) {
      buf.append("list<");
      emit_wit_type<std::remove_const_t<typename detail::is_span<U>::elem>>(buf);
      buf.put('>');
   }
   else if constexpr (detail::is_optional<U>::value) {
      buf.append("option<");
      emit_wit_type<typename detail::is_optional<U>::elem>(buf);
      buf.put('>');
   }
   else if constexpr (Reflected<U>) {
      emit_kebab(buf, reflect<U>::name);
   }
   else {
      buf.append("/* unknown */");
   }
}

// ═════════════════════════════════════════════════════════════════════
// emit_record<T> — emit a WIT record definition for a Reflected type
// ═════════════════════════════════════════════════════════════════════

template <typename T>
constexpr const char* get_data_member_name(std::size_t i) {
   constexpr auto names = reflect<T>::data_member_names;
   return names[i];
}

template <typename T, std::size_t... Is>
constexpr void emit_record_fields(buffer& buf, std::index_sequence<Is...>) {
   ((
      buf.append("    "),
      emit_kebab(buf, get_data_member_name<T>(Is)),
      buf.append(": "),
      emit_wit_type<std::tuple_element_t<Is, struct_tuple_t<T>>>(buf),
      buf.append(",\n")
   ), ...);
}

template <typename T>
constexpr void emit_record(buffer& buf) {
   buf.append("  record ");
   emit_kebab(buf, reflect<T>::name);
   buf.append(" {\n");
   constexpr auto N = std::tuple_size_v<struct_tuple_t<T>>;
   emit_record_fields<T>(buf, std::make_index_sequence<N>{});
   buf.append("  }\n\n");
}

// ═════════════════════════════════════════════════════════════════════
// emit_function — emit a single WIT function signature
// ═════════════════════════════════════════════════════════════════════

// Decompose a free function pointer type
template <typename F> struct fn_decompose;
template <typename R, typename... Args>
struct fn_decompose<R(*)(Args...)> {
   using ret = R;
   static constexpr std::size_t arity = sizeof...(Args);
};

template <typename FnPtr, std::size_t... Is>
constexpr void emit_params(buffer& buf,
                           const std::initializer_list<const char*>& names,
                           std::index_sequence<Is...>) {
   using Decomp = fn_decompose<FnPtr>;
   // Extract the Args pack via a helper
   [&]<typename R, typename... Args>(R(*)(Args...)) {
      unsigned i = 0;
      auto name_it = names.begin();
      ((
         (i > 0 ? buf.append(", ") : (void)0),
         emit_kebab(buf, *name_it),
         buf.append(": "),
         emit_wit_type<std::remove_cvref_t<Args>>(buf),
         ++i,
         ++name_it
      ), ...);
   }(static_cast<FnPtr>(nullptr));
}

template <typename FnPtr>
constexpr void emit_return(buffer& buf) {
   using R = typename fn_decompose<FnPtr>::ret;
   if constexpr (!std::is_void_v<R>) {
      buf.append(" -> ");
      emit_wit_type<R>(buf);
   }
}

// ═════════════════════════════════════════════════════════════════════
// emit_interface_types — emit record definitions for types()
// ═════════════════════════════════════════════════════════════════════

template <typename TypesTuple, std::size_t... Is>
constexpr void emit_types_impl(buffer& buf, std::index_sequence<Is...>) {
   (emit_record<std::tuple_element_t<Is, TypesTuple>>(buf), ...);
}

template <typename TypesTuple>
constexpr void emit_types(buffer& buf) {
   if constexpr (std::tuple_size_v<TypesTuple> > 0)
      emit_types_impl<TypesTuple>(buf,
         std::make_index_sequence<std::tuple_size_v<TypesTuple>>{});
}

// ═════════════════════════════════════════════════════════════════════
// interface_text<Tag> — produce full WIT interface text at compile time
// ═════════════════════════════════════════════════════════════════════

template <typename Tag, std::size_t... Is>
constexpr void emit_functions(buffer& buf, std::index_sequence<Is...>) {
   using info = psio1::detail::interface_info<Tag>;
   using func_types = typename info::func_types;

   ((
      buf.append("  "),
      emit_kebab(buf, info::func_names[Is]),
      buf.append(": func("),
      emit_params<std::tuple_element_t<Is, func_types>>(
         buf, info::param_names[Is],
         std::make_index_sequence<
            fn_decompose<std::tuple_element_t<Is, func_types>>::arity>{}),
      buf.append(")"),
      emit_return<std::tuple_element_t<Is, func_types>>(buf),
      buf.append(";\n")
   ), ...);
}

// Magic prefix for locating WIT blobs in the WASM data section.
// pzam wit embed scans for this sequence and extracts what follows.
inline constexpr char MAGIC[] = "PSIO1_WIT\x01";
inline constexpr unsigned MAGIC_LEN = sizeof(MAGIC) - 1;  // exclude null

template <typename Tag>
consteval auto interface_text() {
   using info = psio1::detail::interface_info<Tag>;
   constexpr auto n_funcs = std::tuple_size_v<typename info::func_types>;

   buffer buf;
   buf.append("interface ");
   emit_kebab(buf, std::string_view{info::name});
   buf.append(" {\n");
   emit_types<typename info::types>(buf);
   emit_functions<Tag>(buf, std::make_index_sequence<n_funcs>{});
   buf.append("}\n");

   // Copy into a tight array (text only, no header)
   std::array<char, buffer::MAX> result{};
   for (unsigned i = 0; i < buf.len; ++i)
      result[i] = buf.data[i];
   return std::pair{result, buf.len};
}

// ═════════════════════════════════════════════════════════════════════
// interface_blob<Tag> — WIT text with PSIO1_WIT magic + length header
// for pzam wit embed to find in the data section.
// Layout: "PSIO1_WIT\x01" (9 bytes) + u32le length + WIT text
// ═════════════════════════════════════════════════════════════════════

template <typename Tag>
consteval unsigned blob_size() {
   return MAGIC_LEN + 4 + interface_text<Tag>().second;
}

template <typename Tag>
consteval auto interface_blob() {
   auto text = interface_text<Tag>();
   unsigned total = MAGIC_LEN + 4 + text.second;

   std::array<char, buffer::MAX> result{};
   unsigned pos = 0;

   // Magic
   for (unsigned i = 0; i < MAGIC_LEN; ++i)
      result[pos++] = MAGIC[i];

   // Length as u32 little-endian
   uint32_t len = text.second;
   result[pos++] = static_cast<char>(len & 0xff);
   result[pos++] = static_cast<char>((len >> 8) & 0xff);
   result[pos++] = static_cast<char>((len >> 16) & 0xff);
   result[pos++] = static_cast<char>((len >> 24) & 0xff);

   // WIT text
   for (unsigned i = 0; i < text.second; ++i)
      result[pos++] = text.first[i];

   return std::pair{result, total};
}

// ═════════════════════════════════════════════════════════════════════
// wit_size<Tag> / wit_array<Tag> — exact-fit blob for WASM data section.
// Contains PSIO1_WIT magic + u32le length + WIT text. pzam wit embed
// scans for the magic and promotes the text to a custom section.
// ═════════════════════════════════════════════════════════════════════

template <typename Tag>
consteval unsigned wit_size() {
   return blob_size<Tag>();
}

template <typename Tag>
consteval auto wit_array() {
   auto blob = interface_blob<Tag>();
   std::array<char, blob_size<Tag>()> result{};
   for (unsigned i = 0; i < result.size(); ++i)
      result[i] = blob.first[i];
   return result;
}

} // namespace constexpr_wit
} // namespace psio1
