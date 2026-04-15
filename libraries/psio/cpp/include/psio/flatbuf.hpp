// psio/flatbuf.hpp — Standalone FlatBuffer serializer/deserializer
//
// Zero-dependency FlatBuffer implementation driven by PSIO_REFLECT.
// Produces wire-compatible FlatBuffer format without requiring the
// FlatBuffers library.
//
// Usage:
//   struct Token { uint16_t kind; std::string text; };
//   PSIO_REFLECT(Token, kind, text)
//
//   // Pack (fast, no vtable dedup)
//   psio::fb_builder fbb;
//   fbb.pack(token);
//   // fbb.data(), fbb.size() → finished FlatBuffer
//
//   // Pack with vtable deduplication (smaller wire size)
//   psio::basic_fb_builder<psio::fb_dedup::on> fbb_dedup;
//   fbb_dedup.pack(order);
//
//   // Unpack
//   auto token2 = psio::fb_unpack<Token>(fbb.data());
//
//   // Zero-copy view (named field access via PSIO_REFLECT proxy)
//   auto v = psio::fb_view<Token>::from_buffer(fbb.data());
//   uint16_t kind        = v.kind();       // named access
//   std::string_view txt = v.text();       // zero-copy
//
//   auto o = psio::fb_view<Order>::from_buffer(buf);
//   auto cname = o.customer().name();      // nested views

#pragma once

#include <psio/reflect.hpp>
#include <psio/view.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <flat_map>
#include <flat_set>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace psio {

struct fb;  // Format tag for FlatBuffer wire format

template <typename T>
class fb_nested;
template <typename T>
class fb_mut;
template <typename T>
class fb_doc_ref;

