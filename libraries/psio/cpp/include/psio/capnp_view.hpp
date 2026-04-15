// capnp_view.hpp — zero-copy read-only views over Cap'n Proto wire data
//
// Implements the unified view<T, cp> format tag, allowing PSIO_REFLECT'd
// C++ structs to be read directly from Cap'n Proto serialized messages with
// zero overhead and no external library dependency.
//
//   auto v = psio::capnp_view<Order>::from_buffer(capnp_flat_array);
//   v.id()               // uint64_t
//   v.customer().name()   // string_view
//   v.items()[0].qty()    // uint32_t
//
// Union support: std::variant<A,B,...> fields are read as tagged variants
// with the discriminant placed after all ordinal fields (matching the
// official capnp compiler's layout).
//
// Wire compatibility: layout matches the official capnp compiler's slot
// allocation for structs with sequential ordinals matching PSIO_REFLECT order.
//
// Supported types:
//   - All scalars (bool, intN, uintN, float, double), enums
//   - Text (std::string → string_view), Data (vector<uint8_t>)
//   - Nested structs, List(scalar), List(Text), List(Struct)
//   - Unions via std::variant (discriminant + per-alternative slots)
//   - Void via std::monostate (zero-size union alternatives)
//   - Default value XOR (non-zero defaults from C++ member initializers)
//
// Limitations:
//   - Single-segment messages only (as produced by messageToFlatArray)
//   - No groups or AnyPointer
//   - No far pointers (inter-segment references)
//   - No packed encoding (reads flat/unpacked only)
//   - Sets and maps not natively supported (capnp has no sorted containers)

#pragma once

#include <psio/reflect.hpp>
#include <psio/view.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

namespace psio
{

   struct cp;  // forward declaration — named 'cp' to avoid collision with ::capnp namespace

   // ── capnp_ptr: pointer into a Cap'n Proto struct ─────────────────────────

   struct capnp_ptr
   {
      const uint8_t* data       = nullptr;  // start of data section
      uint16_t       data_words = 0;        // data section size in 8-byte words
      uint16_t       ptr_count  = 0;        // pointer section entry count

      bool operator==(std::nullptr_t) const { return data == nullptr; }
   };

   // ── Low-level wire format helpers ────────────────────────────────────────

   namespace capnp_detail
   {
      inline uint64_t read_u64(const uint8_t* p)
      {
         uint64_t v;
         std::memcpy(&v, p, 8);
         return v;
      }

      inline uint32_t read_u32(const uint8_t* p)
      {
         uint32_t v;
         std::memcpy(&v, p, 4);
         return v;
      }

      inline uint16_t read_u16(const uint8_t* p)
      {
         uint16_t v;
         std::memcpy(&v, p, 2);
         return v;
      }

      // Extract signed 30-bit offset from a struct or list pointer word.
      // Pointer word layout: bits[0:1]=tag, bits[2:31]=offset (signed 30-bit)
      inline int32_t ptr_offset(uint64_t word)
      {
         return static_cast<int32_t>(static_cast<uint32_t>(word) & ~3u) >> 2;
      }

      // Resolve a struct pointer: returns {data_start, data_words, ptr_count}
      inline capnp_ptr resolve_struct_ptr(const uint8_t* ptr_loc)
      {
         uint64_t word = read_u64(ptr_loc);
         if (word == 0)
            return {};
         int32_t  off    = ptr_offset(word);
         uint16_t dw     = static_cast<uint16_t>((word >> 32) & 0xFFFF);
         uint16_t pc     = static_cast<uint16_t>((word >> 48) & 0xFFFF);
         return {ptr_loc + 8 + static_cast<int64_t>(off) * 8, dw, pc};
      }

      // Read a Text (string) from a pointer slot.
      // Cap'n Proto Text = List(UInt8) with NUL terminator.
      inline std::string_view read_text(const uint8_t* ptr_loc)
      {
         uint64_t word = read_u64(ptr_loc);
         if (word == 0)
            return {};
         int32_t  off   = ptr_offset(word);
         uint32_t count = static_cast<uint32_t>(word >> 35);
         auto*    data  = ptr_loc + 8 + static_cast<int64_t>(off) * 8;
         if (count == 0)
            return {};
         return {reinterpret_cast<const char*>(data), count - 1};  // exclude NUL
      }

      // Pointer section start for a capnp_ptr
      inline const uint8_t* ptr_section(capnp_ptr p)
      {
         return p.data + static_cast<uint32_t>(p.data_words) * 8;
      }

      // Get pointer slot N from a struct
      inline const uint8_t* ptr_slot(capnp_ptr p, uint32_t idx)
      {
         return ptr_section(p) + idx * 8;
      }

      // Resolve a list pointer, returning element data, count, and stride.
      struct list_info
      {
         const uint8_t* data            = nullptr;
         uint32_t       count           = 0;
         uint32_t       elem_stride     = 0;  // bytes per element
         uint16_t       elem_data_words = 0;  // for composite lists only
         uint16_t       elem_ptr_count  = 0;  // for composite lists only
      };

      inline list_info resolve_list_ptr(const uint8_t* ptr_loc)
      {
         uint64_t word = read_u64(ptr_loc);
         if (word == 0)
            return {};

         int32_t  off            = ptr_offset(word);
         uint8_t  elem_sz_tag    = static_cast<uint8_t>((word >> 32) & 7);
         uint32_t count_or_words = static_cast<uint32_t>(word >> 35);

         const uint8_t* list_start = ptr_loc + 8 + static_cast<int64_t>(off) * 8;

         if (elem_sz_tag == 7)
         {
            // Composite list: first word is a tag describing per-element layout
            uint64_t tag   = read_u64(list_start);
            uint32_t count = static_cast<uint32_t>(tag) >> 2;
            uint16_t dw    = static_cast<uint16_t>((tag >> 32) & 0xFFFF);
            uint16_t pc    = static_cast<uint16_t>((tag >> 48) & 0xFFFF);
            return {list_start + 8, count, (dw + pc) * 8u, dw, pc};
         }

         // tag: 0=void, 1=bit, 2=byte, 3=2byte, 4=4byte, 5=8byte, 6=pointer
         static constexpr uint32_t elem_sizes[] = {0, 0, 1, 2, 4, 8, 8, 0};
         return {list_start, count_or_words, elem_sizes[elem_sz_tag], 0, 0};
      }

   }  // namespace capnp_detail

   // ── Type classification helpers ──────────────────────────────────────────

   namespace capnp_detail
   {
      template <typename T>
      struct is_vector : std::false_type {};

      template <typename E>
      struct is_vector<std::vector<E>> : std::true_type
      {
         using element_type = E;
      };

      template <typename T>
      struct is_variant_type : std::false_type {};

      template <typename... Ts>
      struct is_variant_type<std::variant<Ts...>> : std::true_type {};

      // Is this a data-section field (scalar/enum/bool)?
      template <typename F>
      consteval bool is_data_type()
      {
         return std::is_arithmetic_v<F> || std::is_enum_v<F>;
      }

      // Data field byte size (0 for bool = bit-level)
      template <typename F>
      consteval uint32_t data_byte_size()
      {
         if constexpr (std::is_same_v<F, bool>)
            return 0;  // sentinel: bool uses 1 bit
         else if constexpr (std::is_enum_v<F>)
            return sizeof(std::underlying_type_t<F>);
         else if constexpr (std::is_arithmetic_v<F>)
            return sizeof(F);
         else
            return 0;  // pointer type, not in data section
      }

      // Map a variant alternative type to its zero-copy view type.
      // arithmetic/enum/bool → same type
      // std::string → std::string_view
      // std::vector<E> → vec_view<E, cp>
      // std::monostate → std::monostate
      // Reflected struct → view<A, cp>
      template <typename A, typename = void>
      struct alt_view_type_impl
      {
         using type = A;
      };
      template <>
      struct alt_view_type_impl<std::string>
      {
         using type = std::string_view;
      };
      template <>
      struct alt_view_type_impl<std::monostate>
      {
         using type = std::monostate;
      };
      template <typename E>
      struct alt_view_type_impl<std::vector<E>>
      {
         using type = vec_view<E, cp>;
      };
      template <typename A>
      struct alt_view_type_impl<
          A,
          std::enable_if_t<Reflected<A> && !std::is_enum_v<A> && !std::is_arithmetic_v<A>>>
      {
         using type = view<A, cp>;
      };

      template <typename A>
      using alt_view_t = typename alt_view_type_impl<A>::type;

