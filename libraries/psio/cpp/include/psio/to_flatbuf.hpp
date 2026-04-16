// Reflect-based FlatBuffer serializer — no IDL, no flatc codegen.
//
// Usage:
//   struct Token { uint16_t kind; std::string text; };
//   PSIO_REFLECT(Token, kind, text)
//
//   flatbuffers::FlatBufferBuilder fbb(1024);
//   psio::to_flatbuf_finish(fbb, token);
//   auto buf = fbb.GetBufferPointer();
//
// Field order in PSIO_REFLECT must match the .fbs schema order
// so that vtable field IDs (4, 6, 8, …) are wire-compatible.

#pragma once

#include <flatbuffers/flatbuffers.h>
#include <psio/reflect.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace psio {

template <typename T>
flatbuffers::Offset<void> to_flatbuf(flatbuffers::FlatBufferBuilder& fbb,
                                     const T&                        value);

namespace detail::fb {

// ── Type classification ─────────────────────────────────────────────────

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

template <typename T, typename = void>
struct is_table : std::false_type
{
};
template <typename T>
struct is_table<T, std::void_t<typename psio::reflect<T>::data_members>>
    : std::bool_constant<!std::is_arithmetic_v<T> && !std::is_same_v<T, std::string>>
{
};

// ── Phase 1: pre-create sub-objects (strings, vectors, nested tables) ───

template <typename V>
uint32_t pre_create(flatbuffers::FlatBufferBuilder& fbb, const V& val)
{
   if constexpr (std::is_arithmetic_v<V>)
   {
      return 0;
   }
   else if constexpr (std::is_same_v<V, std::string>)
   {
      return fbb.CreateString(val).o;
   }
   else if constexpr (is_optional<V>::value)
   {
      if (val.has_value())
         return pre_create<typename V::value_type>(fbb, *val);
      return 0;
   }
   else if constexpr (is_vector<V>::value)
   {
      using E = typename V::value_type;
      if (val.empty())
         return 0;
      if constexpr (std::is_same_v<E, std::string>)
         return fbb.CreateVectorOfStrings(val).o;
      else if constexpr (std::is_arithmetic_v<E>)
         return fbb.CreateVector(val.data(), val.size()).o;
      else if constexpr (is_table<E>::value)
      {
         std::vector<flatbuffers::Offset<flatbuffers::Table>> offs;
         offs.reserve(val.size());
         for (auto& item : val)
            offs.push_back(
                flatbuffers::Offset<flatbuffers::Table>(to_flatbuf(fbb, item).o));
         return fbb.CreateVector(offs).o;
      }
      else
         static_assert(!sizeof(E*), "unsupported vector element type");
   }
   else if constexpr (is_table<V>::value)
   {
      return to_flatbuf(fbb, val).o;
   }
   else
   {
      static_assert(!sizeof(V*), "unsupported type for FlatBuffer serialization");
   }
}

struct prepare_fn
{
   flatbuffers::FlatBufferBuilder& fbb;
   uint32_t*                       offsets;
   int                             i = 0;

   template <typename T>
   void operator()(const T& val)
   {
      offsets[i++] = pre_create<std::remove_cvref_t<T>>(fbb, val);
   }
};

// ── Phase 2: add one field to the in-progress table ─────────────────────

template <typename V>
void add_one(flatbuffers::FlatBufferBuilder& fbb,
             int                             field_idx,
             const V&                        val,
             uint32_t                        offset)
{
   auto vt = static_cast<flatbuffers::voffset_t>(4 + 2 * field_idx);

   if constexpr (std::is_same_v<V, bool>)
   {
      fbb.AddElement<uint8_t>(vt, static_cast<uint8_t>(val), 0);
   }
   else if constexpr (std::is_arithmetic_v<V>)
   {
      fbb.AddElement<V>(vt, val, V{});
   }
   else if constexpr (is_optional<V>::value)
   {
      using Inner = typename V::value_type;
      if (val.has_value())
      {
         if constexpr (std::is_same_v<Inner, bool>)
            fbb.AddElement<uint8_t>(vt, static_cast<uint8_t>(*val), 0);
         else if constexpr (std::is_arithmetic_v<Inner>)
            fbb.AddElement<Inner>(vt, *val, Inner{});
         else if (offset)
            fbb.AddOffset(vt, flatbuffers::Offset<void>(offset));
      }
   }
   else
   {
      if (offset)
         fbb.AddOffset(vt, flatbuffers::Offset<void>(offset));
   }
}

}  // namespace detail::fb