namespace detail::fbs {

// ── Type classification ──────────────────────────────────────────────────

template <typename T>
struct is_optional : std::false_type
{
};
template <typename T>
struct is_optional<std::optional<T>> : std::true_type
{
};

template <typename T>
struct is_vector : std::false_type
{
};
template <typename T>
struct is_vector<std::vector<T>> : std::true_type
{
};

template <typename T>
struct is_array : std::false_type
{
};
template <typename T, size_t N>
struct is_array<std::array<T, N>> : std::true_type
{
   using value_type                  = T;
   static constexpr size_t arr_size = N;
};

template <typename T>
struct is_variant : std::false_type
{
};
template <typename... Ts>
struct is_variant<std::variant<Ts...>> : std::true_type
{
};

template <typename T>
struct is_fb_nested : std::false_type
{
};
template <typename T>
struct is_fb_nested<fb_nested<T>> : std::true_type
{
   using inner_type = T;
};

template <typename T>
struct is_flat_set : std::false_type
{
};
template <typename K, typename... Rest>
struct is_flat_set<std::flat_set<K, Rest...>> : std::true_type
{
   using key_type = K;
};
template <typename K, typename... Rest>
struct is_flat_set<std::set<K, Rest...>> : std::true_type
{
   using key_type = K;
};

template <typename T>
struct is_flat_map : std::false_type
{
};
template <typename K, typename V, typename... Rest>
struct is_flat_map<std::flat_map<K, V, Rest...>> : std::true_type
{
   using key_type    = K;
   using mapped_type = V;
};
template <typename K, typename V, typename... Rest>
struct is_flat_map<std::map<K, V, Rest...>> : std::true_type
{
   using key_type    = K;
   using mapped_type = V;
};

template <typename T, typename = void>
struct is_table : std::false_type
{
};
template <typename T>
struct is_table<T, std::void_t<typename psio::reflect<T>::data_members>>
    : std::bool_constant<!std::is_arithmetic_v<T> && !std::is_enum_v<T> &&
                          !std::is_same_v<T, std::string>>
{
};

// ── Struct detection (definitionWillNotChange + all-scalar fields) ───────

// A scalar type in FlatBuffer struct context: arithmetic, bool, or enum
template <typename T>
constexpr bool is_fb_scalar()
{
   return std::is_arithmetic_v<T> || std::is_enum_v<T>;
}

template <typename T>
constexpr bool is_fb_struct()
{
   if constexpr (!is_table<T>::value)
      return false;
   else if constexpr (!psio::reflect<T>::definitionWillNotChange)
      return false;
   else
      return psio::apply_members(
          (typename psio::reflect<T>::data_members*)nullptr,
          [](auto... members)
          {
             return (... &&
                     (is_fb_scalar<std::remove_cvref_t<
                          decltype(psio::result_of_member(members))>>() ||
                      is_array<std::remove_cvref_t<
                          decltype(psio::result_of_member(members))>>::value));
          });
}

// ── Vtable slot computation (variant fields consume 2 slots) ────────────
//
// FlatBuffer unions use two vtable entries: a uint8 type selector at slot S
// and the value offset at slot S+1.  Every std::variant field shifts all
// subsequent field slots by +1.

template <typename T>
constexpr auto build_slot_map()
{
   constexpr size_t N = psio::apply_members(
       (typename psio::reflect<T>::data_members*)nullptr,
       [](auto... M) { return sizeof...(M); });

   std::array<int, N> slots{};
   int                slot = 0;
   psio::apply_members(
       (typename psio::reflect<T>::data_members*)nullptr,
       [&](auto... members)
       {
          size_t idx = 0;
          (([&]
           {
              slots[idx++] = slot;
              using V      = std::remove_cvref_t<decltype(psio::result_of_member(members))>;
              if constexpr (is_variant<V>::value)
                 slot += 2;  // type byte + offset
              else
                 slot += 1;
           }()),
           ...);
       });
   return slots;
}

template <typename T>
constexpr int vt_slot(size_t member_idx)
{
   constexpr auto map = build_slot_map<T>();
   return map[member_idx];
}

template <typename T>
constexpr int total_vt_slots()
{
   constexpr size_t N = psio::apply_members(
       (typename psio::reflect<T>::data_members*)nullptr,
       [](auto... M) { return sizeof...(M); });
   if constexpr (N == 0)
      return 0;
   else
   {
      constexpr auto map = build_slot_map<T>();
      // Last field's slot + its width
      using Last = std::tuple_element_t<N - 1, struct_tuple_t<T>>;
      if constexpr (is_variant<Last>::value)
         return map[N - 1] + 2;
      else
         return map[N - 1] + 1;
   }
}

// ── Struct wire layout (computed at compile time) ────────────────────────

// Wire size of one element for struct layout (enums use underlying type size)
template <typename V>
constexpr size_t struct_elem_size()
{
   if constexpr (std::is_enum_v<V>)
      return sizeof(std::underlying_type_t<V>);
   else if constexpr (is_array<V>::value)
      return struct_elem_size<typename is_array<V>::value_type>() * is_array<V>::arr_size;
   else
      return sizeof(V);
}

template <typename V>
constexpr size_t struct_elem_align()
{
   if constexpr (std::is_enum_v<V>)
      return sizeof(std::underlying_type_t<V>);
   else if constexpr (is_array<V>::value)
      return struct_elem_align<typename is_array<V>::value_type>();
   else
      return sizeof(V);
}

template <typename T>
constexpr size_t struct_type_align()
{
   return psio::apply_members(
       (typename psio::reflect<T>::data_members*)nullptr,
       [](auto... members)
       {
          return std::max(
              {struct_elem_align<
                  std::remove_cvref_t<decltype(psio::result_of_member(members))>>()...});
       });
}

template <typename T>
constexpr size_t struct_type_size()
{
   size_t pos = 0;
   psio::apply_members(
       (typename psio::reflect<T>::data_members*)nullptr,
       [&pos](auto... members)
       {
          (([&pos]
           {
              using V  = std::remove_cvref_t<decltype(psio::result_of_member(members))>;
              size_t a = struct_elem_align<V>();
              pos      = (pos + a - 1) & ~(a - 1);
              pos += struct_elem_size<V>();
           }()),
           ...);
       });
   size_t a = struct_type_align<T>();
   return (pos + a - 1) & ~(a - 1);
}

// ── Read/write primitives (little-endian, unaligned-safe) ────────────────

template <typename T>
inline T read(const void* p)
{
   T v;
   std::memcpy(&v, p, sizeof(T));
   return v;
}

template <typename T>
inline void store(void* p, T v)
{
   std::memcpy(p, &v, sizeof(T));
}

// ── FNV-1a hash for vtable deduplication ─────────────────────────────────

inline uint64_t hash_vtable(const uint8_t* p, size_t n)
{
   uint64_t h = 14695981039346656037ULL;
   for (size_t i = 0; i < n; ++i)
   {
      h ^= p[i];
      h *= 1099511628211ULL;
   }
   return h;
}

// ── FlatBuffer navigation ────────────────────────────────────────────────

inline const uint8_t* get_root(const uint8_t* buf)
{
   return buf + read<uint32_t>(buf);
}

inline const uint8_t* get_vtable(const uint8_t* table)
{
   return table - read<int32_t>(table);
}

inline uint16_t field_offset(const uint8_t* vtable, uint16_t slot)
{
   return slot < read<uint16_t>(vtable) ? read<uint16_t>(vtable + slot) : 0;
}

inline const uint8_t* deref(const uint8_t* p)
{
   return p + read<uint32_t>(p);
}

inline std::string_view read_string(const uint8_t* p)
{
   uint32_t len = read<uint32_t>(p);
   return {reinterpret_cast<const char*>(p + 4), len};
}

// ── Struct serialization (inline, no vtable) ────────────────────────────

// Write/read a single scalar-or-enum element inline
template <typename V>
void write_struct_elem(uint8_t*& dest, size_t& pos, const V& field)
{
   if constexpr (std::is_enum_v<V>)
   {
      using U  = std::underlying_type_t<V>;
      size_t a = sizeof(U);
      pos      = (pos + a - 1) & ~(a - 1);
      store(dest + pos, static_cast<U>(field));
      pos += sizeof(U);
   }
   else if constexpr (is_array<V>::value)
   {
      using E  = typename is_array<V>::value_type;
      size_t a = struct_elem_align<E>();
      pos      = (pos + a - 1) & ~(a - 1);
      for (size_t i = 0; i < is_array<V>::arr_size; ++i)
         write_struct_elem(dest, pos, field[i]);
   }
   else
   {
      size_t a = sizeof(V);
      pos      = (pos + a - 1) & ~(a - 1);
      store(dest + pos, field);
      pos += sizeof(V);
   }
}

template <typename V>
void read_struct_elem(const uint8_t* src, size_t& pos, V& field)
{
   if constexpr (std::is_enum_v<V>)
   {
      using U  = std::underlying_type_t<V>;
      size_t a = sizeof(U);
      pos      = (pos + a - 1) & ~(a - 1);
      field    = static_cast<V>(read<U>(src + pos));
      pos += sizeof(U);
   }
   else if constexpr (is_array<V>::value)
   {
      using E  = typename is_array<V>::value_type;
      size_t a = struct_elem_align<E>();
      pos      = (pos + a - 1) & ~(a - 1);
      for (size_t i = 0; i < is_array<V>::arr_size; ++i)
         read_struct_elem(src, pos, field[i]);
   }
   else
   {
      size_t a = sizeof(V);
      pos      = (pos + a - 1) & ~(a - 1);
      field    = read<V>(src + pos);
      pos += sizeof(V);
   }
}

template <typename T>
void write_struct_bytes(uint8_t* dest, const T& value)
{
   size_t pos = 0;
   psio::for_each_member(&value, (typename psio::reflect<T>::data_members*)nullptr,
                         [&](const auto& field)
                         {
                            using V = std::remove_cvref_t<decltype(field)>;
                            write_struct_elem<V>(dest, pos, field);
                         });
}

template <typename T>
void read_struct_bytes(const uint8_t* src, T& out)
{
   size_t pos = 0;
   psio::for_each_member(&out, (typename psio::reflect<T>::data_members*)nullptr,
                         [&](auto& field)
                         {
                            using V = std::remove_cvref_t<decltype(field)>;
                            read_struct_elem<V>(src, pos, field);
                         });
}

// ── Unpack: read one field into a C++ member ─────────────────────────────
// `slot` is the vtable slot index (not the PSIO_REFLECT member index).
// For most fields slot==member_idx, but variant fields consume 2 slots.

// Forward-declare unpack_table for recursive calls
template <typename T>
void unpack_table(const uint8_t* table, const uint8_t* vt, T& out);

template <typename V>
void read_field(const uint8_t* table, const uint8_t* vt, int slot, V& out, const V& def = V{})
{
   uint16_t vt_off = static_cast<uint16_t>(4 + 2 * slot);
   uint16_t fo     = field_offset(vt, vt_off);

   if constexpr (std::is_same_v<V, bool>)
   {
      out = fo ? (read<uint8_t>(table + fo) != 0) : def;
   }
   else if constexpr (std::is_enum_v<V>)
   {
      using U = std::underlying_type_t<V>;
      out     = fo ? static_cast<V>(read<U>(table + fo)) : def;
   }
   else if constexpr (std::is_arithmetic_v<V>)
   {
      out = fo ? read<V>(table + fo) : def;
   }
   else if constexpr (std::is_same_v<V, std::string>)
   {
      if (fo)
      {
         auto sv = read_string(deref(table + fo));
         out.assign(sv.data(), sv.size());
      }
   }
   else if constexpr (is_optional<V>::value)
   {
      using Inner = typename V::value_type;
      if (fo)
      {
         if constexpr (std::is_same_v<Inner, bool>)
            out = read<uint8_t>(table + fo) != 0;
         else if constexpr (std::is_enum_v<Inner>)
            out = static_cast<Inner>(read<std::underlying_type_t<Inner>>(table + fo));
         else if constexpr (std::is_arithmetic_v<Inner>)
            out = read<Inner>(table + fo);
         else if constexpr (std::is_same_v<Inner, std::string>)
         {
            auto sv = read_string(deref(table + fo));
            out     = std::string(sv.data(), sv.size());
         }
      }
   }
   else if constexpr (is_array<V>::value)
   {
      // Fixed-length arrays in tables are stored as FlatBuffer vectors
      using E = typename is_array<V>::value_type;
      if (!fo)
         return;
      auto*    vp    = deref(table + fo);
      uint32_t count = read<uint32_t>(vp);
      auto*    elems = vp + 4;
      size_t   n     = std::min<size_t>(count, is_array<V>::arr_size);
      if constexpr (std::is_arithmetic_v<E> || std::is_enum_v<E>)
      {
         for (size_t j = 0; j < n; ++j)
            out[j] = read<E>(elems + j * sizeof(E));
      }
   }
   else if constexpr (is_variant<V>::value)
   {
      // Union: type byte at slot, value offset at slot+1
      uint16_t type_vt = vt_off;
      uint16_t val_vt  = static_cast<uint16_t>(4 + 2 * (slot + 1));
      uint16_t type_fo = field_offset(vt, type_vt);
      uint16_t val_fo  = field_offset(vt, val_vt);
      if (!type_fo || !val_fo)
         return;
      uint8_t type_idx = read<uint8_t>(table + type_fo);
      if (type_idx == 0)
         return;  // NONE
      auto* sub_table = deref(table + val_fo);
      auto* sub_vt    = get_vtable(sub_table);
      // Dispatch to the correct variant alternative (1-indexed)
      size_t alt = type_idx - 1;
      [&]<size_t... I>(std::index_sequence<I...>)
      {
         (([&]
          {
             if (I == alt)
             {
                using Alt = std::variant_alternative_t<I, V>;
                Alt val{};
                unpack_table(sub_table, sub_vt, val);
                out = std::move(val);
             }
          }()),
          ...);
      }(std::make_index_sequence<std::variant_size_v<V>>{});
   }
   else if constexpr (is_fb_nested<V>::value)
   {
      // Nested FlatBuffer: stored as [ubyte] vector, read raw bytes
      if (fo)
      {
         auto*    vp    = deref(table + fo);
         uint32_t count = read<uint32_t>(vp);
         auto*    elems = vp + 4;
         out = V(std::vector<uint8_t>(elems, elems + count));
      }
   }
   else if constexpr (is_flat_set<V>::value)
   {
      using K = typename is_flat_set<V>::key_type;
      if (!fo)
         return;
      auto*    vp    = deref(table + fo);
      uint32_t count = read<uint32_t>(vp);
      auto*    elems = vp + 4;
      for (uint32_t j = 0; j < count; ++j)
      {
         if constexpr (std::is_same_v<K, std::string>)
         {
            auto* entry = elems + j * 4;
            auto  sv    = read_string(entry + read<uint32_t>(entry));
            out.insert(std::string(sv.data(), sv.size()));
         }
         else if constexpr (std::is_enum_v<K>)
         {
            using U = std::underlying_type_t<K>;
            out.insert(static_cast<K>(read<U>(elems + j * sizeof(U))));
         }
         else if constexpr (std::is_arithmetic_v<K>)
            out.insert(read<K>(elems + j * sizeof(K)));
      }
   }
   else if constexpr (is_flat_map<V>::value)
   {
      using K  = typename is_flat_map<V>::key_type;
      using MV = typename is_flat_map<V>::mapped_type;
      if (!fo)
         return;
      auto*    vp    = deref(table + fo);
      uint32_t count = read<uint32_t>(vp);
      auto*    elems = vp + 4;
      for (uint32_t j = 0; j < count; ++j)
      {
         auto* entry     = elems + j * 4;
         auto* sub_table = entry + read<uint32_t>(entry);
         auto* sub_vt    = get_vtable(sub_table);
         // Read key (field 0, vtable offset 4)
         K        key{};
         uint16_t key_fo = field_offset(sub_vt, 4);
         if constexpr (std::is_same_v<K, std::string>)
         {
            if (key_fo)
            {
               auto sv = read_string(deref(sub_table + key_fo));
               key.assign(sv.data(), sv.size());
            }
         }
         else if constexpr (std::is_enum_v<K>)
         {
            using U = std::underlying_type_t<K>;
            key     = key_fo ? static_cast<K>(read<U>(sub_table + key_fo)) : K{};
         }
         else if constexpr (std::is_arithmetic_v<K>)
            key = key_fo ? read<K>(sub_table + key_fo) : K{};
         // Read value (field 1, vtable offset 6)
         MV       val{};
         uint16_t val_fo = field_offset(sub_vt, 6);
         if constexpr (std::is_same_v<MV, bool>)
            val = val_fo ? (read<uint8_t>(sub_table + val_fo) != 0) : MV{};
         else if constexpr (std::is_enum_v<MV>)
         {
            using U = std::underlying_type_t<MV>;
            val     = val_fo ? static_cast<MV>(read<U>(sub_table + val_fo)) : MV{};
         }
         else if constexpr (std::is_arithmetic_v<MV>)
            val = val_fo ? read<MV>(sub_table + val_fo) : MV{};
         else if constexpr (std::is_same_v<MV, std::string>)
         {
            if (val_fo)
            {
               auto sv = read_string(deref(sub_table + val_fo));
               val.assign(sv.data(), sv.size());
            }
         }
         else if constexpr (is_table<MV>::value)
         {
            if (val_fo)
            {
               auto* nested    = deref(sub_table + val_fo);
               auto* nested_vt = get_vtable(nested);
               unpack_table(nested, nested_vt, val);
            }
         }
         out.insert_or_assign(std::move(key), std::move(val));
      }
   }
   else if constexpr (is_vector<V>::value)
   {
      using E = typename V::value_type;
      if (!fo)
         return;
      auto*    vp    = deref(table + fo);
      uint32_t count = read<uint32_t>(vp);
      auto*    elems = vp + 4;
      out.resize(count);

      if constexpr (std::is_same_v<E, std::string>)
      {
         for (uint32_t j = 0; j < count; ++j)
         {
            auto* entry = elems + j * 4;
            auto  sv    = read_string(entry + read<uint32_t>(entry));
            out[j].assign(sv.data(), sv.size());
         }
      }
      else if constexpr (std::is_enum_v<E>)
      {
         using U = std::underlying_type_t<E>;
         for (uint32_t j = 0; j < count; ++j)
            out[j] = static_cast<E>(read<U>(elems + j * sizeof(U)));
      }
      else if constexpr (std::is_arithmetic_v<E>)
      {
         std::memcpy(out.data(), elems, count * sizeof(E));
      }
      else if constexpr (is_fb_struct<E>())
      {
         constexpr size_t esz = struct_type_size<E>();
         for (uint32_t j = 0; j < count; ++j)
            read_struct_bytes(elems + j * esz, out[j]);
      }
      else if constexpr (is_table<E>::value)
      {
         for (uint32_t j = 0; j < count; ++j)
         {
            auto* entry     = elems + j * 4;
            auto* sub_table = entry + read<uint32_t>(entry);
            auto* sub_vt    = get_vtable(sub_table);
            unpack_table(sub_table, sub_vt, out[j]);
         }
      }
   }
   else if constexpr (is_fb_struct<V>())
   {
      if (fo)
         read_struct_bytes(table + fo, out);
   }
   else if constexpr (is_table<V>::value)
   {
      if (fo)
      {
         auto* sub_table = deref(table + fo);
         auto* sub_vt    = get_vtable(sub_table);
         unpack_table(sub_table, sub_vt, out);
      }
   }
}

template <typename T>
void unpack_table(const uint8_t* table, const uint8_t* vt, T& out)
{
   constexpr auto slots = build_slot_map<T>();

   psio::apply_members(
       (typename reflect<T>::data_members*)nullptr,
       [&](auto... members) {
          auto ptrs = std::make_tuple(members...);
          const T defaults{};
          [&]<size_t... I>(std::index_sequence<I...>) {
             (read_field(table, vt, slots[I], out.*std::get<I>(ptrs),
                         defaults.*std::get<I>(ptrs)),
              ...);
          }(std::make_index_sequence<sizeof...(members)>{});
       });
}

// ── Default value helper ─────────────────────────────────────────────────
// Returns the Nth member's value from a default-constructed T{}.
// Used by view_field/read_field to return correct defaults for absent fields
// and by the builder to omit fields that match their default.

template <typename T, size_t N>
auto default_value()
{
   return psio::apply_members(
       (typename psio::reflect<T>::data_members*)nullptr,
       [](auto... members) {
          constexpr T              defaults{};
          auto                     ptrs = std::make_tuple(members...);
          return defaults.*std::get<N>(ptrs);
       });
}

// ── View: zero-copy field read ───────────────────────────────────────────
// Returns by value for scalars, string_view for strings, vec_view for
// vectors, view<T,fb> for nested tables.  Used by fb::field<T,N>().

template <typename T, size_t N>
auto view_field(const uint8_t* table)
{
   using F               = std::tuple_element_t<N, struct_tuple_t<T>>;
   constexpr int    slot = vt_slot<T>(N);
   constexpr auto   vt   = static_cast<uint16_t>(4 + 2 * slot);

   auto*    vtable = get_vtable(table);
   uint16_t fo     = field_offset(vtable, vt);

   if constexpr (std::is_same_v<F, bool>)
      return fo ? (read<uint8_t>(table + fo) != 0) : default_value<T, N>();
   else if constexpr (std::is_enum_v<F>)
      return fo ? static_cast<F>(read<std::underlying_type_t<F>>(table + fo))
                : default_value<T, N>();
   else if constexpr (std::is_arithmetic_v<F>)
      return fo ? read<F>(table + fo) : default_value<T, N>();
   else if constexpr (std::is_same_v<F, std::string>)
      return fo ? read_string(deref(table + fo)) : std::string_view{};
   else if constexpr (is_optional<F>::value)
   {
      using Inner = typename F::value_type;
      if constexpr (std::is_same_v<Inner, bool>)
         return fo ? std::optional<bool>(read<uint8_t>(table + fo) != 0)
                   : std::optional<bool>{};
      else if constexpr (std::is_enum_v<Inner>)
         return fo ? std::optional<Inner>(
                         static_cast<Inner>(read<std::underlying_type_t<Inner>>(table + fo)))
                   : std::optional<Inner>{};
      else if constexpr (std::is_arithmetic_v<Inner>)
         return fo ? std::optional<Inner>(read<Inner>(table + fo)) : std::optional<Inner>{};
      else if constexpr (std::is_same_v<Inner, std::string>)
         return fo ? read_string(deref(table + fo)) : std::string_view{};
      else
         static_assert(!sizeof(Inner*), "unsupported optional inner type");
   }
   else if constexpr (is_array<F>::value)
   {
      // Fixed-length array → read from vector data
      using E = typename is_array<F>::value_type;
      F result{};
      if (fo)
      {
         auto*    vp    = deref(table + fo);
         uint32_t count = read<uint32_t>(vp);
         auto*    elems = vp + 4;
         size_t   n     = std::min<size_t>(count, is_array<F>::arr_size);
         for (size_t j = 0; j < n; ++j)
            result[j] = read<E>(elems + j * sizeof(E));
      }
      return result;
   }
   else if constexpr (is_variant<F>::value)
   {
      // Union: type byte at slot, value offset at slot+1
      constexpr auto type_vt = static_cast<uint16_t>(4 + 2 * slot);
      constexpr auto val_vt  = static_cast<uint16_t>(4 + 2 * (slot + 1));
      uint16_t       type_fo = field_offset(vtable, type_vt);
      uint16_t       val_fo  = field_offset(vtable, val_vt);
      F              result;
      if (type_fo && val_fo)
      {
         uint8_t type_idx = read<uint8_t>(table + type_fo);
         if (type_idx > 0)
         {
            auto* sub_table = deref(table + val_fo);
            auto* sub_vt    = get_vtable(sub_table);
            [&]<size_t... I>(std::index_sequence<I...>)
            {
               (([&]
                {
                   if (I == type_idx - 1)
                   {
                      using Alt = std::variant_alternative_t<I, F>;
                      Alt val{};
                      unpack_table(sub_table, sub_vt, val);
                      result = std::move(val);
                   }
                }()),
                ...);
            }(std::make_index_sequence<std::variant_size_v<F>>{});
         }
      }
      return result;
   }
   else if constexpr (is_fb_nested<F>::value)
   {
      // Nested FlatBuffer: [ubyte] vector contains a complete FlatBuffer
      using Inner = typename is_fb_nested<F>::inner_type;
      if (fo)
      {
         auto*    vp    = deref(table + fo);
         uint32_t count = read<uint32_t>(vp);
         auto*    bytes = vp + 4;
         return view<Inner, fb>(get_root(bytes));
      }
      return view<Inner, fb>{};
   }
   else if constexpr (is_flat_set<F>::value)
   {
      using K = typename is_flat_set<F>::key_type;
      return fo ? set_view<K, fb>(deref(table + fo)) : set_view<K, fb>{};
   }
   else if constexpr (is_flat_map<F>::value)
   {
      using K  = typename is_flat_map<F>::key_type;
      using MV = typename is_flat_map<F>::mapped_type;
      return fo ? map_view<K, MV, fb>(deref(table + fo)) : map_view<K, MV, fb>{};
   }
   else if constexpr (is_vector<F>::value)
   {
      using E = typename F::value_type;
      return fo ? vec_view<E, fb>(deref(table + fo)) : vec_view<E, fb>{};
   }
   else if constexpr (is_fb_struct<F>())
   {
      F result{};
      if (fo)
         read_struct_bytes(table + fo, result);
      return result;
   }
   else if constexpr (is_table<F>::value)
      return fo ? view<F, fb>(deref(table + fo)) : view<F, fb>{};
   else
      static_assert(!sizeof(F*), "unsupported field type");
}

// ── Field alignment (for alignment-sorted table packing) ────────────────

template <typename V>
constexpr size_t field_align()
{
   if constexpr (std::is_same_v<V, bool>)
      return 1;
   else if constexpr (std::is_enum_v<V>)
      return sizeof(std::underlying_type_t<V>);
   else if constexpr (std::is_arithmetic_v<V>)
      return sizeof(V);
   else if constexpr (is_optional<V>::value)
   {
      using Inner = typename V::value_type;
      if constexpr (std::is_same_v<Inner, bool>)
         return 1;
      else if constexpr (std::is_enum_v<Inner>)
         return sizeof(std::underlying_type_t<Inner>);
      else if constexpr (std::is_arithmetic_v<Inner>)
         return sizeof(Inner);
      else
         return sizeof(uint32_t);
   }
   else if constexpr (is_variant<V>::value)
      return sizeof(uint32_t);  // union = type byte (align 1) + offset (align 4)
   else if constexpr (is_fb_nested<V>::value)
      return sizeof(uint32_t);  // offset to [ubyte] vector
   else if constexpr (is_fb_struct<V>())
      return struct_type_align<V>();
   else
      return sizeof(uint32_t);  // offsets (string, vector, table, array-as-vector)
}

}  // namespace detail::fbs