      // Map std::variant<Ts...> to std::variant<alt_view_t<Ts>...>
      template <typename V>
      struct variant_result_type;
      template <typename... Ts>
      struct variant_result_type<std::variant<Ts...>>
      {
         using type = std::variant<alt_view_t<Ts>...>;
      };
      template <typename V>
      using variant_result_t = typename variant_result_type<V>::type;

   }  // namespace capnp_detail

   // ── capnp_layout<T>: compile-time struct layout ──────────────────────────
   //
   // Computes field positions matching the capnp compiler's slot allocation:
   //   - Data fields go in the data section at naturally-aligned offsets
   //   - Pointer fields go in the pointer section sequentially
   //   - Bools are packed as individual bits
   //   - Smaller fields fill gaps left by larger fields
   //   - Variant fields expand into N ordinals (one per alternative)
   //   - Discriminants are allocated after all ordinal fields

   template <typename T>
      requires Reflected<T>
   struct capnp_layout
   {
      struct field_loc
      {
         bool     is_ptr    = false;  // true = pointer section, false = data section
         uint32_t offset    = 0;      // byte offset in data section, or word index in ptr section
         uint8_t  bit_index = 0;      // for bool: bit within the byte (0-7)
      };

      static constexpr size_t num_members = std::tuple_size_v<struct_tuple_t<T>>;

      struct layout_result
      {
         // Non-variant fields: location.  Variant fields: discriminant location.
         field_loc fields[64] = {};

         // Variant support
         bool      is_variant[64]     = {};
         uint8_t   var_alt_start[64]  = {};   // start index into var_alt_locs
         uint8_t   var_alt_count[64]  = {};   // number of alternatives
         field_loc var_alt_locs[128]  = {};    // packed alternative locations

         uint16_t data_words = 0;
         uint16_t ptr_count  = 0;
      };

      static consteval layout_result compute()
      {
         return apply_members(
             (typename reflect<T>::data_members*)nullptr,
             [](auto... members)
             {
                layout_result r{};

                // Bit-level occupancy for data section (max 32 words = 2048 bits)
                bool occupied[2048] = {};

                auto alloc_bits = [&](uint32_t bit_count, uint32_t bit_align) -> uint32_t
                {
                   for (uint32_t bit = 0;; bit += bit_align)
                   {
                      uint32_t end_bit      = bit + bit_count;
                      uint32_t words_needed = (end_bit + 63) / 64;
                      if (words_needed > r.data_words)
                         r.data_words = words_needed;

                      bool ok = true;
                      for (uint32_t b = bit; b < end_bit; ++b)
                      {
                         if (occupied[b])
                         {
                            ok = false;
                            break;
                         }
                      }
                      if (ok)
                      {
                         for (uint32_t b = bit; b < end_bit; ++b)
                            occupied[b] = true;
                         return bit;
                      }
                   }
                };

                // Allocate a slot for a single type (one capnp ordinal)
                auto alloc_type_slot = [&]<typename F>() -> field_loc
                {
                   if constexpr (std::is_same_v<F, std::monostate>)
                   {
                      // Void: no space
                      return {};
                   }
                   else if constexpr (!capnp_detail::is_data_type<F>())
                   {
                      // Pointer type (string, vector, struct)
                      return {true, r.ptr_count++, 0};
                   }
                   else if constexpr (std::is_same_v<F, bool>)
                   {
                      uint32_t bit = alloc_bits(1, 1);
                      return {false, bit / 8, static_cast<uint8_t>(bit % 8)};
                   }
                   else
                   {
                      uint32_t sz  = capnp_detail::data_byte_size<F>();
                      uint32_t bit = alloc_bits(sz * 8, sz * 8);
                      return {false, bit / 8, 0};
                   }
                };

                uint8_t var_alt_idx = 0;
                size_t  field_idx   = 0;

                // Process each reflect field.  Variants expand into N ordinals.
                (
                    [&]
                    {
                       using F =
                           std::remove_cvref_t<decltype(std::declval<T>().*members)>;
                       size_t i = field_idx++;

                       if constexpr (capnp_detail::is_variant_type<F>::value)
                       {
                          // Variant field: allocate one ordinal per alternative
                          r.is_variant[i]     = true;
                          r.var_alt_start[i]  = var_alt_idx;
                          constexpr size_t N  = std::variant_size_v<F>;
                          r.var_alt_count[i]  = static_cast<uint8_t>(N);

                          [&]<size_t... Js>(std::index_sequence<Js...>)
                          {
                             (
                                 [&]
                                 {
                                    using A = std::variant_alternative_t<Js, F>;
                                    r.var_alt_locs[var_alt_idx++] =
                                        alloc_type_slot.template operator()<A>();
                                 }(),
                                 ...);
                          }(std::make_index_sequence<N>{});
                       }
                       else
                       {
                          r.fields[i] = alloc_type_slot.template operator()<F>();
                       }
                    }(),
                    ...);

                // Allocate discriminant (uint16_t) for each variant field,
                // placed after all ordinal fields (matches official compiler).
                for (size_t i = 0; i < sizeof...(members); ++i)
                {
                   if (r.is_variant[i])
                   {
                      uint32_t bit = alloc_bits(16, 16);
                      r.fields[i]  = {false, bit / 8, 0};
                   }
                }

                return r;
             });
      }

      static constexpr auto result = compute();

      static constexpr uint16_t data_words = result.data_words;
      static constexpr uint16_t ptr_count  = result.ptr_count;

      static consteval field_loc loc(size_t i) { return result.fields[i]; }
      static consteval bool      is_var(size_t i) { return result.is_variant[i]; }
      static consteval uint8_t   alt_count(size_t i) { return result.var_alt_count[i]; }
      static consteval field_loc alt_loc(size_t i, size_t j)
      {
         return result.var_alt_locs[result.var_alt_start[i] + j];
      }
   };

   // ══════════════════════════════════════════════════════════════════════════
   // vec_view<E, cp> — zero-copy list view (Cap'n Proto format)
   //
   // Handles both scalar lists and composite (struct) lists.
   // ══════════════════════════════════════════════════════════════════════════

   template <typename E>
   class vec_view<E, cp>
   {
      const uint8_t* data_  = nullptr;
      uint32_t       count_ = 0;
      uint32_t       stride_ = 0;           // bytes per element
      uint16_t       elem_data_words_ = 0;  // for composite lists
      uint16_t       elem_ptr_count_  = 0;  // for composite lists

     public:
      vec_view() = default;

      // Scalar/pointer list constructor with explicit stride
      vec_view(const uint8_t* data, uint32_t count, uint32_t stride)
          : data_(data), count_(count), stride_(stride)
      {
      }

      // Composite list constructor (list of structs)
      vec_view(const uint8_t* data, uint32_t count, uint16_t dw, uint16_t pc)
          : data_(data), count_(count), stride_((dw + pc) * 8), elem_data_words_(dw), elem_ptr_count_(pc)
      {
      }

      explicit operator bool() const { return data_ != nullptr; }

      uint32_t size() const { return count_; }
      bool     empty() const { return count_ == 0; }

      auto operator[](uint32_t i) const
      {
         const uint8_t* entry = data_ + i * stride_;
         if constexpr (std::is_same_v<E, std::string>)
         {
            // List of Text: each element is a pointer
            return capnp_detail::read_text(entry);
         }
         else if constexpr (std::is_same_v<E, bool>)
         {
            // List of Bool: packed as bytes (1 bool per byte in our simplified model)
            return (*entry) != 0;
         }
         else if constexpr (std::is_enum_v<E>)
         {
            using U = std::underlying_type_t<E>;
            U val;
            std::memcpy(&val, entry, sizeof(U));
            return static_cast<E>(val);
         }
         else if constexpr (std::is_arithmetic_v<E>)
         {
            E val;
            std::memcpy(&val, entry, sizeof(E));
            return val;
         }
         else if constexpr (capnp_detail::is_vector<E>::value)
         {
            // Nested list: each element is a pointer to another list
            using Inner = typename capnp_detail::is_vector<E>::element_type;
            auto info   = capnp_detail::resolve_list_ptr(entry);
            if (!info.data)
               return vec_view<Inner, cp>{};
            if constexpr (Reflected<Inner> && !std::is_enum_v<Inner>)
               return vec_view<Inner, cp>(info.data, info.count,
                                          info.elem_data_words, info.elem_ptr_count);
            else
               return vec_view<Inner, cp>(info.data, info.count, info.elem_stride);
         }
         else if constexpr (Reflected<E>)
         {
            return view<E, cp>(capnp_ptr{entry, elem_data_words_, elem_ptr_count_});
         }
         else
         {
            static_assert(!sizeof(E*), "unsupported list element type for capnp view");
         }
      }