/// Serialize a PSIO_REFLECT'd struct as a FlatBuffer table.
/// Returns the table's offset within the builder.
template <typename T>
flatbuffers::Offset<void> to_flatbuf(flatbuffers::FlatBufferBuilder& fbb,
                                     const T&                        value)
{
   constexpr size_t N = psio::apply_members(
       (typename reflect<T>::data_members*)nullptr,
       [](auto... M) { return sizeof...(M); });

   uint32_t offsets[N];

   psio::for_each_member(&value, (typename reflect<T>::data_members*)nullptr,
                         detail::fb::prepare_fn{fbb, offsets});

   auto start = fbb.StartTable();

   // Add fields in reverse order (FlatBuffers convention: largest alignment
   // first minimizes padding in the back-to-front buffer layout).
   psio::apply_members(
       (typename reflect<T>::data_members*)nullptr,
       [&](auto... members) {
          auto ptrs = std::make_tuple(members...);
          [&]<size_t... I>(std::index_sequence<I...>) {
             (detail::fb::add_one(fbb, N - 1 - I,
                                  value.*std::get<N - 1 - I>(ptrs),
                                  offsets[N - 1 - I]),
              ...);
          }(std::make_index_sequence<N>{});
       });

   return flatbuffers::Offset<void>(fbb.EndTable(start));
}

/// Build and finish a complete FlatBuffer for a root table.
template <typename T>
void to_flatbuf_finish(flatbuffers::FlatBufferBuilder& fbb, const T& value)
{
   auto root = to_flatbuf(fbb, value);
   fbb.Finish(flatbuffers::Offset<flatbuffers::Table>(root.o));
}

// ═══════════════════════════════════════════════════════════════════════════
// Reading: from_flatbuf (unpack) and flatbuf_view (zero-copy)
// ═══════════════════════════════════════════════════════════════════════════

namespace detail::fb {

// Forward declaration for recursive unpack
template <typename T>
void unpack_table(const flatbuffers::Table& table, T& out);

/// Read a single FlatBuffer field into a C++ member.
template <typename V>
void read_field(const flatbuffers::Table& table, int idx, V& out)
{
   auto vt = static_cast<flatbuffers::voffset_t>(4 + 2 * idx);

   if constexpr (std::is_same_v<V, bool>)
   {
      out = table.GetField<uint8_t>(vt, 0) != 0;
   }
   else if constexpr (std::is_arithmetic_v<V>)
   {
      out = table.GetField<V>(vt, V{});
   }
   else if constexpr (std::is_same_v<V, std::string>)
   {
      auto s = table.GetPointer<const flatbuffers::String*>(vt);
      if (s)
         out.assign(s->c_str(), s->size());
   }
   else if constexpr (is_optional<V>::value)
   {
      using Inner = typename V::value_type;
      if (table.GetOptionalFieldOffset(vt))
      {
         if constexpr (std::is_same_v<Inner, bool>)
            out = table.GetField<uint8_t>(vt, 0) != 0;
         else if constexpr (std::is_arithmetic_v<Inner>)
            out = table.GetField<Inner>(vt, Inner{});
         else if constexpr (std::is_same_v<Inner, std::string>)
         {
            auto s = table.GetPointer<const flatbuffers::String*>(vt);
            if (s)
               out = std::string(s->c_str(), s->size());
         }
      }
   }
   else if constexpr (is_vector<V>::value)
   {
      using E = typename V::value_type;
      if constexpr (std::is_same_v<E, std::string>)
      {
         auto vec = table.GetPointer<
             const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>*>(vt);
         if (vec)
         {
            out.resize(vec->size());
            for (flatbuffers::uoffset_t j = 0; j < vec->size(); ++j)
               out[j].assign(vec->Get(j)->c_str(), vec->Get(j)->size());
         }
      }
      else if constexpr (std::is_arithmetic_v<E>)
      {
         auto vec = table.GetPointer<const flatbuffers::Vector<E>*>(vt);
         if (vec)
         {
            out.resize(vec->size());
            for (flatbuffers::uoffset_t j = 0; j < vec->size(); ++j)
               out[j] = vec->Get(j);
         }
      }
      else if constexpr (is_table<E>::value)
      {
         auto vec = table.GetPointer<
             const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::Table>>*>(vt);
         if (vec)
         {
            out.resize(vec->size());
            for (flatbuffers::uoffset_t j = 0; j < vec->size(); ++j)
               unpack_table(*vec->Get(j), out[j]);
         }
      }
   }
   else if constexpr (is_table<V>::value)
   {
      auto nested = table.GetPointer<const flatbuffers::Table*>(vt);
      if (nested)
         unpack_table(*nested, out);
   }
}

struct unpack_fn
{
   const flatbuffers::Table& table;
   int                       i = 0;