// ══════════════════════════════════════════════════════════════════════════
// fb_builder — back-to-front buffer builder for FlatBuffer wire format
//
// Template parameter controls vtable deduplication:
//   fb_builder                             — no dedup (fastest pack)
//   basic_fb_builder<fb_dedup::on>         — dedup (smallest wire size)
// ══════════════════════════════════════════════════════════════════════════

enum class fb_dedup : bool
{
   off = false,
   on  = true
};

template <fb_dedup Dedup = fb_dedup::off>
class basic_fb_builder
{
   uint8_t* buf_;
   size_t   cap_;
   size_t   head_;  // write cursor (starts at cap_, grows toward 0)
   size_t   min_align_;

   // Table construction state
   struct field_loc
   {
      uint32_t off;
      uint16_t vt;
   };
   static constexpr size_t kMaxFields = 64;
   field_loc               fields_[kMaxFields];
   size_t   nfields_;
   uint32_t tbl_start_;

   // Vtable dedup cache — hash + fixed array, no heap allocation
   struct empty_vt_cache
   {
      void clear() {}
   };
   struct vt_cache
   {
      struct entry
      {
         uint64_t hash;
         uint32_t offset;
      };
      static constexpr size_t kCapacity = 32;
      entry                   entries[kCapacity];
      size_t                  count = 0;

      void clear() { count = 0; }

      uint32_t find(const uint8_t* buf, size_t cap, const uint8_t* vt,
                    uint16_t vt_size, uint64_t h) const
      {
         for (size_t i = 0; i < count; ++i)
            if (entries[i].hash == h &&
                std::memcmp(buf + cap - entries[i].offset, vt, vt_size) == 0)
               return entries[i].offset;
         return 0;
      }

      void add(uint64_t h, uint32_t off)
      {
         if (count < kCapacity)
            entries[count++] = {h, off};
      }
   };
   [[no_unique_address]]
   std::conditional_t<Dedup == fb_dedup::on, vt_cache, empty_vt_cache> vt_cache_;

   // ── Buffer management ──────────────────────────────────────────────

   size_t sz() const { return cap_ - head_; }

   void grow(size_t needed)
   {
      size_t nc   = std::max(cap_ * 2, cap_ + needed);
      auto*  nb   = new uint8_t[nc];
      size_t tail = sz();
      std::memcpy(nb + nc - tail, buf_ + head_, tail);
      delete[] buf_;
      buf_  = nb;
      head_ = nc - tail;
      cap_  = nc;
   }

   uint8_t* alloc(size_t n)
   {
      if (n > head_)
         grow(n);
      head_ -= n;
      return buf_ + head_;
   }

   void zero_pad(size_t n) { std::memset(alloc(n), 0, n); }

   void track(size_t a)
   {
      if (a > min_align_)
         min_align_ = a;
   }

   void align(size_t a)
   {
      size_t p = (~sz() + 1) & (a - 1);
      if (p)
         zero_pad(p);
      track(a);
   }

   void pre_align(size_t len, size_t a)
   {
      if (!len)
         return;
      size_t p = (~(sz() + len) + 1) & (a - 1);
      if (p)
         zero_pad(p);
      track(a);
   }

   template <typename T>
   void push(T v)
   {
      auto* p = alloc(sizeof(T));
      std::memcpy(p, &v, sizeof(T));
   }

   // ── Sub-object creation ────────────────────────────────────────────

   uint32_t create_string(const char* s, size_t len)
   {
      pre_align(len + 1, sizeof(uint32_t));
      *alloc(1) = 0;
      std::memcpy(alloc(len), s, len);
      align(sizeof(uint32_t));
      push(static_cast<uint32_t>(len));
      return static_cast<uint32_t>(sz());
   }

   template <typename T>
   uint32_t create_vec_scalar(const T* data, size_t count)
   {
      size_t body = count * sizeof(T);
      pre_align(body, sizeof(uint32_t));
      pre_align(body, sizeof(T));
      if (body)
         std::memcpy(alloc(body), data, body);
      align(sizeof(uint32_t));
      push(static_cast<uint32_t>(count));
      return static_cast<uint32_t>(sz());
   }

   uint32_t create_vec_offsets(const uint32_t* offs, size_t count)
   {
      pre_align(count * sizeof(uint32_t), sizeof(uint32_t));
      for (size_t i = count; i > 0;)
      {
         --i;
         align(sizeof(uint32_t));
         push(static_cast<uint32_t>(sz()) - offs[i] + uint32_t(4));
      }
      align(sizeof(uint32_t));
      push(static_cast<uint32_t>(count));
      return static_cast<uint32_t>(sz());
   }

   uint32_t create_vec_strings(const std::vector<std::string>& strs)
   {
      std::vector<uint32_t> offs(strs.size());
      for (size_t i = 0; i < strs.size(); ++i)
         offs[i] = create_string(strs[i].data(), strs[i].size());
      return create_vec_offsets(offs.data(), offs.size());
   }

   // ── Table construction ─────────────────────────────────────────────

   void start_table()
   {
      nfields_   = 0;
      tbl_start_ = static_cast<uint32_t>(sz());
   }

   template <typename T>
   void add_scalar(uint16_t vt, T val, T def)
   {
      if (val == def)
         return;
      align(sizeof(T));
      push(val);
      fields_[nfields_++] = {static_cast<uint32_t>(sz()), vt};
   }

   template <typename T>
   void add_scalar_force(uint16_t vt, T val)
   {
      align(sizeof(T));
      push(val);
      fields_[nfields_++] = {static_cast<uint32_t>(sz()), vt};
   }

   void add_offset_field(uint16_t vt, uint32_t off)
   {
      if (!off)
         return;
      align(sizeof(uint32_t));
      push(static_cast<uint32_t>(sz()) - off + uint32_t(4));
      fields_[nfields_++] = {static_cast<uint32_t>(sz()), vt};
   }

   template <typename T>
   void add_struct_field(uint16_t vt, const T& value)
   {
      constexpr size_t ssz = detail::fbs::struct_type_size<T>();
      constexpr size_t sal = detail::fbs::struct_type_align<T>();
      align(sal);
      auto* dest = alloc(ssz);
      std::memset(dest, 0, ssz);
      detail::fbs::write_struct_bytes(dest, value);
      fields_[nfields_++] = {static_cast<uint32_t>(sz()), vt};
   }

   uint32_t end_table()
   {
      align(sizeof(int32_t));
      push(int32_t{0});
      uint32_t tbl_off = static_cast<uint32_t>(sz());

      // Compute vtable size
      uint16_t max_vt = 0;
      for (size_t i = 0; i < nfields_; ++i)
         if (fields_[i].vt > max_vt)
            max_vt = fields_[i].vt;
      uint16_t vt_size    = std::max<uint16_t>(static_cast<uint16_t>(max_vt + 2), 4);
      uint16_t tbl_obj_sz = static_cast<uint16_t>(tbl_off - tbl_start_);

      // Write vtable into the buffer
      auto* vt = alloc(vt_size);
      std::memset(vt, 0, vt_size);
      detail::fbs::store(vt, vt_size);
      detail::fbs::store(vt + 2, tbl_obj_sz);

      for (size_t i = 0; i < nfields_; ++i)
      {
         uint16_t fo = static_cast<uint16_t>(tbl_off - fields_[i].off);
         detail::fbs::store(vt + fields_[i].vt, fo);
      }

      uint32_t vt_off = static_cast<uint32_t>(sz());

      if constexpr (Dedup == fb_dedup::on)
      {
         uint64_t hash     = detail::fbs::hash_vtable(vt, vt_size);
         uint32_t existing = vt_cache_.find(buf_, cap_, vt, vt_size, hash);
         if (existing)
         {
            head_ += vt_size;  // discard duplicate
            vt_off = existing;
         }
         else
         {
            vt_cache_.add(hash, vt_off);
         }
      }

      // Patch soffset_t in table → vtable
      int32_t soff = static_cast<int32_t>(vt_off) - static_cast<int32_t>(tbl_off);
      detail::fbs::store(buf_ + cap_ - tbl_off, soff);

      return tbl_off;
   }

   void finish_buffer(uint32_t root)
   {
      pre_align(sizeof(uint32_t), min_align_);
      align(sizeof(uint32_t));
      push(static_cast<uint32_t>(sz()) - root + uint32_t(4));
   }

   void finish_buffer_with_id(uint32_t root, const char* file_id)
   {
      // file_identifier: 4 bytes right after the root offset
      pre_align(sizeof(uint32_t) + 4, min_align_);
      align(sizeof(uint32_t));
      // Write identifier (4 bytes) then root offset
      auto* id_ptr = alloc(4);
      std::memcpy(id_ptr, file_id, 4);
      push(static_cast<uint32_t>(sz()) - root + uint32_t(4));
   }