      auto at(uint32_t i) const { return operator[](i); }

      struct read_fn
      {
         auto operator()(const vec_view& v, uint32_t i) const { return v[i]; }
      };
      using iterator = index_iterator<vec_view, read_fn>;

      iterator begin() const { return {this, 0}; }
      iterator end() const { return {this, count_}; }
   };

   // ── Default value XOR ─────────────────────────────────────────────────────
   //
   // Cap'n Proto XORs non-zero default values into the wire representation.
   // To read the actual value: actual = wire_value XOR default_value.
   // For floats, the XOR is bitwise (reinterpret as integer, XOR, reinterpret).
   //
   // Default values come from the C++ struct's default member initializers.
   // When all defaults are zero (the common case), XOR is a no-op and the
   // compiler optimizes it away entirely.

   namespace capnp_detail
   {
      // Bitwise XOR of a wire value with its default.  For integers this is
      // plain ^; for floats we reinterpret through an integer of matching width.
      template <typename F>
      F xor_default(F wire, F def)
      {
         if constexpr (std::is_same_v<F, bool>)
         {
            return wire != def;
         }
         else if constexpr (std::is_same_v<F, float>)
         {
            uint32_t w, d;
            std::memcpy(&w, &wire, 4);
            std::memcpy(&d, &def, 4);
            w ^= d;
            float r;
            std::memcpy(&r, &w, 4);
            return r;
         }
         else if constexpr (std::is_same_v<F, double>)
         {
            uint64_t w, d;
            std::memcpy(&w, &wire, 8);
            std::memcpy(&d, &def, 8);
            w ^= d;
            double r;
            std::memcpy(&r, &w, 8);
            return r;
         }
         else  // integral or enum underlying type
         {
            return static_cast<F>(wire ^ def);
         }
      }

      // Get the default value of the Nth data field of T by default-
      // constructing T once and reading the member.  Cached in a static
      // local so the construction happens at most once per (T, N).
      template <typename T, size_t N, typename F>
      F field_default()
      {
         static const F val = apply_members(
             (typename reflect<T>::data_members*)nullptr,
             [](auto... members)
             {
                T    obj{};
                auto tup = std::make_tuple(members...);
                auto mp  = std::get<N>(tup);
                if constexpr (std::is_enum_v<F>)
                   return static_cast<F>(obj.*mp);
                else
                   return static_cast<F>(obj.*mp);
             });
         return val;
      }
   }  // namespace capnp_detail

   // ── capnp_view_field: read field N from a Cap'n Proto struct ─────────────

   namespace capnp_detail
   {
      // Read a single capnp value given its type and location.
      // Used by both regular fields and variant alternatives.
      template <typename A>
      auto read_capnp_value(capnp_ptr p, typename capnp_layout<
                            std::remove_cvref_t<decltype(*static_cast<A*>(nullptr))>
                            >::field_loc)
         requires false;  // never matches — see specialization below

      // Generic: read a value of type A from location aloc within struct p
      template <typename A, typename LocT>
      auto read_typed_value(capnp_ptr p, LocT aloc)
      {
         if constexpr (std::is_same_v<A, std::monostate>)
         {
            return std::monostate{};
         }
         else if constexpr (std::is_same_v<A, std::string>)
         {
            return read_text(ptr_slot(p, aloc.offset));
         }
         else if constexpr (is_vector<A>::value)
         {
            using E   = typename is_vector<A>::element_type;
            auto  info = resolve_list_ptr(ptr_slot(p, aloc.offset));
            if (!info.data)
               return vec_view<E, cp>{};
            if constexpr (Reflected<E> && !std::is_enum_v<E>)
               return vec_view<E, cp>(info.data, info.count, info.elem_data_words,
                                      info.elem_ptr_count);
            else
               return vec_view<E, cp>(info.data, info.count, info.elem_stride);
         }
         else if constexpr (Reflected<A> && !std::is_enum_v<A>)
         {
            return view<A, cp>(resolve_struct_ptr(ptr_slot(p, aloc.offset)));
         }
         else if constexpr (std::is_same_v<A, bool>)
         {
            return static_cast<bool>((p.data[aloc.offset] >> aloc.bit_index) & 1);
         }
         else if constexpr (std::is_enum_v<A>)
         {
            using U = std::underlying_type_t<A>;
            U wire;
            std::memcpy(&wire, p.data + aloc.offset, sizeof(U));
            return static_cast<A>(wire);
         }
         else if constexpr (std::is_arithmetic_v<A>)
         {
            A wire;
            std::memcpy(&wire, p.data + aloc.offset, sizeof(A));
            return wire;
         }
         else
         {
            static_assert(!sizeof(A*), "unsupported type for capnp view");
         }
      }

      // ── Variant reading helpers ──────────────────────────────────────────

      // Read one alternative of a variant and wrap it in the result variant.
      template <typename Variant, size_t J, typename T, size_t FieldIdx>
      variant_result_t<Variant> read_one_alt(capnp_ptr p)
      {
         using A    = std::variant_alternative_t<J, Variant>;
         using layout = capnp_layout<T>;
         constexpr auto aloc = layout::alt_loc(FieldIdx, J);

         using result_t = variant_result_t<Variant>;
         using view_t   = alt_view_t<A>;

         if constexpr (std::is_same_v<A, std::monostate>)
            return result_t(std::in_place_index<J>, std::monostate{});
         else
            return result_t(std::in_place_index<J>, read_typed_value<A>(p, aloc));
      }

      // Dispatch to the correct alternative based on discriminant value.
      template <typename Variant, typename T, size_t FieldIdx, size_t... Js>
      variant_result_t<Variant> read_variant_dispatch(
          capnp_ptr p, uint16_t disc, std::index_sequence<Js...>)
      {
         using result_t = variant_result_t<Variant>;
         result_t result;
         // Short-circuit fold: assign the matching alternative
         (void)((disc == Js ? (result = read_one_alt<Variant, Js, T, FieldIdx>(p), true)
                            : false) || ...);
         return result;
      }

      // ── Main field reader ───────────────────────────────────────────────

      template <typename T, size_t N>
      auto capnp_view_field(capnp_ptr p)
      {
         using F      = std::tuple_element_t<N, struct_tuple_t<T>>;
         using layout = capnp_layout<T>;

         if constexpr (is_variant_type<F>::value)
         {
            // Variant field: read discriminant, dispatch to active alternative
            constexpr auto disc_loc = layout::loc(N);  // discriminant stored in fields[N]
            uint16_t disc;
            std::memcpy(&disc, p.data + disc_loc.offset, 2);

            constexpr size_t alt_count = std::variant_size_v<F>;
            return read_variant_dispatch<F, T, N>(
                p, disc, std::make_index_sequence<alt_count>{});
         }
         else
         {
            constexpr auto loc = layout::loc(N);

            if constexpr (loc.is_ptr)
            {
               // Pointer field
               const uint8_t* slot = ptr_slot(p, loc.offset);

               if constexpr (std::is_same_v<F, std::string>)
               {
                  return read_text(slot);
               }
               else if constexpr (is_vector<F>::value)
               {
                  using E  = typename is_vector<F>::element_type;
                  auto info = resolve_list_ptr(slot);
                  if (!info.data)
                     return vec_view<E, cp>{};

                  if constexpr (Reflected<E> && !std::is_enum_v<E>)
                     return vec_view<E, cp>(info.data, info.count,
                                            info.elem_data_words, info.elem_ptr_count);
                  else
                     return vec_view<E, cp>(info.data, info.count, info.elem_stride);
               }
               else if constexpr (Reflected<F> && !std::is_enum_v<F>)
               {
                  return view<F, cp>(resolve_struct_ptr(slot));
               }
               else
               {
                  static_assert(!sizeof(F*), "unsupported pointer field type");
               }
            }
            else
            {
               // Data field: read from data section, XOR with default value
               if constexpr (std::is_same_v<F, bool>)
               {
                  bool wire = (p.data[loc.offset] >> loc.bit_index) & 1;
                  return xor_default(wire, field_default<T, N, bool>());
               }
               else if constexpr (std::is_enum_v<F>)
               {
                  using U = std::underlying_type_t<F>;
                  U wire;
                  std::memcpy(&wire, p.data + loc.offset, sizeof(U));
                  U def = static_cast<U>(field_default<T, N, F>());
                  return static_cast<F>(xor_default(wire, def));
               }
               else if constexpr (std::is_arithmetic_v<F>)
               {
                  F wire;
                  std::memcpy(&wire, p.data + loc.offset, sizeof(F));
                  return xor_default(wire, field_default<T, N, F>());
               }
               else
               {
                  static_assert(!sizeof(F*), "unsupported data field type");
               }
            }
         }
      }

   }  // namespace capnp_detail