   template <typename V>
   void operator()(V& field)
   {
      read_field<std::remove_cvref_t<V>>(table, i++, field);
   }
};

template <typename T>
void unpack_table(const flatbuffers::Table& table, T& out)
{
   psio::for_each_member(&out, (typename reflect<T>::data_members*)nullptr,
                         unpack_fn{table});
}

/// View helper: read one field as its zero-copy view type.
template <typename F>
auto view_field(const flatbuffers::Table& table, flatbuffers::voffset_t vt)
{
   if constexpr (std::is_same_v<F, bool>)
      return table.GetField<uint8_t>(vt, 0) != 0;
   else if constexpr (std::is_arithmetic_v<F>)
      return table.GetField<F>(vt, F{});
   else if constexpr (std::is_same_v<F, std::string>)
      return table.GetPointer<const flatbuffers::String*>(vt);
   else if constexpr (is_optional<F>::value)
   {
      using Inner = typename F::value_type;
      if constexpr (std::is_arithmetic_v<Inner> || std::is_same_v<Inner, bool>)
      {
         if (table.GetOptionalFieldOffset(vt))
         {
            if constexpr (std::is_same_v<Inner, bool>)
               return std::optional<bool>(table.GetField<uint8_t>(vt, 0) != 0);
            else
               return std::optional<Inner>(table.GetField<Inner>(vt, Inner{}));
         }
         return std::optional<Inner>{};
      }
      else  // optional<string> → nullable String*
         return table.GetPointer<const flatbuffers::String*>(vt);
   }
   else if constexpr (is_vector<F>::value)
   {
      using E = typename F::value_type;
      if constexpr (std::is_same_v<E, std::string>)
         return table.GetPointer<
             const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>*>(vt);
      else if constexpr (std::is_arithmetic_v<E>)
         return table.GetPointer<const flatbuffers::Vector<E>*>(vt);
      else
         return table.GetPointer<
             const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::Table>>*>(vt);
   }
   // is_table handled by flatbuf_view below (returns flatbuf_view<F>)
}

}  // namespace detail::fb

/// Deserialize a FlatBuffer into a C++ struct.
template <typename T>
T from_flatbuf(const uint8_t* buf)
{
   auto* table = flatbuffers::GetRoot<flatbuffers::Table>(buf);
   T     result{};
   detail::fb::unpack_table(*table, result);
   return result;
}

/// Zero-copy view of a FlatBuffer table, driven by PSIO_REFLECT.
///
///   auto v = psio::flatbuf_view<UserProfile>::from_buffer(buf);
///   uint64_t id              = v.get<0>();  // scalar
///   const flatbuffers::String* name = v.get<1>();  // zero-copy string
///   auto customer_view       = v.get<1>();  // nested → flatbuf_view<T>
template <typename T>
class flatbuf_view
{
   const flatbuffers::Table* table_;

  public:
   explicit flatbuf_view(const flatbuffers::Table* t) : table_(t) {}

   static flatbuf_view from_buffer(const uint8_t* buf)
   {
      return flatbuf_view(flatbuffers::GetRoot<flatbuffers::Table>(buf));
   }

   /// Read the Nth field (zero-indexed, matches PSIO_REFLECT order).
   /// Returns: scalars by value, strings as const String*, vectors as
   /// const Vector<…>*, nested tables as flatbuf_view<NestedType>.
   template <size_t N>
   auto get() const
   {
      using F        = std::tuple_element_t<N, struct_tuple_t<T>>;
      constexpr auto vt = static_cast<flatbuffers::voffset_t>(4 + 2 * N);

      if constexpr (detail::fb::is_table<F>::value)
         return flatbuf_view<F>(
             table_->GetPointer<const flatbuffers::Table*>(vt));
      else
         return detail::fb::view_field<F>(*table_, vt);
   }
};

}  // namespace psio