   void finish_size_prefixed(uint32_t root)
   {
      finish_buffer(root);
      // Prepend 4-byte size prefix
      auto total = static_cast<uint32_t>(sz());
      push(total);
   }

   void finish_size_prefixed_with_id(uint32_t root, const char* file_id)
   {
      finish_buffer_with_id(root, file_id);
      auto total = static_cast<uint32_t>(sz());
      push(total);
   }

   // ── Reflect-driven helpers ─────────────────────────────────────────

   /// Write a key-value pair as a 2-field FlatBuffer table.
   /// Field 0 = key (vtable offset 4), field 1 = value (vtable offset 6).
   template <typename K, typename V>
   uint32_t write_pair_table(const K& key, const V& value)
   {
      uint32_t key_off = pre_create<K>(key);
      uint32_t val_off = pre_create<V>(value);
      start_table();
      // Add in reverse alignment order (largest first) for minimal padding
      constexpr size_t ka = detail::fbs::field_align<K>();
      constexpr size_t va = detail::fbs::field_align<V>();
      auto add_key = [&]
      {
         if constexpr (std::is_same_v<K, bool>)
            add_scalar<uint8_t>(4, static_cast<uint8_t>(key), uint8_t(0));
         else if constexpr (std::is_enum_v<K>)
         {
            using U = std::underlying_type_t<K>;
            add_scalar<U>(4, static_cast<U>(key), U(0));
         }
         else if constexpr (std::is_arithmetic_v<K>)
            add_scalar<K>(4, key, K{});
         else if (key_off)
            add_offset_field(4, key_off);
      };
      auto add_val = [&]
      {
         if constexpr (std::is_same_v<V, bool>)
            add_scalar<uint8_t>(6, static_cast<uint8_t>(value), uint8_t(0));
         else if constexpr (std::is_enum_v<V>)
         {
            using U = std::underlying_type_t<V>;
            add_scalar<U>(6, static_cast<U>(value), U(0));
         }
         else if constexpr (std::is_arithmetic_v<V>)
            add_scalar<V>(6, value, V{});
         else if constexpr (detail::fbs::is_fb_struct<V>())
            add_struct_field(6, value);
         else if (val_off)
            add_offset_field(6, val_off);
      };
      if constexpr (ka < va)
      {
         add_key();
         add_val();
      }
      else
      {
         add_val();
         add_key();
      }
      return end_table();
   }

   template <typename V>
   uint32_t pre_create(const V& val)
   {
      if constexpr (std::is_same_v<V, bool> || std::is_arithmetic_v<V> || std::is_enum_v<V>)
         return 0;
      else if constexpr (std::is_same_v<V, std::string>)
         return create_string(val.data(), val.size());
      else if constexpr (detail::fbs::is_optional<V>::value)
      {
         if (val.has_value())
            return pre_create<typename V::value_type>(*val);
         return 0;
      }
      else if constexpr (detail::fbs::is_array<V>::value)
      {
         // Fixed-length arrays in tables → stored as FlatBuffer vectors
         using E              = typename detail::fbs::is_array<V>::value_type;
         constexpr size_t len = detail::fbs::is_array<V>::arr_size;
         if constexpr (std::is_arithmetic_v<E> || std::is_enum_v<E>)
            return create_vec_scalar(reinterpret_cast<const E*>(val.data()), len);
         else
            static_assert(!sizeof(E*), "unsupported array element type");
      }
      else if constexpr (detail::fbs::is_variant<V>::value)
      {
         // Union: pre-create the active alternative's table
         uint32_t result = 0;
         std::visit(
             [&](const auto& alt)
             {
                using Alt = std::remove_cvref_t<decltype(alt)>;
                if constexpr (detail::fbs::is_table<Alt>::value)
                   result = write_table(alt);
             },
             val);
         return result;
      }
      else if constexpr (detail::fbs::is_fb_nested<V>::value)
      {
         // Nested FlatBuffer: write as [ubyte] vector
         if (val.size() == 0)
            return 0;
         return create_vec_scalar(val.data(), val.size());
      }
      else if constexpr (detail::fbs::is_flat_set<V>::value)
      {
         using K = typename detail::fbs::is_flat_set<V>::key_type;
         if (val.empty())
            return 0;
         if constexpr (std::is_same_v<K, std::string>)
         {
            std::vector<std::string> keys(val.begin(), val.end());
            return create_vec_strings(keys);
         }
         else if constexpr (std::is_enum_v<K>)
         {
            using U = std::underlying_type_t<K>;
            std::vector<U> raw(val.size());
            size_t         i = 0;
            for (auto& k : val)
               raw[i++] = static_cast<U>(k);
            return create_vec_scalar(raw.data(), raw.size());
         }
         else if constexpr (std::is_arithmetic_v<K>)
         {
            std::vector<K> keys(val.begin(), val.end());
            return create_vec_scalar(keys.data(), keys.size());
         }
         else
            static_assert(!sizeof(K*), "unsupported flat_set key type");
      }
      else if constexpr (detail::fbs::is_flat_map<V>::value)
      {
         if (val.empty())
            return 0;
         // Each entry becomes a 2-field table {key, value}
         std::vector<uint32_t> offs(val.size());
         size_t                i = 0;
         for (const auto& entry : val)
            offs[i++] = write_pair_table(entry.first, entry.second);
         return create_vec_offsets(offs.data(), offs.size());
      }
      else if constexpr (detail::fbs::is_vector<V>::value)
      {
         using E = typename V::value_type;
         if (val.empty())
            return 0;
         if constexpr (std::is_same_v<E, std::string>)
            return create_vec_strings(val);
         else if constexpr (std::is_enum_v<E>)
         {
            // Enums stored as underlying type
            using U = std::underlying_type_t<E>;
            return create_vec_scalar(reinterpret_cast<const U*>(val.data()), val.size());
         }
         else if constexpr (std::is_arithmetic_v<E>)
            return create_vec_scalar(val.data(), val.size());
         else if constexpr (detail::fbs::is_fb_struct<E>())
         {
            constexpr size_t esz = detail::fbs::struct_type_size<E>();
            constexpr size_t eal = detail::fbs::struct_type_align<E>();
            size_t           body = val.size() * esz;
            pre_align(body, std::max(sizeof(uint32_t), eal));
            auto* dest = alloc(body);
            std::memset(dest, 0, body);
            for (size_t i = 0; i < val.size(); ++i)
               detail::fbs::write_struct_bytes(dest + i * esz, val[i]);
            align(sizeof(uint32_t));
            push(static_cast<uint32_t>(val.size()));
            return static_cast<uint32_t>(sz());
         }
         else if constexpr (detail::fbs::is_table<E>::value)
         {
            std::vector<uint32_t> offs(val.size());
            for (size_t i = 0; i < val.size(); ++i)
               offs[i] = write_table(val[i]);
            return create_vec_offsets(offs.data(), offs.size());
         }
         else
            static_assert(!sizeof(E*), "unsupported vector element type");
      }
      else if constexpr (detail::fbs::is_fb_struct<V>())
         return 0;  // structs are inline, no pre-creation
      else if constexpr (detail::fbs::is_table<V>::value)
         return write_table(val);
      else
         static_assert(!sizeof(V*), "unsupported type");
   }

   // `slot` is the vtable slot index (accounting for variant fields taking 2 slots)
   template <typename V>
   void add_field(int slot, const V& val, uint32_t off, const V& def)
   {
      auto vt = static_cast<uint16_t>(4 + 2 * slot);

      if constexpr (std::is_same_v<V, bool>)
         add_scalar<uint8_t>(vt, static_cast<uint8_t>(val), static_cast<uint8_t>(def));
      else if constexpr (std::is_enum_v<V>)
      {
         using U = std::underlying_type_t<V>;
         add_scalar<U>(vt, static_cast<U>(val), static_cast<U>(def));
      }
      else if constexpr (std::is_arithmetic_v<V>)
         add_scalar<V>(vt, val, def);
      else if constexpr (detail::fbs::is_fb_struct<V>())
         add_struct_field(vt, val);
      else if constexpr (detail::fbs::is_optional<V>::value)
      {
         using Inner = typename V::value_type;
         if (val.has_value())
         {
            if constexpr (std::is_same_v<Inner, bool>)
               add_scalar_force<uint8_t>(vt, static_cast<uint8_t>(*val));
            else if constexpr (std::is_enum_v<Inner>)
               add_scalar_force<std::underlying_type_t<Inner>>(
                   vt, static_cast<std::underlying_type_t<Inner>>(*val));
            else if constexpr (std::is_arithmetic_v<Inner>)
               add_scalar_force<Inner>(vt, *val);
            else if (off)
               add_offset_field(vt, off);
         }
      }
      else if constexpr (detail::fbs::is_variant<V>::value)
      {
         // Union: type byte at slot, value offset at slot+1
         auto val_vt = static_cast<uint16_t>(4 + 2 * (slot + 1));
         auto idx    = static_cast<uint8_t>(val.index() + 1);
         add_scalar<uint8_t>(vt, idx, uint8_t(0));
         if (off)
            add_offset_field(val_vt, off);
      }
      else
      {
         if (off)
            add_offset_field(vt, off);
      }
   }

   template <typename T>
   uint32_t write_table(const T& value)
   {
      constexpr size_t N = psio::apply_members(
          (typename reflect<T>::data_members*)nullptr,
          [](auto... M) { return sizeof...(M); });

      constexpr auto slot_map = detail::fbs::build_slot_map<T>();

      // Phase 1: pre-create sub-objects (strings, vectors, nested tables)
      uint32_t offs[N];
      int      oi = 0;
      psio::for_each_member(
          &value, (typename reflect<T>::data_members*)nullptr,
          [&](const auto& field) {
             offs[oi++] = pre_create<std::remove_cvref_t<decltype(field)>>(field);
          });

      // Phase 2: build table with fields sorted by alignment.
      // Smallest-aligned first (→ highest offsets in back-to-front buffer),
      // largest-aligned last (→ lowest offsets).  This matches the official
      // FlatBuffers packing strategy and minimizes inter-group padding.
      start_table();

      psio::apply_members(
          (typename reflect<T>::data_members*)nullptr,
          [&](auto... members) {
             auto ptrs = std::make_tuple(members...);
             auto iseq = std::make_index_sequence<N>{};

             const T defaults{};

             auto add_with_align = [&]<size_t Align, size_t... I>(
                                       std::integral_constant<size_t, Align>,
                                       std::index_sequence<I...>) {
                (([&] {
                   using V = std::remove_cvref_t<decltype(value.*std::get<I>(ptrs))>;
                   if constexpr (detail::fbs::field_align<V>() == Align)
                      add_field(slot_map[I], value.*std::get<I>(ptrs), offs[I],
                                defaults.*std::get<I>(ptrs));
                }()),
                 ...);
             };

             add_with_align(std::integral_constant<size_t, 1>{}, iseq);
             add_with_align(std::integral_constant<size_t, 2>{}, iseq);
             add_with_align(std::integral_constant<size_t, 4>{}, iseq);
             add_with_align(std::integral_constant<size_t, 8>{}, iseq);
          });

      return end_table();
   }

  public:
   explicit basic_fb_builder(size_t initial = 1024)
       : buf_(new uint8_t[initial]),
         cap_(initial),
         head_(initial),
         min_align_(1),
         nfields_(0),
         tbl_start_(0)
   {
   }

   ~basic_fb_builder() { delete[] buf_; }

   basic_fb_builder(const basic_fb_builder&)            = delete;
   basic_fb_builder& operator=(const basic_fb_builder&) = delete;

   /// Serialize a PSIO_REFLECT'd struct to FlatBuffer format.
   template <typename T>
   void pack(const T& value)
   {
      auto root = write_table(value);
      finish_buffer(root);
   }

   /// Pack with a 4-byte file identifier (e.g. "MYID").
   template <typename T>
   void pack(const T& value, const char* file_id)
   {
      auto root = write_table(value);
      finish_buffer_with_id(root, file_id);
   }

   /// Pack with a 4-byte size prefix (for streaming/framing).
   template <typename T>
   void pack_size_prefixed(const T& value)
   {
      auto root = write_table(value);
      finish_size_prefixed(root);
   }

   /// Pack with both size prefix and file identifier.
   template <typename T>
   void pack_size_prefixed(const T& value, const char* file_id)
   {
      auto root = write_table(value);
      finish_size_prefixed_with_id(root, file_id);
   }