   // ══════════════════════════════════════════════════════════════════════════
   // struct cp — Cap'n Proto format tag for view<T, cp>
   // ══════════════════════════════════════════════════════════════════════════

   struct cp
   {
      using ptr_t = capnp_ptr;

      template <typename T>
      static ptr_t root(const void* buf)
      {
         auto* msg = static_cast<const uint8_t*>(buf);
         uint32_t seg_count_m1 = capnp_detail::read_u32(msg);
         uint32_t table_bytes = 4 + (seg_count_m1 + 1) * 4;
         table_bytes          = (table_bytes + 7) & ~7u;
         auto* segment        = msg + table_bytes;
         return capnp_detail::resolve_struct_ptr(segment);
      }

      template <typename T, size_t N>
      static auto field(ptr_t p)
      {
         return capnp_detail::capnp_view_field<T, N>(p);
      }
   };

   /// capnp_view<T> — alias for view<T, cp>
   template <typename T>
   using capnp_view = view<T, cp>;

   // ══════════════════════════════════════════════════════════════════════════
   // capnp_unpack — deserialize Cap'n Proto wire data to native C++ struct
   // ══════════════════════════════════════════════════════════════════════════

   namespace capnp_detail
   {
      // Forward declarations for recursive unpacking
      template <typename T>
      void unpack_struct(capnp_ptr p, T& obj);

      // Convert a vec_view to a native vector
      template <typename E>
      auto unpack_vec(vec_view<E, cp> vv)
      {
         if constexpr (is_vector<E>::value)
         {
            // Nested list: recurse
            using Inner = typename is_vector<E>::element_type;
            std::vector<std::vector<Inner>> result;
            result.reserve(vv.size());
            for (uint32_t i = 0; i < vv.size(); ++i)
               result.push_back(unpack_vec<Inner>(vv[i]));
            return result;
         }
         else if constexpr (std::is_same_v<E, std::string>)
         {
            std::vector<std::string> result;
            result.reserve(vv.size());
            for (uint32_t i = 0; i < vv.size(); ++i)
               result.emplace_back(vv[i]);
            return result;
         }
         else if constexpr (Reflected<E> && !std::is_enum_v<E>)
         {
            std::vector<E> result;
            result.reserve(vv.size());
            for (uint32_t i = 0; i < vv.size(); ++i)
            {
               E elem{};
               unpack_struct(vv[i].data(), elem);
               result.push_back(std::move(elem));
            }
            return result;
         }
         else
         {
            // Scalar/enum list
            std::vector<E> result;
            result.reserve(vv.size());
            for (uint32_t i = 0; i < vv.size(); ++i)
               result.push_back(vv[i]);
            return result;
         }
      }

      // Unpack a variant view result back to the native variant type
      template <typename NativeVariant, typename ViewVariant, size_t... Js>
      NativeVariant unpack_variant_impl(const ViewVariant& vv, std::index_sequence<Js...>)
      {
         NativeVariant result;
         size_t        idx = vv.index();
         (void)((idx == Js
                     ? [&]
                   {
                      using A    = std::variant_alternative_t<Js, NativeVariant>;
                      using ViewA = std::variant_alternative_t<Js, ViewVariant>;
                      if constexpr (std::is_same_v<A, std::monostate>)
                         result.template emplace<Js>();
                      else if constexpr (std::is_same_v<A, std::string>)
                         result.template emplace<Js>(std::string(std::get<Js>(vv)));
                      else if constexpr (is_vector<A>::value)
                      {
                         using E = typename is_vector<A>::element_type;
                         result.template emplace<Js>(
                             unpack_vec<E>(std::get<Js>(vv)));
                      }
                      else if constexpr (Reflected<A> && !std::is_enum_v<A>)
                      {
                         A elem{};
                         auto v = std::get<Js>(vv);
                         unpack_struct(v.data(), elem);
                         result.template emplace<Js>(std::move(elem));
                      }
                      else
                         result.template emplace<Js>(std::get<Js>(vv));
                      return true;
                   }()
                     : false) ||
                 ...);
         return result;
      }

      // Unpack one field (N and member pointer always in sync)
      template <typename T, size_t N, typename MemberPtr>
      void unpack_one_field(capnp_ptr p, T& obj, MemberPtr mp)
      {
         using F  = std::remove_cvref_t<decltype(obj.*mp)>;
         auto val = capnp_view_field<T, N>(p);

         if constexpr (is_variant_type<F>::value)
         {
            obj.*mp = unpack_variant_impl<F>(
                val, std::make_index_sequence<std::variant_size_v<F>>{});
         }
         else if constexpr (std::is_same_v<F, std::string>)
         {
            obj.*mp = std::string(val);
         }
         else if constexpr (is_vector<F>::value)
         {
            using E = typename is_vector<F>::element_type;
            obj.*mp = unpack_vec<E>(val);
         }
         else if constexpr (Reflected<F> && !std::is_enum_v<F>)
         {
            unpack_struct(val.data(), obj.*mp);
         }
         else
         {
            obj.*mp = val;
         }
      }

      // Unpack all fields from a capnp_ptr into a native struct T
      template <typename T>
      void unpack_struct(capnp_ptr p, T& obj)
      {
         if (p == nullptr)
            return;

         apply_members(
             (typename reflect<T>::data_members*)nullptr,
             [&]<typename... Ms>(Ms... members)
             {
                auto tup = std::make_tuple(members...);
                [&]<size_t... Is>(std::index_sequence<Is...>)
                {
                   (unpack_one_field<T, Is>(p, obj, std::get<Is>(tup)), ...);
                }(std::make_index_sequence<sizeof...(Ms)>{});
             });
      }
   }  // namespace capnp_detail

   /// Unpack a Cap'n Proto flat-array message into a native C++ struct.
   template <typename T>
      requires Reflected<T>
   T capnp_unpack(const void* buf)
   {
      T    obj{};
      auto p = cp::root<T>(buf);
      capnp_detail::unpack_struct(p, obj);
      return obj;
   }

   // ══════════════════════════════════════════════════════════════════════════
   // capnp_pack — serialize a native C++ struct to Cap'n Proto wire format
   // ══════════════════════════════════════════════════════════════════════════

   namespace capnp_detail
   {
      // Builder that appends words to a flat buffer.
      class capnp_word_buf
      {
         std::vector<uint64_t> words_;

        public:
         uint32_t alloc(uint32_t n)
         {
            uint32_t off = static_cast<uint32_t>(words_.size());
            words_.resize(off + n, 0);
            return off;
         }

         uint8_t* byte_ptr(uint32_t word_idx)
         {
            return reinterpret_cast<uint8_t*>(&words_[word_idx]);
         }

         void write_struct_ptr(uint32_t at, uint32_t target, uint16_t dw, uint16_t pc)
         {
            int32_t  off  = static_cast<int32_t>(target) - static_cast<int32_t>(at) - 1;
            uint64_t word = (uint64_t(pc) << 48) | (uint64_t(dw) << 32) |
                            (static_cast<uint32_t>(off << 2) & 0xFFFFFFFFu);
            words_[at] = word;
         }

         void write_list_ptr(uint32_t at, uint32_t target, uint8_t elem_sz, uint32_t count)
         {
            int32_t  off  = static_cast<int32_t>(target) - static_cast<int32_t>(at) - 1;
            uint64_t word = (uint64_t(count) << 35) | (uint64_t(elem_sz) << 32) |
                            (static_cast<uint32_t>(off << 2) & 0xFFFFFFFFu) | 1u;
            words_[at] = word;
         }

         void write_composite_tag(uint32_t at, uint32_t count, uint16_t dw, uint16_t pc)
         {
            words_[at] = (uint64_t(pc) << 48) | (uint64_t(dw) << 32) |
                         (static_cast<uint32_t>(count) << 2);
         }

         template <typename V>
         void write_field(uint32_t struct_start, uint32_t byte_offset, V val)
         {
            std::memcpy(byte_ptr(struct_start) + byte_offset, &val, sizeof(val));
         }

         void write_bool(uint32_t struct_start, uint32_t byte_offset, uint8_t bit, bool val)
         {
            auto* p = byte_ptr(struct_start) + byte_offset;
            if (val)
               *p |= (1u << bit);
         }