   /// Reset the builder for reuse (keeps allocation).
   void clear()
   {
      head_      = cap_;
      min_align_ = 1;
      vt_cache_.clear();
   }

   const uint8_t* data() const { return buf_ + head_; }
   size_t         size() const { return sz(); }
};

/// Default builder (no vtable dedup — fastest pack).
using fb_builder = basic_fb_builder<>;

// ══════════════════════════════════════════════════════════════════════════
// fb_unpack — deserialize a FlatBuffer into a C++ struct
// ══════════════════════════════════════════════════════════════════════════

template <typename T>
T fb_unpack(const uint8_t* buf)
{
   auto* table  = detail::fbs::get_root(buf);
   auto* vtable = detail::fbs::get_vtable(table);
   T     result{};
   detail::fbs::unpack_table(table, vtable, result);
   return result;
}

/// Unpack a size-prefixed buffer (skip 4-byte length prefix).
template <typename T>
T fb_unpack_size_prefixed(const uint8_t* buf)
{
   return fb_unpack<T>(buf + 4);
}

/// Read the 4-byte file identifier from a buffer.
inline std::string_view fb_file_identifier(const uint8_t* buf)
{
   return {reinterpret_cast<const char*>(buf + 4), 4};
}

/// Check if buffer has the expected file identifier.
inline bool fb_has_identifier(const uint8_t* buf, const char* expected)
{
   return std::memcmp(buf + 4, expected, 4) == 0;
}

/// Get buffer size from a size-prefixed buffer.
inline uint32_t fb_size_prefixed_length(const uint8_t* buf)
{
   return detail::fbs::read<uint32_t>(buf);
}

// ══════════════════════════════════════════════════════════════════════════
// vec_view<E, fb> — zero-copy vector view (FlatBuffer format)
// ══════════════════════════════════════════════════════════════════════════

template <typename E>
class vec_view<E, fb>
{
   const uint8_t* data_ = nullptr;

  public:
   vec_view() = default;
   explicit vec_view(const uint8_t* p) : data_(p) {}
   explicit operator bool() const { return data_ != nullptr; }

   uint32_t size() const { return data_ ? detail::fbs::read<uint32_t>(data_) : 0; }
   bool     empty() const { return size() == 0; }

   auto operator[](uint32_t i) const
   {
      auto* elems = data_ + 4;
      if constexpr (std::is_same_v<E, std::string>)
      {
         auto* entry = elems + i * 4;
         return detail::fbs::read_string(entry + detail::fbs::read<uint32_t>(entry));
      }
      else if constexpr (detail::fbs::is_fb_struct<E>())
      {
         constexpr size_t esz = detail::fbs::struct_type_size<E>();
         E                result{};
         detail::fbs::read_struct_bytes(elems + i * esz, result);
         return result;
      }
      else if constexpr (detail::fbs::is_table<E>::value)
      {
         auto* entry = elems + i * 4;
         return view<E, fb>(entry + detail::fbs::read<uint32_t>(entry));
      }
      else if constexpr (std::is_enum_v<E>)
      {
         using U = std::underlying_type_t<E>;
         return static_cast<E>(detail::fbs::read<U>(elems + i * sizeof(U)));
      }
      else
      {
         static_assert(std::is_arithmetic_v<E>);
         return detail::fbs::read<E>(elems + i * sizeof(E));
      }
   }

   auto at(uint32_t i) const { return operator[](i); }
};

// ══════════════════════════════════════════════════════════════════════════
// set_view<K, fb> — zero-copy sorted set view with binary search
//
// Inherits sorted_set_algo for contains(), find(), lower_bound().
// No operator[] — sets are not indexed.
// ══════════════════════════════════════════════════════════════════════════

template <typename K>
class set_view<K, fb> : public sorted_set_algo<set_view<K, fb>>
{
   friend class sorted_set_algo<set_view<K, fb>>;

   const uint8_t* data_ = nullptr;

   auto read_key(uint32_t i) const
   {
      auto* elems = data_ + 4;
      if constexpr (std::is_same_v<K, std::string>)
      {
         auto* entry = elems + i * 4;
         return detail::fbs::read_string(entry + detail::fbs::read<uint32_t>(entry));
      }
      else if constexpr (std::is_enum_v<K>)
      {
         using U = std::underlying_type_t<K>;
         return static_cast<K>(detail::fbs::read<U>(elems + i * sizeof(U)));
      }
      else
      {
         static_assert(std::is_arithmetic_v<K>);
         return detail::fbs::read<K>(elems + i * sizeof(K));
      }
   }

  public:
   set_view() = default;
   explicit set_view(const uint8_t* p) : data_(p) {}
   explicit operator bool() const { return data_ != nullptr; }

   uint32_t size() const { return data_ ? detail::fbs::read<uint32_t>(data_) : 0; }
   bool     empty() const { return size() == 0; }
   // contains(), find(), lower_bound() inherited from sorted_set_algo
};

// ══════════════════════════════════════════════════════════════════════════
// map_view<K, V, fb> — zero-copy sorted map view with O(log n) key lookup
//
// Inherits sorted_map_algo for find_index(), contains(), lower_bound().
// Wire format: sorted vector of 2-field entry tables {key, value}.
// ══════════════════════════════════════════════════════════════════════════

template <typename K, typename V>
class map_view<K, V, fb> : public sorted_map_algo<map_view<K, V, fb>>
{
   friend class sorted_map_algo<map_view<K, V, fb>>;

   const uint8_t* data_ = nullptr;

   const uint8_t* entry_table(uint32_t i) const
   {
      auto* elems = data_ + 4;
      auto* slot  = elems + i * 4;
      return slot + detail::fbs::read<uint32_t>(slot);
   }

   auto read_key(uint32_t i) const
   {
      auto*    t  = entry_table(i);
      auto*    vt = detail::fbs::get_vtable(t);
      uint16_t fo = detail::fbs::field_offset(vt, 4);
      if constexpr (std::is_same_v<K, std::string>)
         return fo ? detail::fbs::read_string(detail::fbs::deref(t + fo))
                   : std::string_view{};
      else if constexpr (std::is_enum_v<K>)
      {
         using U = std::underlying_type_t<K>;
         return fo ? static_cast<K>(detail::fbs::read<U>(t + fo)) : K{};
      }
      else
      {
         static_assert(std::is_arithmetic_v<K>);
         return fo ? detail::fbs::read<K>(t + fo) : K{};
      }
   }

   auto read_value(uint32_t i) const
   {
      auto*    t  = entry_table(i);
      auto*    vt = detail::fbs::get_vtable(t);
      uint16_t fo = detail::fbs::field_offset(vt, 6);
      if constexpr (std::is_same_v<V, bool>)
         return fo ? (detail::fbs::read<uint8_t>(t + fo) != 0) : false;
      else if constexpr (std::is_enum_v<V>)
      {
         using U = std::underlying_type_t<V>;
         return fo ? static_cast<V>(detail::fbs::read<U>(t + fo)) : V{};
      }
      else if constexpr (std::is_arithmetic_v<V>)
         return fo ? detail::fbs::read<V>(t + fo) : V{};
      else if constexpr (std::is_same_v<V, std::string>)
         return fo ? detail::fbs::read_string(detail::fbs::deref(t + fo))
                   : std::string_view{};
      else if constexpr (detail::fbs::is_table<V>::value)
         return fo ? view<V, fb>(detail::fbs::deref(t + fo)) : view<V, fb>{};
      else
      {
         static_assert(!sizeof(V*), "unsupported map value type for view");
         return V{};
      }
   }

  public:
   map_view() = default;
   explicit map_view(const uint8_t* p) : data_(p) {}
   explicit operator bool() const { return data_ != nullptr; }

   uint32_t size() const { return data_ ? detail::fbs::read<uint32_t>(data_) : 0; }
   bool     empty() const { return size() == 0; }

   /// Proxy for a single key-value entry.
   struct entry_view
   {
      const map_view* map_;
      uint32_t        idx_;
      auto            key() const { return map_->read_key(idx_); }
      auto            value() const { return map_->read_value(idx_); }
   };

   // contains(), find_index(), lower_bound() inherited from sorted_map_algo

   /// Find by key — returns entry_view.  Check `idx_ < map.size()` for found.
   entry_view find(const auto& key) const
   {
      return {this, sorted_map_algo<map_view>::find_index(key)};
   }

   /// Value lookup by key with fallback
   auto value_or(const auto& key, const auto& fallback) const
   {
      uint32_t idx = sorted_map_algo<map_view>::find_index(key);
      if (idx < size())
         return read_value(idx);
      return decltype(read_value(0))(fallback);
   }
};

// ── Backward-compatible aliases ─────────────────────────────────────────

template <typename E>
using fb_vec = vec_view<E, fb>;
template <typename K>
using fb_sorted_vec = set_view<K, fb>;
template <typename K, typename V>
using fb_sorted_map = map_view<K, V, fb>;

// ══════════════════════════════════════════════════════════════════════════
// struct fb — FlatBuffer format tag for view<T, fb>
//
// Satisfies the Format concept: ptr_t, root<T>(), field<T,N>().
// view<T, fb> provides named field access identical to the old fb_view<T>.
// ══════════════════════════════════════════════════════════════════════════

struct fb
{
   using ptr_t = const uint8_t*;

   template <typename T>
   static ptr_t root(const void* buf)
   {
      return detail::fbs::get_root(static_cast<const uint8_t*>(buf));
   }

   template <typename T, size_t N>
   static auto field(ptr_t table)
   {
      return detail::fbs::view_field<T, N>(table);
   }
};

/// fb_view<T> — alias for view<T, fb> (backward-compatible name)
template <typename T>
using fb_view = view<T, fb>;

// ══════════════════════════════════════════════════════════════════════════
// fb_mut_ref — assignable proxy reference for unaligned in-buffer scalars
//
// FlatBuffer fields may not be naturally aligned, so we can't return a raw
// T&.  This thin wrapper uses memcpy for loads and stores, and implicitly
// converts to T for reads, while operator= writes back to the buffer.
// ══════════════════════════════════════════════════════════════════════════

template <typename T>
class fb_mut_ref
{
   uint8_t* ptr_;

  public:
   explicit fb_mut_ref(uint8_t* p) : ptr_(p) {}

   explicit operator bool() const { return ptr_ != nullptr; }

   operator T() const
   {
      if (!ptr_)
         return T{};
      T v;
      std::memcpy(&v, ptr_, sizeof(T));
      return v;
   }

   fb_mut_ref& operator=(T v)
   {
      if (ptr_)
         std::memcpy(ptr_, &v, sizeof(T));
      return *this;
   }
};

// Bool specialization — stores as uint8_t but exposes bool interface
template <>
class fb_mut_ref<bool>
{
   uint8_t* ptr_;

  public:
   explicit fb_mut_ref(uint8_t* p) : ptr_(p) {}

   operator bool() const { return ptr_ ? (*ptr_ != 0) : false; }

   fb_mut_ref& operator=(bool v)
   {
      if (ptr_)
         *ptr_ = static_cast<uint8_t>(v);
      return *this;
   }
};

// ── mut_field: mutable field accessor ─────────────────────────────────────
// Returns fb_mut_ref<T> for scalars/enums/bools (in-place writable),
// falls through to view_field for everything else (read-only).