         // Write a text string (NUL terminated). Returns nothing — pointer is set inline.
         void write_text(uint32_t ptr_word, const std::string& text)
         {
            uint32_t len     = static_cast<uint32_t>(text.size());
            uint32_t total   = len + 1;
            uint32_t n_words = (total + 7) / 8;
            uint32_t target  = alloc(n_words);
            write_list_ptr(ptr_word, target, 2, total);
            std::memcpy(byte_ptr(target), text.data(), len);
            byte_ptr(target)[len] = 0;
         }

         void write_text(uint32_t ptr_word, std::string_view text)
         {
            uint32_t len     = static_cast<uint32_t>(text.size());
            uint32_t total   = len + 1;
            uint32_t n_words = (total + 7) / 8;
            uint32_t target  = alloc(n_words);
            write_list_ptr(ptr_word, target, 2, total);
            std::memcpy(byte_ptr(target), text.data(), len);
            byte_ptr(target)[len] = 0;
         }

         // Produce flat-array message: [segment_table][segment_data]
         std::vector<uint8_t> finish()
         {
            uint32_t             seg_size = static_cast<uint32_t>(words_.size());
            std::vector<uint8_t> result(8 + seg_size * 8);
            uint32_t             zero = 0;
            std::memcpy(result.data(), &zero, 4);
            std::memcpy(result.data() + 4, &seg_size, 4);
            std::memcpy(result.data() + 8, words_.data(), seg_size * 8);
            return result;
         }
      };

      // Forward declaration for recursive packing (templated on Builder
      // so the same logic works with both capnp_word_buf and capnp_seg_builder)
      template <typename T, typename Builder>
      void pack_struct(Builder& buf, uint32_t data_start,
                       uint32_t ptrs_start, const T& obj);

      // Capnp element size tag for a scalar type
      template <typename E>
      consteval uint8_t scalar_elem_tag()
      {
         if constexpr (std::is_same_v<E, bool>)
            return 1;  // bit
         else if constexpr (sizeof(E) == 1)
            return 2;  // byte
         else if constexpr (sizeof(E) == 2)
            return 3;  // 2-byte
         else if constexpr (sizeof(E) == 4)
            return 4;  // 4-byte
         else if constexpr (sizeof(E) == 8)
            return 5;  // 8-byte
         else
            static_assert(!sizeof(E*), "unsupported scalar size");
      }

      // Pack a vector into capnp wire format, writing the list pointer at ptr_word
      template <typename E, typename Builder>
      void pack_vec(Builder& buf, uint32_t ptr_word, const std::vector<E>& vec)
      {
         if (vec.empty())
            return;  // null pointer = empty list

         if constexpr (is_vector<E>::value)
         {
            // List of lists: outer is list of pointers (elem_sz=6)
            uint32_t outer = buf.alloc(static_cast<uint32_t>(vec.size()));
            buf.write_list_ptr(ptr_word, outer, 6, static_cast<uint32_t>(vec.size()));
            for (uint32_t i = 0; i < vec.size(); ++i)
               pack_vec(buf, outer + i, vec[i]);
         }
         else if constexpr (std::is_same_v<E, std::string>)
         {
            // List of Text: list of pointers
            uint32_t outer = buf.alloc(static_cast<uint32_t>(vec.size()));
            buf.write_list_ptr(ptr_word, outer, 6, static_cast<uint32_t>(vec.size()));
            for (uint32_t i = 0; i < vec.size(); ++i)
               buf.write_text(outer + i, vec[i]);
         }
         else if constexpr (Reflected<E> && !std::is_enum_v<E>)
         {
            // List of structs: composite list
            using EL         = capnp_layout<E>;
            uint32_t words_per = EL::data_words + EL::ptr_count;
            uint32_t count     = static_cast<uint32_t>(vec.size());

            uint32_t tag = buf.alloc(1);
            buf.write_composite_tag(tag, count, EL::data_words, EL::ptr_count);

            // Pre-allocate all elements contiguously
            uint32_t first_elem = buf.alloc(count * words_per);

            buf.write_list_ptr(ptr_word, tag, 7, count * words_per);

            for (uint32_t i = 0; i < count; ++i)
            {
               uint32_t elem_start = first_elem + i * words_per;
               pack_struct(buf, elem_start, elem_start + EL::data_words, vec[i]);
            }
         }
         else if constexpr (std::is_same_v<E, bool>)
         {
            // List of Bool: packed bits (simplified as bytes for now)
            uint32_t n_words = (static_cast<uint32_t>(vec.size()) + 7) / 8;
            uint32_t target  = buf.alloc(n_words);
            buf.write_list_ptr(ptr_word, target, 1, static_cast<uint32_t>(vec.size()));
            for (uint32_t i = 0; i < vec.size(); ++i)
            {
               if (vec[i])
                  buf.byte_ptr(target)[i / 8] |= (1u << (i % 8));
            }
         }
         else
         {
            // Scalar list
            constexpr uint32_t elem_sz = sizeof(E);
            uint32_t count             = static_cast<uint32_t>(vec.size());
            uint32_t total_bytes       = count * elem_sz;
            uint32_t n_words           = (total_bytes + 7) / 8;
            uint32_t target            = buf.alloc(n_words);

            if constexpr (std::is_enum_v<E>)
            {
               using U = std::underlying_type_t<E>;
               buf.write_list_ptr(ptr_word, target, scalar_elem_tag<U>(), count);
               for (uint32_t i = 0; i < count; ++i)
               {
                  U val = static_cast<U>(vec[i]);
                  std::memcpy(buf.byte_ptr(target) + i * sizeof(U), &val, sizeof(U));
               }
            }
            else
            {
               buf.write_list_ptr(ptr_word, target, scalar_elem_tag<E>(), count);
               std::memcpy(buf.byte_ptr(target), vec.data(), total_bytes);
            }
         }
      }

      // Pack a single value of type A into the given location
      template <typename T, typename A, size_t FieldIdx, typename Builder>
      void pack_one_value(Builder& buf, uint32_t data_start,
                          uint32_t ptrs_start, const A& val,
                          typename capnp_layout<T>::field_loc aloc)
      {
         if constexpr (std::is_same_v<A, std::monostate>)
         {
            // Void: nothing to write
         }
         else if constexpr (std::is_same_v<A, std::string>)
         {
            buf.write_text(ptrs_start + aloc.offset, val);
         }
         else if constexpr (is_vector<A>::value)
         {
            pack_vec(buf, ptrs_start + aloc.offset, val);
         }
         else if constexpr (Reflected<A> && !std::is_enum_v<A>)
         {
            using AL     = capnp_layout<A>;
            uint32_t cd  = buf.alloc(AL::data_words);
            uint32_t cp  = buf.alloc(AL::ptr_count);
            buf.write_struct_ptr(ptrs_start + aloc.offset, cd, AL::data_words, AL::ptr_count);
            pack_struct(buf, cd, cp, val);
         }
         else if constexpr (std::is_same_v<A, bool>)
         {
            buf.write_bool(data_start, aloc.offset, aloc.bit_index, val);
         }
         else if constexpr (std::is_enum_v<A>)
         {
            using U = std::underlying_type_t<A>;
            U wire  = static_cast<U>(val);
            buf.write_field(data_start, aloc.offset, wire);
         }
         else if constexpr (std::is_arithmetic_v<A>)
         {
            buf.write_field(data_start, aloc.offset, val);
         }
      }

      // Pack one field (N and member pointer always in sync)
      template <typename T, size_t N, typename MemberPtr, typename Builder>
      void pack_one_field(Builder& buf, uint32_t data_start,
                          uint32_t ptrs_start, const T& obj, MemberPtr mp)
      {
         using F      = std::remove_cvref_t<decltype(obj.*mp)>;
         using layout = capnp_layout<T>;
         const auto& field_val = obj.*mp;

         if constexpr (is_variant_type<F>::value)
         {
            constexpr auto disc_loc = layout::loc(N);
            uint16_t disc = static_cast<uint16_t>(field_val.index());
            buf.write_field(data_start, disc_loc.offset, disc);

            [&]<size_t... Js>(std::index_sequence<Js...>)
            {
               ((field_val.index() == Js
                     ? [&]
                   {
                      constexpr auto aloc = layout::alt_loc(N, Js);
                      using A = std::variant_alternative_t<Js, F>;
                      pack_one_value<T, A, N>(
                          buf, data_start, ptrs_start,
                          std::get<Js>(field_val), aloc);
                      return true;
                   }()
                     : false) ||
                ...);
            }(std::make_index_sequence<std::variant_size_v<F>>{});
         }
         else
         {
            constexpr auto loc = layout::loc(N);

            if constexpr (loc.is_ptr)
            {
               if constexpr (std::is_same_v<F, std::string>)
               {
                  if (!field_val.empty())
                     buf.write_text(ptrs_start + loc.offset, field_val);
               }
               else if constexpr (is_vector<F>::value)
               {
                  pack_vec(buf, ptrs_start + loc.offset, field_val);
               }
               else if constexpr (Reflected<F> && !std::is_enum_v<F>)
               {
                  using FL     = capnp_layout<F>;
                  uint32_t cd  = buf.alloc(FL::data_words);
                  uint32_t cp_ = buf.alloc(FL::ptr_count);
                  buf.write_struct_ptr(ptrs_start + loc.offset,
                                       cd, FL::data_words, FL::ptr_count);
                  pack_struct(buf, cd, cp_, field_val);
               }
            }
            else
            {
               if constexpr (std::is_same_v<F, bool>)
               {
                  bool actual = field_val;
                  bool def    = field_default<T, N, bool>();
                  buf.write_bool(data_start, loc.offset, loc.bit_index,
                                 xor_default(actual, def));
               }
               else if constexpr (std::is_enum_v<F>)
               {
                  using U = std::underlying_type_t<F>;
                  U actual = static_cast<U>(field_val);
                  U def    = static_cast<U>(field_default<T, N, F>());
                  buf.write_field(data_start, loc.offset,
                                  xor_default(actual, def));
               }
               else if constexpr (std::is_arithmetic_v<F>)
               {
                  F actual = field_val;
                  F def    = field_default<T, N, F>();
                  buf.write_field(data_start, loc.offset,
                                  xor_default(actual, def));
               }
            }
         }
      }

      // Pack all fields of struct T
      template <typename T, typename Builder>
      void pack_struct(Builder& buf, uint32_t data_start,
                       uint32_t ptrs_start, const T& obj)
      {
         apply_members(
             (typename reflect<T>::data_members*)nullptr,
             [&]<typename... Ms>(Ms... members)
             {
                auto tup = std::make_tuple(members...);
                [&]<size_t... Is>(std::index_sequence<Is...>)
                {
                   (pack_one_field<T, Is>(buf, data_start, ptrs_start, obj,
                                          std::get<Is>(tup)),
                    ...);
                }(std::make_index_sequence<sizeof...(Ms)>{});
             });
      }
   }  // namespace capnp_detail

   /// Serialize a native C++ struct to Cap'n Proto flat-array format.
   template <typename T>
      requires Reflected<T>
   std::vector<uint8_t> capnp_pack(const T& obj)
   {
      capnp_detail::capnp_word_buf buf;
      using L = capnp_layout<T>;

      uint32_t root_ptr   = buf.alloc(1);
      uint32_t data_start = buf.alloc(L::data_words);
      uint32_t ptrs_start = buf.alloc(L::ptr_count);
      buf.write_struct_ptr(root_ptr, data_start, L::data_words, L::ptr_count);

      capnp_detail::pack_struct(buf, data_start, ptrs_start, obj);

      return buf.finish();
   }

   // ══════════════════════════════════════════════════════════════════════════
   // capnp_validate — bounds-check a Cap'n Proto flat-array message
   // ══════════════════════════════════════════════════════════════════════════

   namespace capnp_detail
   {
      // Validate that all pointers in a struct stay within [seg_start, seg_end).
      // Uses a word budget (traversal limit) to bound work — prevents cycles and
      // amplification attacks where a small message references the same subtree
      // many times.  The official capnp library uses the same approach.
      //
      // words_left: decremented by the number of words each struct/list occupies.
      //             A cycle or shared-pointer amplification exhausts the budget
      //             and returns false.
      // depth:      caps recursion depth to prevent stack overflow.
      inline bool validate_struct(const uint8_t* seg_start, const uint8_t* seg_end,
                                  capnp_ptr p, int64_t& words_left, int depth = 0)
      {
         if (p == nullptr)
            return true;
         if (depth > 64)
            return false;

         uint32_t struct_words = static_cast<uint32_t>(p.data_words) + p.ptr_count;
         words_left -= struct_words;
         if (words_left < 0)
            return false;

         // Check data section bounds
         const uint8_t* data_end = p.data + static_cast<uint32_t>(p.data_words) * 8;
         if (p.data < seg_start || data_end > seg_end)
            return false;

         // Check pointer section bounds
         const uint8_t* ps     = ptr_section(p);
         const uint8_t* ps_end = ps + static_cast<uint32_t>(p.ptr_count) * 8;
         if (ps < seg_start || ps_end > seg_end)
            return false;

         // Validate each pointer in the pointer section
         for (uint32_t i = 0; i < p.ptr_count; ++i)
         {
            const uint8_t* slot = ps + i * 8;
            uint64_t       word = read_u64(slot);
            if (word == 0)
               continue;

            uint8_t tag = word & 3;
            if (tag == 0)
            {
               // Struct pointer
               auto child = resolve_struct_ptr(slot);
               if (!validate_struct(seg_start, seg_end, child, words_left, depth + 1))
                  return false;
            }
            else if (tag == 1)
            {
               // List pointer
               auto info = resolve_list_ptr(slot);
               if (!info.data)
                  continue;

               // Check list data bounds
               uint32_t       total_bytes = info.count * info.elem_stride;
               const uint8_t* list_end    = info.data + total_bytes;
               if (info.data < seg_start || list_end > seg_end)
                  return false;

               // Charge word budget for the list data
               uint32_t list_words = (total_bytes + 7) / 8;
               words_left -= list_words;
               if (words_left < 0)
                  return false;

               // For composite lists, validate each element struct
               if (info.elem_data_words > 0 || info.elem_ptr_count > 0)
               {
                  for (uint32_t j = 0; j < info.count; ++j)
                  {
                     capnp_ptr elem{info.data + j * info.elem_stride,
                                    info.elem_data_words, info.elem_ptr_count};
                     if (!validate_struct(seg_start, seg_end, elem,
                                          words_left, depth + 1))
                        return false;
                  }
               }
               else if (info.elem_stride == 8)
               {
                  // List of pointers — validate each
                  for (uint32_t j = 0; j < info.count; ++j)
                  {
                     const uint8_t* entry = info.data + j * 8;
                     uint64_t       w     = read_u64(entry);
                     if (w == 0)
                        continue;
                     uint8_t t = w & 3;
                     if (t == 0)
                     {
                        auto child = resolve_struct_ptr(entry);
                        if (!validate_struct(seg_start, seg_end, child,
                                             words_left, depth + 1))
                           return false;
                     }
                     else if (t == 1)
                     {
                        auto inner = resolve_list_ptr(entry);
                        if (inner.data)
                        {
                           uint32_t ib = inner.count * inner.elem_stride;
                           if (inner.data < seg_start || inner.data + ib > seg_end)
                              return false;
                           uint32_t iw = (ib + 7) / 8;
                           words_left -= iw;
                           if (words_left < 0)
                              return false;
                        }
                     }
                  }
               }
            }
            else
            {
               return false;  // far pointers / other not supported
            }
         }
         return true;
      }
   }  // namespace capnp_detail

   /// Validate a Cap'n Proto flat-array message.
   /// Returns true if all pointers are within segment bounds and the
   /// traversal limit is not exceeded.  The traversal limit defaults to
   /// 8x the segment size (matching the official capnp library's default),
   /// which detects cycles and amplification attacks.
   inline bool capnp_validate(const void* buf, size_t len)
   {
      auto* msg = static_cast<const uint8_t*>(buf);
      if (len < 8)
         return false;

      uint32_t seg_count_m1 = capnp_detail::read_u32(msg);
      if (seg_count_m1 != 0)
         return false;  // only single-segment messages supported

      uint32_t seg_words = capnp_detail::read_u32(msg + 4);
      uint32_t table_bytes = 8;

      if (len < table_bytes + static_cast<size_t>(seg_words) * 8)
         return false;

      const uint8_t* seg_start = msg + table_bytes;
      const uint8_t* seg_end   = seg_start + static_cast<size_t>(seg_words) * 8;

      if (seg_words == 0)
         return false;

      // Traversal limit: 8x segment size (same as official capnp default)
      int64_t words_left = static_cast<int64_t>(seg_words) * 8;

      auto root = capnp_detail::resolve_struct_ptr(seg_start);
      return capnp_detail::validate_struct(seg_start, seg_end, root, words_left);
   }