namespace detail::fbs {

template <typename T, size_t N>
auto mut_field(uint8_t* table)
{
   using F               = std::tuple_element_t<N, struct_tuple_t<T>>;
   constexpr int    slot = vt_slot<T>(N);
   constexpr auto   vt   = static_cast<uint16_t>(4 + 2 * slot);

   // vtable is reached via signed offset from table; it's read-only
   auto*    vtable = get_vtable(table);
   uint16_t fo     = field_offset(vtable, vt);

   if constexpr (std::is_same_v<F, bool>)
      return fb_mut_ref<bool>(fo ? table + fo : nullptr);
   else if constexpr (std::is_enum_v<F>)
      return fb_mut_ref<F>(fo ? table + fo : nullptr);
   else if constexpr (std::is_arithmetic_v<F>)
      return fb_mut_ref<F>(fo ? table + fo : nullptr);
   else if constexpr (is_optional<F>::value)
   {
      using Inner = typename F::value_type;
      if constexpr (std::is_same_v<Inner, bool>)
         return fb_mut_ref<bool>(fo ? table + fo : nullptr);
      else if constexpr (std::is_enum_v<Inner>)
         return fb_mut_ref<Inner>(fo ? table + fo : nullptr);
      else if constexpr (std::is_arithmetic_v<Inner>)
         return fb_mut_ref<Inner>(fo ? table + fo : nullptr);
      else
         return view_field<T, N>(static_cast<const uint8_t*>(table));
   }
   else if constexpr (is_table<F>::value)
   {
      // sub-table: return mutable view so nested scalars are writable
      if (fo)
      {
         auto* sub = table + fo + read<uint32_t>(table + fo);
         return fb_mut<F>(sub);
      }
      return fb_mut<F>{};
   }
   else
   {
      // strings, vectors, nested — read-only view
      return view_field<T, N>(static_cast<const uint8_t*>(table));
   }
}

// ── Offset adjustment for in-place buffer mutation ──────────────────────
// When bytes are inserted or removed at splice_pos, every offset whose
// source and target straddle that position must be adjusted by delta.

using splice_fn_t = void (*)(std::vector<uint8_t>&, size_t, int32_t);

inline void adjust_fwd(uint8_t* buf, size_t off_pos, size_t sp, int32_t d)
{
   uint32_t val = read<uint32_t>(buf + off_pos);
   size_t   tgt = off_pos + val;
   size_t   ns  = off_pos >= sp ? off_pos + d : off_pos;
   size_t   nt  = tgt >= sp ? tgt + d : tgt;
   store<uint32_t>(buf + off_pos, static_cast<uint32_t>(nt - ns));
}

template <typename T>
void adjust_table(uint8_t* buf, size_t ta, size_t sp, int32_t d);

template <typename T, size_t I>
void adjust_field(uint8_t* buf, size_t ta, const uint8_t* vt,
                  uint16_t vts, size_t sp, int32_t d)
{
   using F            = std::tuple_element_t<I, struct_tuple_t<T>>;
   constexpr int slot = vt_slot<T>(I);
   auto          ve   = static_cast<uint16_t>(4 + 2 * slot);
   uint16_t      fo   = ve + 2 <= vts ? read<uint16_t>(vt + ve) : 0;
   if (!fo)
      return;

   size_t op = ta + fo;

   if constexpr (std::is_same_v<F, std::string>)
   {
      adjust_fwd(buf, op, sp, d);
   }
   else if constexpr (is_optional<F>::value)
   {
      using Inner = typename F::value_type;
      if constexpr (std::is_same_v<Inner, std::string>)
         adjust_fwd(buf, op, sp, d);
      else if constexpr (is_table<Inner>::value)
      {
         size_t tgt = op + read<uint32_t>(buf + op);
         adjust_fwd(buf, op, sp, d);
         adjust_table<Inner>(buf, tgt, sp, d);
      }
   }
   else if constexpr (is_table<F>::value)
   {
      size_t tgt = op + read<uint32_t>(buf + op);
      adjust_fwd(buf, op, sp, d);
      adjust_table<F>(buf, tgt, sp, d);
   }
   else if constexpr (is_vector<F>::value)
   {
      using E    = typename F::value_type;
      size_t va2 = op + read<uint32_t>(buf + op);
      adjust_fwd(buf, op, sp, d);
      if constexpr (std::is_same_v<E, std::string>)
      {
         uint32_t n = read<uint32_t>(buf + va2);
         for (uint32_t j = 0; j < n; ++j)
            adjust_fwd(buf, va2 + 4 + j * 4, sp, d);
      }
      else if constexpr (is_table<E>::value)
      {
         uint32_t n = read<uint32_t>(buf + va2);
         for (uint32_t j = 0; j < n; ++j)
         {
            size_t ep = va2 + 4 + j * 4;
            size_t et = ep + read<uint32_t>(buf + ep);
            adjust_fwd(buf, ep, sp, d);
            adjust_table<E>(buf, et, sp, d);
         }
      }
   }
   else if constexpr (is_variant<F>::value)
   {
      auto     vve = static_cast<uint16_t>(4 + 2 * (slot + 1));
      uint16_t vfo = vve + 2 <= vts ? read<uint16_t>(vt + vve) : 0;
      if (vfo)
      {
         uint8_t ti = read<uint8_t>(buf + ta + fo);
         if (ti > 0)
         {
            size_t vp  = ta + vfo;
            size_t vt2 = vp + read<uint32_t>(buf + vp);
            adjust_fwd(buf, vp, sp, d);
            [&]<size_t... J>(std::index_sequence<J...>)
            {
               ((J + 1 == ti ? (adjust_table<std::variant_alternative_t<J, F>>(buf, vt2, sp, d), 0)
                             : 0),
                ...);
            }(std::make_index_sequence<std::variant_size_v<F>>{});
         }
      }
   }
   else if constexpr (is_fb_nested<F>::value || is_array<F>::value)
   {
      adjust_fwd(buf, op, sp, d);
   }
   // scalars, enums, bools, inline structs: no offset to adjust
}

template <typename T>
void adjust_table(uint8_t* buf, size_t ta, size_t sp, int32_t d)
{
   // Read vtable location BEFORE adjusting soffset
   int32_t so  = read<int32_t>(buf + ta);
   size_t  va  = ta - so;
   auto*   vt  = buf + va;
   auto    vts = read<uint16_t>(vt);

   // Adjust soffset (signed backward offset to vtable)
   size_t nta = ta >= sp ? ta + d : ta;
   size_t nva = va >= sp ? va + d : va;
   store<int32_t>(buf + ta, static_cast<int32_t>(nta - nva));

   // Walk all reflected fields
   constexpr size_t N = std::tuple_size_v<struct_tuple_t<T>>;
   [&]<size_t... I>(std::index_sequence<I...>)
   {
      (adjust_field<T, I>(buf, ta, vt, vts, sp, d), ...);
   }(std::make_index_sequence<N>{});
}

/// Adjust all offsets in a FlatBuffer rooted at type RootT, then
/// insert/remove bytes at splice_pos.  delta > 0 inserts, delta < 0 removes.
template <typename RootT>
void splice_buffer(std::vector<uint8_t>& buf, size_t splice_pos, int32_t delta)
{
   if (delta == 0)
      return;

   // Read root table position before any adjustments
   size_t root_abs = read<uint32_t>(buf.data());

   // Adjust root offset (forward offset from position 0)
   adjust_fwd(buf.data(), 0, splice_pos, delta);

   // Walk the full type tree and adjust all internal offsets
   adjust_table<RootT>(buf.data(), root_abs, splice_pos, delta);

   // Splice the actual bytes
   if (delta > 0)
      buf.insert(buf.begin() + splice_pos, delta, 0);
   else
      buf.erase(buf.begin() + splice_pos + delta, buf.begin() + splice_pos);
}

}  // namespace detail::fbs

// ══════════════════════════════════════════════════════════════════════════
// fb_mut — mutable view of a FlatBuffer table
//
// Same named accessors as fb_view, but scalar/enum/bool fields return
// fb_mut_ref<T> — an assignable proxy reference:
//
//   auto m = psio::fb_mut<UserProfile>::from_buffer(buf);
//   m.id() = 999;           // writes in-place
//   m.age() = 33;
//   m.verified() = false;
//   auto name = m.name();   // still returns string_view (read-only)
// ══════════════════════════════════════════════════════════════════════════

template <typename T>
class fb_mut_impl
{
   uint8_t* table_;

  public:
   explicit fb_mut_impl(uint8_t* t = nullptr) : table_(t) {}

   uint8_t* table_ptr() const { return table_; }

   template <size_t I, auto /* MemberPtr */>
   auto get() const
   {
      return detail::fbs::mut_field<T, I>(table_);
   }
};

template <typename T>
class fb_mut : public reflect<T>::template proxy<fb_mut_impl<T>>
{
   using base = typename reflect<T>::template proxy<fb_mut_impl<T>>;

  public:
   explicit fb_mut(uint8_t* t = nullptr) : base(t) {}
   explicit operator bool() const { return this->psio_get_proxy().table_ptr() != nullptr; }

   static fb_mut from_buffer(uint8_t* buf)
   {
      return fb_mut(buf + detail::fbs::read<uint32_t>(buf));
   }

   template <size_t N>
   auto get() const
   {
      return detail::fbs::mut_field<T, N>(this->psio_get_proxy().table_ptr());
   }
};

// ══════════════════════════════════════════════════════════════════════════
// fb_doc — mutable FlatBuffer document with O(1) string/dynamic mutation
//
// Scalars are mutated in-place (same as fb_mut).  Strings are mutated by
// appending a new blob at the end of the buffer and updating the single
// parent offset — the old string becomes dead space.  Call canonicalize()
// to rebuild with no dead space and produce a deterministic byte
// representation (compact() is an alias).
//
//   auto doc = psio::fb_doc<UserProfile>::from_value(user);
//   doc.id() = 999;
//   doc.name() = "Bob";                // O(1) append
//   doc.email() = "bob@example.com";   // O(1) append
//   doc.canonicalize();                // deterministic, minimal-size
//   send(doc.data(), doc.size());
// ══════════════════════════════════════════════════════════════════════════

/// Mutable string proxy.  Reads return string_view into the buffer.
/// Assignment appends a new string blob and updates one offset.
class fb_doc_str
{
   std::vector<uint8_t>* buf_;
   size_t                off_pos_;  // absolute position of forward offset (0 = absent)

  public:
   fb_doc_str(std::vector<uint8_t>* b, size_t op) : buf_(b), off_pos_(op) {}

   operator std::string_view() const
   {
      if (!off_pos_)
         return {};
      auto*    buf = buf_->data();
      size_t   sa  = off_pos_ + detail::fbs::read<uint32_t>(buf + off_pos_);
      uint32_t len = detail::fbs::read<uint32_t>(buf + sa);
      return {reinterpret_cast<const char*>(buf + sa + 4), len};
   }

   const char* data() const { return std::string_view(*this).data(); }
   size_t      size() const { return std::string_view(*this).size(); }

   fb_doc_str& operator=(std::string_view s)
   {
      if (!off_pos_)
         return *this;

      // Append new string blob at end of buffer
      size_t new_total = (4 + s.size() + 1 + 3) & ~size_t(3);
      size_t new_pos   = buf_->size();
      buf_->resize(new_pos + new_total, 0);

      auto* buf = buf_->data();
      detail::fbs::store<uint32_t>(buf + new_pos, static_cast<uint32_t>(s.size()));
      std::memcpy(buf + new_pos + 4, s.data(), s.size());
      buf[new_pos + 4 + s.size()] = 0;

      // Update the one forward offset to point to the new blob
      detail::fbs::store<uint32_t>(buf + off_pos_,
                                   static_cast<uint32_t>(new_pos - off_pos_));
      return *this;
   }
};

/// Proxy backing object for fb_doc / fb_doc_ref.
/// Stores a buffer pointer and a byte offset to the table.
template <typename T>
class fb_doc_impl
{
   std::vector<uint8_t>* buf_;
   size_t                table_off_;

  public:
   fb_doc_impl(std::vector<uint8_t>* b = nullptr, size_t off = 0)
       : buf_(b), table_off_(off)
   {
   }

   void                  set_buf(std::vector<uint8_t>* b) { buf_ = b; }
   void                  set_off(size_t o) { table_off_ = o; }
   std::vector<uint8_t>* buf() const { return buf_; }
   size_t                off() const { return table_off_; }

   template <size_t I, auto /* MemberPtr */>
   auto get() const
   {
      using F            = std::tuple_element_t<I, struct_tuple_t<T>>;
      constexpr int slot = detail::fbs::vt_slot<T>(I);
      auto          vt   = static_cast<uint16_t>(4 + 2 * slot);

      auto*    table  = buf_->data() + table_off_;
      auto*    vtable = detail::fbs::get_vtable(table);
      uint16_t fo     = detail::fbs::field_offset(vtable, vt);

      if constexpr (std::is_same_v<F, bool>)
         return fb_mut_ref<bool>(fo ? table + fo : nullptr);
      else if constexpr (std::is_enum_v<F>)
         return fb_mut_ref<F>(fo ? table + fo : nullptr);
      else if constexpr (std::is_arithmetic_v<F>)
         return fb_mut_ref<F>(fo ? table + fo : nullptr);
      else if constexpr (std::is_same_v<F, std::string>)
         return fb_doc_str(buf_, fo ? table_off_ + fo : 0);
      else if constexpr (detail::fbs::is_optional<F>::value)
      {
         using Inner = typename F::value_type;
         if constexpr (std::is_same_v<Inner, bool>)
            return fb_mut_ref<bool>(fo ? table + fo : nullptr);
         else if constexpr (std::is_enum_v<Inner>)
            return fb_mut_ref<Inner>(fo ? table + fo : nullptr);
         else if constexpr (std::is_arithmetic_v<Inner>)
            return fb_mut_ref<Inner>(fo ? table + fo : nullptr);
         else if constexpr (std::is_same_v<Inner, std::string>)
            return fb_doc_str(buf_, fo ? table_off_ + fo : 0);
         else
            return detail::fbs::view_field<T, I>(
                static_cast<const uint8_t*>(buf_->data() + table_off_));
      }
      else if constexpr (detail::fbs::is_table<F>::value)
      {
         if (fo)
         {
            size_t sub = table_off_ + fo + detail::fbs::read<uint32_t>(table + fo);
            return fb_doc_ref<F>(buf_, sub);
         }
         return fb_doc_ref<F>(static_cast<std::vector<uint8_t>*>(nullptr), 0);
      }
      else
         return detail::fbs::view_field<T, I>(
             static_cast<const uint8_t*>(buf_->data() + table_off_));
   }
};

/// Lightweight mutable view of a sub-table within an fb_doc buffer.
template <typename T>
class fb_doc_ref : public reflect<T>::template proxy<fb_doc_impl<T>>
{
   using base = typename reflect<T>::template proxy<fb_doc_impl<T>>;

  public:
   fb_doc_ref(std::vector<uint8_t>* b = nullptr, size_t off = 0) : base(b, off) {}
   explicit operator bool() const { return this->psio_get_proxy().buf() != nullptr; }

   template <size_t N>
   auto get() const
   {
      return this->psio_get_proxy().template get<N, nullptr>();
   }
};

/// Mutable FlatBuffer document — owns the buffer, supports O(1) mutation.
template <typename T>
class fb_doc : public reflect<T>::template proxy<fb_doc_impl<T>>
{
   using base = typename reflect<T>::template proxy<fb_doc_impl<T>>;
   std::vector<uint8_t> buf_;

   void fix_base()
   {
      this->psio_get_proxy().set_buf(&buf_);
      if (!buf_.empty())
         this->psio_get_proxy().set_off(detail::fbs::read<uint32_t>(buf_.data()));
   }

  public:
   fb_doc() : base(nullptr, 0) {}

   explicit fb_doc(std::vector<uint8_t> buf) : base(nullptr, 0), buf_(std::move(buf))
   {
      fix_base();
   }

   static fb_doc from_value(const T& val)
   {
      fb_builder fbb;
      fbb.pack(val);
      return fb_doc(std::vector<uint8_t>(fbb.data(), fbb.data() + fbb.size()));
   }

   fb_doc(const fb_doc& o) : base(static_cast<const base&>(o)), buf_(o.buf_) { fix_base(); }
   fb_doc& operator=(const fb_doc& o)
   {
      buf_ = o.buf_;
      fix_base();
      return *this;
   }
   fb_doc(fb_doc&& o) noexcept : base(static_cast<base&&>(o)), buf_(std::move(o.buf_))
   {
      fix_base();
   }
   fb_doc& operator=(fb_doc&& o) noexcept
   {
      buf_ = std::move(o.buf_);
      fix_base();
      return *this;
   }

   explicit operator bool() const { return !buf_.empty(); }

   const uint8_t*              data() const { return buf_.data(); }
   size_t                      size() const { return buf_.size(); }
   std::vector<uint8_t>&       buffer() { return buf_; }
   const std::vector<uint8_t>& buffer() const { return buf_; }

   T unpack() const { return fb_unpack<T>(buf_.data()); }

   /// Rebuild the buffer: reclaim dead space and produce the canonical
   /// byte representation.  Two documents with the same logical data
   /// will be byte-identical after canonicalize().
   void canonicalize()
   {
      T          val = fb_unpack<T>(buf_.data());
      fb_builder fbb;
      fbb.pack(val);
      buf_.assign(fbb.data(), fbb.data() + fbb.size());
      fix_base();
   }

   /// Alias for canonicalize().
   void compact() { canonicalize(); }

   template <size_t N>
   auto get() const
   {
      return this->psio_get_proxy().template get<N, nullptr>();
   }
};

// ══════════════════════════════════════════════════════════════════════════
// fb_nested — owns a serialized FlatBuffer and exposes the same named
// proxy accessors as fb_view.  Used for nested FlatBuffer fields:
//
//   struct Outer { fb_nested<Inner> payload; };
//
// On the wire: stored as a [ubyte] vector containing a complete FlatBuffer.
// In C++: payload.name() works just like fb_view<Inner>::name().
//         payload.unpack() → Inner.
// ══════════════════════════════════════════════════════════════════════════

template <typename T>
class fb_nested_impl
{
   const std::vector<uint8_t>* data_;

  public:
   explicit fb_nested_impl(const std::vector<uint8_t>* d) : data_(d) {}

   const uint8_t* table_ptr() const
   {
      return (data_ && !data_->empty()) ? detail::fbs::get_root(data_->data()) : nullptr;
   }

   template <size_t I, auto /* MemberPtr */>
   auto get() const
   {
      return detail::fbs::view_field<T, I>(table_ptr());
   }
};

template <typename T>
class fb_nested : public reflect<T>::template proxy<fb_nested_impl<T>>
{
   using base = typename reflect<T>::template proxy<fb_nested_impl<T>>;
   std::vector<uint8_t> data_;

  public:
   fb_nested() : base(&data_) {}
   explicit fb_nested(std::vector<uint8_t> d) : base(&data_), data_(std::move(d)) {}

   fb_nested(const fb_nested& o) : base(&data_), data_(o.data_) {}
   fb_nested& operator=(const fb_nested& o)
   {
      data_ = o.data_;
      return *this;
   }
   fb_nested(fb_nested&& o) noexcept : base(&data_), data_(std::move(o.data_)) {}
   fb_nested& operator=(fb_nested&& o) noexcept
   {
      data_ = std::move(o.data_);
      return *this;
   }

   explicit operator bool() const { return !data_.empty(); }

   T unpack() const { return fb_unpack<T>(data_.data()); }

   fb_view<T> view() const
   {
      return data_.empty() ? fb_view<T>{} : fb_view<T>::from_buffer(data_.data());
   }

   const uint8_t*                data() const { return data_.data(); }
   size_t                        size() const { return data_.size(); }
   std::vector<uint8_t>&         buffer() { return data_; }
   const std::vector<uint8_t>&   buffer() const { return data_; }

   template <size_t N>
   auto get() const
   {
      return detail::fbs::view_field<T, N>(this->psio_get_proxy().table_ptr());
   }
};

// ══════════════════════════════════════════════════════════════════════════
// fb_verify — bounds-check a FlatBuffer before trusting it
//
// Returns true if the buffer is structurally valid for type T.
// Checks: root offset, vtable pointers, field offsets, string lengths,
// vector lengths, nested tables (recursive).
// ══════════════════════════════════════════════════════════════════════════

namespace detail::fbs {

inline bool in_bounds(const uint8_t* buf, size_t size, const uint8_t* p, size_t len)
{
   return p >= buf && len <= static_cast<size_t>(buf + size - p);
}

// Forward declaration
template <typename T>
bool verify_table(const uint8_t* buf, size_t size, const uint8_t* table, int depth);

inline bool verify_string(const uint8_t* buf, size_t size, const uint8_t* p)
{
   if (!in_bounds(buf, size, p, 4))
      return false;
   uint32_t len = read<uint32_t>(p);
   // string data + null terminator
   return in_bounds(buf, size, p + 4, len + 1);
}

template <typename E>
bool verify_vector(const uint8_t* buf, size_t size, const uint8_t* p, int depth);

template <typename V>
bool verify_field(const uint8_t* buf, size_t size, const uint8_t* table,
                  const uint8_t* vt, int slot, int depth)
{
   uint16_t vt_off = static_cast<uint16_t>(4 + 2 * slot);
   uint16_t fo     = field_offset(vt, vt_off);
   if (!fo)
      return true;  // absent field is valid

   auto* field_ptr = table + fo;

   if constexpr (std::is_same_v<V, bool> || std::is_arithmetic_v<V>)
   {
      return in_bounds(buf, size, field_ptr, sizeof(V));
   }
   else if constexpr (std::is_enum_v<V>)
   {
      return in_bounds(buf, size, field_ptr, sizeof(std::underlying_type_t<V>));
   }
   else if constexpr (std::is_same_v<V, std::string>)
   {
      if (!in_bounds(buf, size, field_ptr, 4))
         return false;
      auto* str = deref(field_ptr);
      return verify_string(buf, size, str);
   }
   else if constexpr (is_optional<V>::value)
   {
      using Inner = typename V::value_type;
      // Optional field present — verify as its inner type
      if constexpr (std::is_arithmetic_v<Inner> || std::is_enum_v<Inner> ||
                    std::is_same_v<Inner, bool>)
         return in_bounds(buf, size, field_ptr,
                          sizeof(std::conditional_t<std::is_enum_v<Inner>,
                                                    std::underlying_type_t<Inner>, Inner>));
      else if constexpr (std::is_same_v<Inner, std::string>)
      {
         if (!in_bounds(buf, size, field_ptr, 4))
            return false;
         return verify_string(buf, size, deref(field_ptr));
      }
      else
         return true;
   }
   else if constexpr (is_array<V>::value)
   {
      // Table arrays stored as vectors
      if (!in_bounds(buf, size, field_ptr, 4))
         return false;
      auto* vp = deref(field_ptr);
      return verify_vector<typename is_array<V>::value_type>(buf, size, vp, depth);
   }
   else if constexpr (is_variant<V>::value)
   {
      // Union: type byte is at this slot, value offset at slot+1
      if (!in_bounds(buf, size, field_ptr, 1))
         return false;
      uint8_t type_idx = read<uint8_t>(field_ptr);
      if (type_idx == 0)
         return true;  // NONE
      // Check value offset at slot+1
      uint16_t val_vt_off = static_cast<uint16_t>(4 + 2 * (slot + 1));
      uint16_t val_fo     = field_offset(vt, val_vt_off);
      if (!val_fo)
         return false;  // has type but no value
      auto* val_ptr = table + val_fo;
      if (!in_bounds(buf, size, val_ptr, 4))
         return false;
      auto* sub_table = deref(val_ptr);
      // Verify as generic table (we don't know the specific type statically
      // without the variant index, but we can check basic table structure)
      if (!in_bounds(buf, size, sub_table, 4))
         return false;
      auto* sub_vt = get_vtable(sub_table);
      if (!in_bounds(buf, size, sub_vt, 4))
         return false;
      uint16_t sub_vt_size = read<uint16_t>(sub_vt);
      return in_bounds(buf, size, sub_vt, sub_vt_size);
   }
   else if constexpr (is_vector<V>::value)
   {
      if (!in_bounds(buf, size, field_ptr, 4))
         return false;
      auto* vp = deref(field_ptr);
      return verify_vector<typename V::value_type>(buf, size, vp, depth);
   }
   else if constexpr (is_fb_struct<V>())
   {
      return in_bounds(buf, size, field_ptr, struct_type_size<V>());
   }
   else if constexpr (is_table<V>::value)
   {
      if (!in_bounds(buf, size, field_ptr, 4))
         return false;
      return verify_table<V>(buf, size, deref(field_ptr), depth + 1);
   }
   else
      return true;
}

template <typename E>
bool verify_vector(const uint8_t* buf, size_t size, const uint8_t* vp, int depth)
{
   if (!in_bounds(buf, size, vp, 4))
      return false;
   uint32_t count = read<uint32_t>(vp);
   auto*    elems = vp + 4;

   if constexpr (std::is_same_v<E, std::string>)
   {
      if (!in_bounds(buf, size, elems, count * 4))
         return false;
      for (uint32_t j = 0; j < count; ++j)
      {
         auto* entry = elems + j * 4;
         if (!verify_string(buf, size, entry + read<uint32_t>(entry)))
            return false;
      }
   }
   else if constexpr (std::is_arithmetic_v<E> || std::is_enum_v<E>)
   {
      size_t elem_sz = std::is_enum_v<E> ? sizeof(std::underlying_type_t<E>) : sizeof(E);
      if (!in_bounds(buf, size, elems, count * elem_sz))
         return false;
   }
   else if constexpr (is_fb_struct<E>())
   {
      if (!in_bounds(buf, size, elems, count * struct_type_size<E>()))
         return false;
   }
   else if constexpr (is_table<E>::value)
   {
      if (!in_bounds(buf, size, elems, count * 4))
         return false;
      for (uint32_t j = 0; j < count; ++j)
      {
         auto* entry     = elems + j * 4;
         auto* sub_table = entry + read<uint32_t>(entry);
         if (!verify_table<E>(buf, size, sub_table, depth + 1))
            return false;
      }
   }
   return true;
}

template <typename T>
bool verify_table(const uint8_t* buf, size_t size, const uint8_t* table, int depth)
{
   if (depth > 64)
      return false;  // recursion limit

   if (!in_bounds(buf, size, table, 4))
      return false;

   auto* vt = get_vtable(table);
   if (!in_bounds(buf, size, vt, 4))
      return false;

   uint16_t vt_size = read<uint16_t>(vt);
   if (vt_size < 4 || !in_bounds(buf, size, vt, vt_size))
      return false;

   uint16_t obj_size = read<uint16_t>(vt + 2);
   if (!in_bounds(buf, size, table, obj_size))
      return false;

   // Verify each field
   constexpr auto slots = build_slot_map<T>();
   bool           ok    = true;
   size_t         idx   = 0;
   psio::apply_members(
       (typename reflect<T>::data_members*)nullptr,
       [&](auto... members)
       {
          (([&]
           {
              if (!ok)
                 return;
              using V = std::remove_cvref_t<decltype(psio::result_of_member(members))>;
              ok      = verify_field<V>(buf, size, table, vt, slots[idx], depth);
              ++idx;
           }()),
           ...);
       });
   return ok;
}

}  // namespace detail::fbs