   // ══════════════════════════════════════════════════════════════════════════
   // capnp_ref — mutable reference over Cap'n Proto wire data
   //
   // Provides field-level in-place mutation of capnp flat-array messages.
   // Scalar fields are overwritten in-place.  Pointer fields (strings, vectors,
   // nested structs) allocate new space at the end of the segment and repoint
   // the pointer — old data becomes dead space.  No sibling/ancestor offset
   // patching is needed (unlike fracpack) because capnp pointers are
   // self-contained relative offsets.
   //
   // Usage:
   //   auto data = capnp_pack(my_obj);
   //   capnp_ref<MyStruct> ref(std::move(data));
   //   auto f = ref.fields();
   //   uint32_t id = f.id();         // read via implicit conversion
   //   f.id() = 42;                  // write scalar in-place
   //   f.name() = "new name";        // write string (grows segment)
   //   f.items() = {1, 2, 3};        // write vector (grows segment)
   //   f.customer().age() = 30;      // drill into nested struct
   //
   // Buffer capability tiers (deduced from Buffer type):
   //   span<const uint8_t>   → read-only
   //   span<uint8_t>         → read + scalar overwrite
   //   vector<uint8_t>       → read + full mutation (grow segment)
   // ══════════════════════════════════════════════════════════════════════════

   namespace capnp_detail
   {
      // ── Buffer capability traits ─────────────────────────────────────────

      template <typename Buffer>
      struct capnp_buf_traits
      {
         static constexpr bool is_const = []
         {
            if constexpr (requires(Buffer& b) { b.data(); })
               return std::is_const_v<
                   std::remove_pointer_t<decltype(std::declval<Buffer>().data())>>;
            else
               return true;
         }();
         static constexpr bool can_resize =
             requires(Buffer& b) { b.resize(size_t{}); };
         static constexpr bool can_write = !is_const;
         static constexpr bool can_grow  = can_write && can_resize;
      };

      // ── Segment builder for mutation ─────────────────────────────────────
      //
      // Same interface as capnp_word_buf but operates on an existing flat-array
      // message (header + segment).  Word indices are within the segment.

      class capnp_seg_builder
      {
         std::vector<uint8_t>& msg_;

        public:
         explicit capnp_seg_builder(std::vector<uint8_t>& msg) : msg_(msg) {}

         uint32_t seg_words() const
         {
            uint32_t w;
            std::memcpy(&w, msg_.data() + 4, 4);
            return w;
         }

         uint32_t alloc(uint32_t n)
         {
            uint32_t off       = seg_words();
            uint32_t new_words = off + n;
            msg_.resize(8 + static_cast<size_t>(new_words) * 8, 0);
            std::memcpy(msg_.data() + 4, &new_words, 4);
            return off;
         }

         uint8_t* byte_ptr(uint32_t word_idx)
         {
            return msg_.data() + 8 + static_cast<size_t>(word_idx) * 8;
         }

         void write_struct_ptr(uint32_t at, uint32_t target, uint16_t dw,
                               uint16_t pc)
         {
            int32_t  off  = static_cast<int32_t>(target) - static_cast<int32_t>(at) - 1;
            uint64_t word = (uint64_t(pc) << 48) | (uint64_t(dw) << 32) |
                            (static_cast<uint32_t>(off << 2) & 0xFFFFFFFFu);
            std::memcpy(byte_ptr(at), &word, 8);
         }

         void write_list_ptr(uint32_t at, uint32_t target, uint8_t elem_sz,
                             uint32_t count)
         {
            int32_t  off  = static_cast<int32_t>(target) - static_cast<int32_t>(at) - 1;
            uint64_t word = (uint64_t(count) << 35) | (uint64_t(elem_sz) << 32) |
                            (static_cast<uint32_t>(off << 2) & 0xFFFFFFFFu) | 1u;
            std::memcpy(byte_ptr(at), &word, 8);
         }

         void write_composite_tag(uint32_t at, uint32_t count, uint16_t dw,
                                  uint16_t pc)
         {
            uint64_t word = (uint64_t(pc) << 48) | (uint64_t(dw) << 32) |
                            (static_cast<uint32_t>(count) << 2);
            std::memcpy(byte_ptr(at), &word, 8);
         }

         template <typename V>
         void write_field(uint32_t struct_start, uint32_t byte_offset, V val)
         {
            std::memcpy(byte_ptr(struct_start) + byte_offset, &val, sizeof(val));
         }

         void write_bool(uint32_t struct_start, uint32_t byte_offset,
                         uint8_t bit, bool val)
         {
            auto* p = byte_ptr(struct_start) + byte_offset;
            if (val)
               *p |= (1u << bit);
            else
               *p &= ~(1u << bit);
         }

         void write_text(uint32_t ptr_word, std::string_view text)
         {
            uint32_t len     = static_cast<uint32_t>(text.size());
            uint32_t total   = len + 1;
            uint32_t n_words = (total + 7) / 8;
            uint32_t target  = alloc(n_words);
            write_list_ptr(ptr_word, target, 2, total);
            std::memcpy(byte_ptr(target), text.data(), len);
            byte_ptr(target)[len] = 0;
         }

         void write_text(uint32_t ptr_word, const std::string& text)
         {
            write_text(ptr_word, std::string_view(text));
         }
      };

      // ── capnp_field_handle ───────────────────────────────────────────────
      //
      // Read/write handle for a single field of a capnp struct.
      // Reading returns the zero-copy view type; writing accepts the native type.

      template <typename T, typename Buffer>
      class capnp_proxy_obj;  // forward decl

      template <typename Root, typename FieldType, typename Buffer, size_t FieldIdx>
      class capnp_field_handle
      {
         Buffer*  buf_;
         uint32_t struct_data_byte_;  // byte offset of struct data section from seg start
         uint16_t data_words_;
         uint16_t ptr_count_;

         static constexpr bool can_write = capnp_buf_traits<Buffer>::can_write;
         static constexpr bool can_grow  = capnp_buf_traits<Buffer>::can_grow;

         capnp_ptr make_ptr() const
         {
            auto* seg = reinterpret_cast<const uint8_t*>(buf_->data()) + 8;
            return {seg + struct_data_byte_, data_words_, ptr_count_};
         }

         uint32_t data_start_word() const { return struct_data_byte_ / 8; }
         uint32_t ptrs_start_word() const { return data_start_word() + data_words_; }

        public:
         capnp_field_handle(Buffer* b, uint32_t sdb, uint16_t dw, uint16_t pc)
             : buf_(b), struct_data_byte_(sdb), data_words_(dw), ptr_count_(pc)
         {
         }

         // ── Read ──────────────────────────────────────────────────────────
         auto get() const { return capnp_view_field<Root, FieldIdx>(make_ptr()); }

         using view_type =
             decltype(capnp_view_field<Root, FieldIdx>(std::declval<capnp_ptr>()));
         operator view_type() const { return get(); }

         // ── Write: data-section scalar/enum/bool (in-place) ───────────────
         capnp_field_handle& operator=(const FieldType& v)
            requires(is_data_type<FieldType>() && can_write &&
                     !is_variant_type<FieldType>::value)
         {
            using layout       = capnp_layout<Root>;
            constexpr auto loc = layout::loc(FieldIdx);
            static_assert(!loc.is_ptr, "data field must be in data section");

            auto* data =
                reinterpret_cast<uint8_t*>(buf_->data()) + 8 + struct_data_byte_;

            if constexpr (std::is_same_v<FieldType, bool>)
            {
               bool wire = xor_default(v, field_default<Root, FieldIdx, bool>());
               if (wire)
                  data[loc.offset] |= (1u << loc.bit_index);
               else
                  data[loc.offset] &= ~(1u << loc.bit_index);
            }
            else if constexpr (std::is_enum_v<FieldType>)
            {
               using U = std::underlying_type_t<FieldType>;
               U def   = static_cast<U>(field_default<Root, FieldIdx, FieldType>());
               U wire  = xor_default(static_cast<U>(v), def);
               std::memcpy(data + loc.offset, &wire, sizeof(U));
            }
            else
            {
               FieldType wire =
                   xor_default(v, field_default<Root, FieldIdx, FieldType>());
               std::memcpy(data + loc.offset, &wire, sizeof(FieldType));
            }
            return *this;
         }

         // ── Write: string (allocate at end, repoint) ──────────────────────
         capnp_field_handle& operator=(std::string_view v)
            requires(std::is_same_v<FieldType, std::string> && can_grow)
         {
            using layout       = capnp_layout<Root>;
            constexpr auto loc = layout::loc(FieldIdx);
            static_assert(loc.is_ptr, "string field must be a pointer");

            uint32_t ptr_word = ptrs_start_word() + loc.offset;

            if (v.empty())
            {
               // Zero the pointer — represents empty/null string
               uint64_t zero = 0;
               std::memcpy(buf_->data() + 8 + ptr_word * 8, &zero, 8);
               return *this;
            }

            capnp_seg_builder sb(*buf_);
            sb.write_text(ptr_word, v);
            return *this;
         }