/// Verify a FlatBuffer is structurally valid for type T.
/// Returns true if safe to access via fb_unpack or fb_view.
template <typename T>
bool fb_verify(const uint8_t* buf, size_t size)
{
   if (size < 4)
      return false;
   auto root_off = detail::fbs::read<uint32_t>(buf);
   if (root_off >= size)
      return false;
   return detail::fbs::verify_table<T>(buf, size, buf + root_off, 0);
}

/// Verify a size-prefixed FlatBuffer.
template <typename T>
bool fb_verify_size_prefixed(const uint8_t* buf, size_t size)
{
   if (size < 8)
      return false;
   auto len = detail::fbs::read<uint32_t>(buf);
   if (len + 4 > size)
      return false;
   return fb_verify<T>(buf + 4, len);
}

// ══════════════════════════════════════════════════════════════════════════
// Schema export: to_fbs_schema<T>()
//
// Generates a .fbs schema string from PSIO_REFLECT'd types.
// Types with definitionWillNotChange + all-scalar fields → struct.
// Everything else → table.
//
//   std::string schema = psio::to_fbs_schema<Order>();
// ══════════════════════════════════════════════════════════════════════════

namespace detail::fbs {

// Map C++ scalar types to FlatBuffer type names
template <typename V>
constexpr const char* fbs_scalar_name()
{
   if constexpr (std::is_same_v<V, bool>)
      return "bool";
   else if constexpr (std::is_same_v<V, uint8_t>)
      return "ubyte";
   else if constexpr (std::is_same_v<V, int8_t>)
      return "byte";
   else if constexpr (std::is_same_v<V, uint16_t>)
      return "ushort";
   else if constexpr (std::is_same_v<V, int16_t>)
      return "short";
   else if constexpr (std::is_same_v<V, uint32_t>)
      return "uint";
   else if constexpr (std::is_same_v<V, int32_t>)
      return "int";
   else if constexpr (std::is_same_v<V, uint64_t>)
      return "ulong";
   else if constexpr (std::is_same_v<V, int64_t>)
      return "long";
   else if constexpr (std::is_same_v<V, float>)
      return "float";
   else if constexpr (std::is_same_v<V, double>)
      return "double";
   else
      return nullptr;
}

// Detect if an enum has PSIO_REFLECT_ENUM
template <typename V, typename = void>
struct has_enum_reflect : std::false_type {};
template <typename V>
struct has_enum_reflect<V, std::void_t<decltype(reflect<V>::is_enum)>>
    : std::bool_constant<reflect<V>::is_enum> {};

// Get the FlatBuffer type string for any supported field type.
template <typename V>
std::string fbs_type_name()
{
   if constexpr (std::is_same_v<V, bool>)
      return "bool";
   else if constexpr (std::is_enum_v<V> && has_enum_reflect<V>::value)
      return std::string(reflect<V>::name.c_str());  // reflected enum → use name
   else if constexpr (std::is_enum_v<V>)
      return fbs_scalar_name<std::underlying_type_t<V>>();  // unreflected → underlying type
   else if constexpr (std::is_arithmetic_v<V>)
      return fbs_scalar_name<V>();
   else if constexpr (std::is_same_v<V, std::string>)
      return "string";
   else if constexpr (is_optional<V>::value)
      return fbs_type_name<typename V::value_type>();
   else if constexpr (is_array<V>::value)
   {
      // In struct context: [T:N], in table context: [T] (vector)
      return "[" + fbs_type_name<typename is_array<V>::value_type>() + "]";
   }
   else if constexpr (is_variant<V>::value)
   {
      // Unions reference the union type name (generated separately)
      // For now emit as the first alternative's name with _union suffix
      using First = std::variant_alternative_t<0, V>;
      return std::string(reflect<First>::name.c_str()) + "_union";
   }
   else if constexpr (is_fb_nested<V>::value)
   {
      using Inner = typename is_fb_nested<V>::inner_type;
      return "[ubyte] (nested_flatbuffer: \"" + std::string(reflect<Inner>::name.c_str()) + "\")";
   }
   else if constexpr (is_flat_set<V>::value)
      return "[" + fbs_type_name<typename is_flat_set<V>::key_type>() + "]";
   else if constexpr (is_flat_map<V>::value)
   {
      // Emit as vector of entry tables with key attribute
      using K  = typename is_flat_map<V>::key_type;
      using MV = typename is_flat_map<V>::mapped_type;
      return "[" + fbs_type_name<K>() + "_" + fbs_type_name<MV>() + "_entry]";
   }
   else if constexpr (is_vector<V>::value)
      return "[" + fbs_type_name<typename V::value_type>() + "]";
   else if constexpr (is_table<V>::value)
      return std::string(reflect<V>::name.c_str());
   else
   {
      static_assert(!sizeof(V*), "unsupported type for FBS schema export");
      return {};
   }
}

// Emit a reflected enum type definition
template <typename E>
void emit_enum_type(std::string& out, std::vector<std::string>& seen)
{
   static_assert(std::is_enum_v<E>);
   std::string ename(reflect<E>::name.c_str());
   for (auto& s : seen)
      if (s == ename)
         return;
   seen.push_back(ename);

   out += "enum ";
   out += ename;
   out += " : ";
   out += fbs_scalar_name<std::underlying_type_t<E>>();
   out += " {\n";
   for (size_t i = 0; i < reflect<E>::count; ++i)
   {
      out += "  ";
      out += reflect<E>::labels[i];
      out += " = ";
      out += std::to_string(static_cast<std::underlying_type_t<E>>(reflect<E>::values[i]));
      out += ",\n";
   }
   out += "}\n\n";
}

// Emit a union type definition for a std::variant
template <typename V>
void emit_union(std::string& out, std::vector<std::string>& seen, const char* field_name);

// Emit a type definition if it hasn't been emitted yet.
template <typename T>
void emit_schema(std::string& out, std::vector<std::string>& seen)
{
   std::string name(reflect<T>::name.c_str());
   for (auto& s : seen)
      if (s == name)
         return;
   seen.push_back(name);

   // Recurse into nested types first (dependency order)
   apply_members(
       (typename reflect<T>::data_members*)nullptr,
       [&](auto... members)
       {
          size_t fi = 0;
          auto recurse = [&]<typename V>(V*)
          {
             using F = std::remove_cvref_t<V>;
             if constexpr (std::is_enum_v<F> && has_enum_reflect<F>::value)
             {
                emit_enum_type<F>(out, seen);
             }
             else if constexpr (is_optional<F>::value)
             {
                using Inner = typename F::value_type;
                if constexpr (std::is_enum_v<Inner> && has_enum_reflect<Inner>::value)
                   emit_enum_type<Inner>(out, seen);
                else if constexpr (is_table<Inner>::value)
                   emit_schema<Inner>(out, seen);
             }
             else if constexpr (is_variant<F>::value)
             {
                // Emit all variant alternative types, then emit the union
                [&]<size_t... I>(std::index_sequence<I...>)
                {
                   (([]<typename Alt>(std::string& o, std::vector<std::string>& s, Alt*)
                    {
                       if constexpr (is_table<Alt>::value)
                          emit_schema<Alt>(o, s);
                    }(out, seen,
                      static_cast<std::variant_alternative_t<I, F>*>(nullptr))),
                    ...);
                }(std::make_index_sequence<std::variant_size_v<F>>{});
                emit_union<F>(out, seen, reflect<T>::data_member_names[fi]);
             }
             else if constexpr (is_flat_map<F>::value)
             {
                using K  = typename is_flat_map<F>::key_type;
                using MV = typename is_flat_map<F>::mapped_type;
                // Emit value type schema if it's a table
                if constexpr (is_table<MV>::value)
                   emit_schema<MV>(out, seen);
                // Emit auto-generated entry table
                std::string entry_name = fbs_type_name<K>() + "_" + fbs_type_name<MV>() + "_entry";
                bool already = false;
                for (auto& s : seen)
                   if (s == entry_name)
                      already = true;
                if (!already)
                {
                   seen.push_back(entry_name);
                   out += "table " + entry_name + " {\n";
                   out += "  key: " + fbs_type_name<K>() + " (key);\n";
                   out += "  value: " + fbs_type_name<MV>() + ";\n";
                   out += "}\n\n";
                }
             }
             else if constexpr (is_vector<F>::value)
             {
                using E = typename F::value_type;
                if constexpr (is_table<E>::value)
                   emit_schema<E>(out, seen);
             }
             else if constexpr (is_fb_nested<F>::value)
             {
                using Inner = typename is_fb_nested<F>::inner_type;
                emit_schema<Inner>(out, seen);
             }
             else if constexpr (is_table<F>::value)
             {
                emit_schema<F>(out, seen);
             }
             ++fi;
          };
          (recurse(
               static_cast<std::remove_cvref_t<decltype(psio::result_of_member(members))>*>(
                   nullptr)),
           ...);
       });

   // Emit this type
   if constexpr (is_fb_struct<T>())
      out += "struct ";
   else
      out += "table ";
   out += name;
   out += " {\n";

   // Emit fields
   size_t idx = 0;
   psio::apply_members(
       (typename reflect<T>::data_members*)nullptr,
       [&](auto... members)
       {
          (([&]
           {
              using V = std::remove_cvref_t<decltype(psio::result_of_member(members))>;
              out += "  ";
              out += reflect<T>::data_member_names[idx];
              out += ": ";
              if constexpr (is_variant<V>::value)
              {
                 // Union field references the generated union type name
                 using First = std::variant_alternative_t<0, V>;
                 out += std::string(reflect<First>::name.c_str()) + "_union";
              }
              else
              {
                 out += fbs_type_name<V>();
              }
              out += ";\n";
              ++idx;
           }()),
           ...);
       });

   out += "}\n\n";
}

template <typename V>
void emit_union(std::string& out, std::vector<std::string>& seen, const char* /*field_name*/)
{
   // Union name = first alternative name + "_union"
   using First     = std::variant_alternative_t<0, V>;
   std::string uname = std::string(reflect<First>::name.c_str()) + "_union";
   for (auto& s : seen)
      if (s == uname)
         return;
   seen.push_back(uname);

   out += "union ";
   out += uname;
   out += " {\n";
   [&]<size_t... I>(std::index_sequence<I...>)
   {
      ((out += "  ",
        out += std::string(
            reflect<std::variant_alternative_t<I, V>>::name.c_str()),
        out += ",\n"),
       ...);
   }(std::make_index_sequence<std::variant_size_v<V>>{});
   out += "}\n\n";
}

}  // namespace detail::fbs

/// Generate a FlatBuffer schema (.fbs) string for a root type and all
/// its dependencies.
template <typename T>
std::string to_fbs_schema()
{
   std::string              out;
   std::vector<std::string> seen;
   detail::fbs::emit_schema<T>(out, seen);
   out += "root_type ";
   out += reflect<T>::name.c_str();
   out += ";\n";
   return out;
}

}  // namespace psio