         capnp_field_handle& operator=(const std::string& v)
            requires(std::is_same_v<FieldType, std::string> && can_grow)
         {
            return *this = std::string_view(v);
         }

         capnp_field_handle& operator=(const char* v)
            requires(std::is_same_v<FieldType, std::string> && can_grow)
         {
            return *this = std::string_view(v);
         }

         // ── Write: vector (allocate at end, repoint) ──────────────────────
         capnp_field_handle& operator=(const FieldType& v)
            requires(is_vector<FieldType>::value && can_grow)
         {
            using layout       = capnp_layout<Root>;
            constexpr auto loc = layout::loc(FieldIdx);
            static_assert(loc.is_ptr, "vector field must be a pointer");

            uint32_t ptr_word = ptrs_start_word() + loc.offset;

            if (v.empty())
            {
               uint64_t zero = 0;
               std::memcpy(buf_->data() + 8 + ptr_word * 8, &zero, 8);
               return *this;
            }

            capnp_seg_builder sb(*buf_);
            using E = typename is_vector<FieldType>::element_type;
            pack_vec<E>(sb, ptr_word, v);
            return *this;
         }

         // ── Write: nested struct (allocate at end, repoint) ───────────────
         capnp_field_handle& operator=(const FieldType& v)
            requires(Reflected<FieldType> && !std::is_enum_v<FieldType> &&
                     !is_variant_type<FieldType>::value &&
                     !is_data_type<FieldType>() && can_grow)
         {
            using layout       = capnp_layout<Root>;
            constexpr auto loc = layout::loc(FieldIdx);
            static_assert(loc.is_ptr, "struct field must be a pointer");

            capnp_seg_builder sb(*buf_);
            uint32_t          ptr_word = ptrs_start_word() + loc.offset;

            using FL     = capnp_layout<FieldType>;
            uint32_t cd  = sb.alloc(FL::data_words);
            uint32_t cp_ = sb.alloc(FL::ptr_count);
            sb.write_struct_ptr(ptr_word, cd, FL::data_words, FL::ptr_count);
            pack_struct(sb, cd, cp_, v);
            return *this;
         }

         // ── Write: variant (set discriminant + active alternative) ────────
         capnp_field_handle& operator=(const FieldType& v)
            requires(is_variant_type<FieldType>::value && can_grow)
         {
            using layout       = capnp_layout<Root>;
            capnp_seg_builder sb(*buf_);

            uint32_t dw = data_start_word();
            uint32_t pw = ptrs_start_word();

            // Write discriminant
            constexpr auto disc_loc = layout::loc(FieldIdx);
            uint16_t       disc     = static_cast<uint16_t>(v.index());
            sb.write_field(dw, disc_loc.offset, disc);

            // Write the active alternative's value
            [&]<size_t... Js>(std::index_sequence<Js...>)
            {
               ((v.index() == Js
                     ? [&]
                   {
                      constexpr auto aloc = layout::alt_loc(FieldIdx, Js);
                      using A = std::variant_alternative_t<Js, FieldType>;
                      pack_one_value<Root, A, FieldIdx>(sb, dw, pw,
                                                        std::get<Js>(v), aloc);
                      return true;
                   }()
                     : false) ||
                ...);
            }(std::make_index_sequence<std::variant_size_v<FieldType>>{});

            return *this;
         }
      };

      // ── capnp_proxy_obj ──────────────────────────────────────────────────
      //
      // Bridges PSIO_REFLECT's proxy pattern to capnp_field_handle.
      // For struct fields → returns a nested proxy (enabling drill-in).
      // For leaf fields → returns a capnp_field_handle (enabling read/write).

      template <typename T, typename Buffer>
      class capnp_proxy_obj
      {
         Buffer*  buf_;
         uint32_t struct_data_byte_;  // data section byte offset from seg start
         uint16_t data_words_;
         uint16_t ptr_count_;

        public:
         capnp_proxy_obj(Buffer* b, uint32_t sdb, uint16_t dw, uint16_t pc)
             : buf_(b), struct_data_byte_(sdb), data_words_(dw), ptr_count_(pc)
         {
         }

         template <int I, auto MemberPtr>
         decltype(auto) get()
         {
            using F            = std::remove_cvref_t<decltype(std::declval<T>().*MemberPtr)>;
            constexpr size_t idx = static_cast<size_t>(I);

            if constexpr (Reflected<F> && !std::is_enum_v<F> &&
                          !is_variant_type<F>::value && !is_vector<F>::value &&
                          !std::is_arithmetic_v<F> &&
                          !std::is_same_v<F, std::string>)
            {
               // Nested struct → resolve pointer, return nested proxy
               using layout       = capnp_layout<T>;
               constexpr auto loc = layout::loc(idx);
               static_assert(loc.is_ptr, "nested struct must be a pointer field");

               uint32_t ptr_byte =
                   struct_data_byte_ + data_words_ * 8 + loc.offset * 8;
               const auto* seg =
                   reinterpret_cast<const uint8_t*>(buf_->data()) + 8;
               uint64_t word;
               std::memcpy(&word, seg + ptr_byte, 8);

               if (word == 0)
               {
                  // Null pointer — create proxy at offset 0 with zero layout.
                  // Reads return defaults; writes require allocating the struct
                  // first (not yet supported — use operator= on parent).
                  using inner_proxy = capnp_proxy_obj<F, Buffer>;
                  using proxy_type =
                      typename reflect<F>::template proxy<inner_proxy>;
                  return proxy_type{inner_proxy{buf_, 0, 0, 0}};
               }

               int32_t  off = ptr_offset(word);
               uint16_t dw  = static_cast<uint16_t>((word >> 32) & 0xFFFF);
               uint16_t pc  = static_cast<uint16_t>((word >> 48) & 0xFFFF);
               uint32_t target_byte =
                   ptr_byte + 8 + static_cast<int32_t>(off) * 8;

               using inner_proxy = capnp_proxy_obj<F, Buffer>;
               using proxy_type =
                   typename reflect<F>::template proxy<inner_proxy>;
               return proxy_type{inner_proxy{buf_, target_byte, dw, pc}};
            }
            else
            {
               // Leaf field → return field handle
               return capnp_field_handle<T, F, Buffer, idx>{
                   buf_, struct_data_byte_, data_words_, ptr_count_};
            }
         }

         template <int I, auto MemberPtr>
         decltype(auto) get() const
         {
            return const_cast<capnp_proxy_obj*>(this)
                ->template get<I, MemberPtr>();
         }
      };

   }  // namespace capnp_detail

   // ── capnp_ref: top-level typed handle over a capnp message ────────────────

   template <typename T, typename Buffer = std::vector<uint8_t>>
      requires Reflected<T>
   class capnp_ref
   {
      Buffer buf_;

      using proxy_obj_t = capnp_detail::capnp_proxy_obj<T, Buffer>;
      using proxy_t     = typename reflect<T>::template proxy<proxy_obj_t>;

      // Resolve root struct from the flat-array message
      auto root_info() const
      {
         auto* seg = reinterpret_cast<const uint8_t*>(buf_.data()) + 8;
         auto  root = capnp_detail::resolve_struct_ptr(seg);
         return std::tuple{
             static_cast<uint32_t>(root.data - seg), root.data_words,
             root.ptr_count};
      }

     public:
      explicit capnp_ref(Buffer b) : buf_(std::move(b)) {}

      /// Named field accessors.  Each accessor returns either a field_handle
      /// (for leaf fields) or a nested proxy (for struct fields).
      proxy_t fields()
      {
         auto [data_byte, dw, pc] = root_info();
         return proxy_t{proxy_obj_t{&buf_, data_byte, dw, pc}};
      }

      /// Read-only view of the message
      view<T, cp> as_view() const
      {
         auto p = capnp_detail::resolve_struct_ptr(
             reinterpret_cast<const uint8_t*>(buf_.data()) + 8);
         return view<T, cp>(p);
      }

      /// Unpack to native struct
      T unpack() const { return capnp_unpack<T>(buf_.data()); }

      /// Validate the message
      bool validate() const { return capnp_validate(buf_.data(), buf_.size()); }

      /// Raw buffer access
      const uint8_t* data() const
      {
         return reinterpret_cast<const uint8_t*>(buf_.data());
      }
      size_t         size() const { return buf_.size(); }
      Buffer&        buffer() { return buf_; }
      const Buffer&  buffer() const { return buf_; }
   };

}  // namespace psio
