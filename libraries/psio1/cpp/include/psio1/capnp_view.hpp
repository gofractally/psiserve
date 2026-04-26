// capnp_view.hpp — zero-copy read-only views over Cap'n Proto wire data
//
// Implements the unified view<T, cp> format tag, allowing PSIO1_REFLECT'd
// C++ structs to be read directly from Cap'n Proto serialized messages with
// zero overhead and no external library dependency.
//
//   auto v = psio1::capnp_view<Order>::from_buffer(capnp_flat_array);
//   v.id()               // uint64_t
//   v.customer().name()   // string_view
//   v.items()[0].qty()    // uint32_t
//
// Union support: std::variant<A,B,...> fields are read as tagged variants
// with the discriminant placed after all ordinal fields (matching the
// official capnp compiler's layout).
//
// Wire compatibility: layout matches the official capnp compiler's slot
// allocation for structs with sequential ordinals matching PSIO1_REFLECT order.
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

#include <psio1/reflect.hpp>
#include <psio1/view.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

namespace psio1
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
   // validate_capnp — bounds-check a Cap'n Proto flat-array message
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
   inline bool validate_capnp(const void* buf, size_t len)
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

      // ── Free-list allocator for reclaiming dead space ──────────────────
      //
      // When a pointer field is overwritten, the old data becomes unreachable.
      // The free list tracks these dead regions so alloc() can reuse them
      // instead of growing the segment indefinitely.

      class capnp_free_list
      {
         struct block
         {
            uint32_t offset;  // word offset in segment
            uint32_t size;    // size in words
         };
         std::vector<block> blocks_;  // sorted by offset

        public:
         void free(uint32_t offset, uint32_t size)
         {
            if (size == 0)
               return;

            auto it = std::lower_bound(
                blocks_.begin(), blocks_.end(), offset,
                [](const block& b, uint32_t off) { return b.offset < off; });

            // Try to merge with predecessor
            if (it != blocks_.begin())
            {
               auto prev = std::prev(it);
               if (prev->offset + prev->size == offset)
               {
                  prev->size += size;
                  if (it != blocks_.end() &&
                      prev->offset + prev->size == it->offset)
                  {
                     prev->size += it->size;
                     blocks_.erase(it);
                  }
                  return;
               }
            }

            // Try to merge with successor
            if (it != blocks_.end() && offset + size == it->offset)
            {
               it->offset = offset;
               it->size += size;
               return;
            }

            blocks_.insert(it, {offset, size});
         }

         // Returns word offset of a block >= n words, or UINT32_MAX if none
         uint32_t try_alloc(uint32_t n)
         {
            uint32_t best_idx  = UINT32_MAX;
            uint32_t best_size = UINT32_MAX;
            for (uint32_t i = 0; i < static_cast<uint32_t>(blocks_.size()); ++i)
            {
               if (blocks_[i].size >= n && blocks_[i].size < best_size)
               {
                  best_idx  = i;
                  best_size = blocks_[i].size;
                  if (best_size == n)
                     break;
               }
            }

            if (best_idx == UINT32_MAX)
               return UINT32_MAX;

            uint32_t offset = blocks_[best_idx].offset;
            if (blocks_[best_idx].size == n)
               blocks_.erase(blocks_.begin() + best_idx);
            else
            {
               blocks_[best_idx].offset += n;
               blocks_[best_idx].size -= n;
            }
            return offset;
         }

         bool     empty() const { return blocks_.empty(); }
         size_t   block_count() const { return blocks_.size(); }
         uint32_t total_free() const
         {
            uint32_t sum = 0;
            for (auto& b : blocks_)
               sum += b.size;
            return sum;
         }
      };

      // ── Segment builder for mutation ─────────────────────────────────────
      //
      // Same interface as capnp_word_buf but operates on an existing flat-array
      // message (header + segment).  Word indices are within the segment.
      // Optionally backed by a free list for reclaiming dead pointer data.

      class capnp_seg_builder
      {
         std::vector<uint8_t>& msg_;
         capnp_free_list*      free_list_;

        public:
         explicit capnp_seg_builder(std::vector<uint8_t>& msg,
                                    capnp_free_list*      fl = nullptr)
             : msg_(msg), free_list_(fl)
         {
         }

         uint32_t seg_words() const
         {
            uint32_t w;
            std::memcpy(&w, msg_.data() + 4, 4);
            return w;
         }

         uint32_t alloc(uint32_t n)
         {
            // Try to reuse a dead region first
            if (free_list_)
            {
               uint32_t off = free_list_->try_alloc(n);
               if (off != UINT32_MAX)
               {
                  std::memset(byte_ptr(off), 0, static_cast<size_t>(n) * 8);
                  return off;
               }
            }

            uint32_t off       = seg_words();
            uint32_t new_words = off + n;
            msg_.resize(8 + static_cast<size_t>(new_words) * 8, 0);
            std::memcpy(msg_.data() + 4, &new_words, 4);
            return off;
         }

         // Recursively free the allocation pointed to by the pointer at
         // ptr_word.  Handles struct, list (scalar, pointer, composite),
         // and text pointers.  Child pointer allocations are freed first.
         void free_ptr_target(uint32_t ptr_word)
         {
            if (!free_list_)
               return;

            uint64_t word;
            std::memcpy(&word, byte_ptr(ptr_word), 8);
            if (word == 0)
               return;

            uint8_t  tag    = word & 3;
            int32_t  off    = ptr_offset(word);
            uint32_t target = static_cast<uint32_t>(
                static_cast<int32_t>(ptr_word) + 1 + off);

            if (tag == 0)
            {
               // Struct pointer
               uint16_t dw = static_cast<uint16_t>((word >> 32) & 0xFFFF);
               uint16_t pc = static_cast<uint16_t>((word >> 48) & 0xFFFF);

               // Recursively free pointer children first
               uint32_t ptrs_start = target + dw;
               for (uint16_t i = 0; i < pc; ++i)
                  free_ptr_target(ptrs_start + i);

               free_list_->free(target, static_cast<uint32_t>(dw) + pc);
            }
            else if (tag == 1)
            {
               // List pointer
               uint8_t  elem_sz = static_cast<uint8_t>((word >> 32) & 7);
               uint32_t count   = static_cast<uint32_t>(word >> 35);

               if (elem_sz == 7)
               {
                  // Composite list: tag word + body
                  uint64_t tag_word;
                  std::memcpy(&tag_word, byte_ptr(target), 8);
                  uint32_t elem_count =
                      static_cast<uint32_t>(tag_word) >> 2;
                  uint16_t edw =
                      static_cast<uint16_t>((tag_word >> 32) & 0xFFFF);
                  uint16_t epc =
                      static_cast<uint16_t>((tag_word >> 48) & 0xFFFF);
                  uint32_t stride = static_cast<uint32_t>(edw) + epc;

                  for (uint32_t e = 0; e < elem_count; ++e)
                  {
                     uint32_t elem_ptrs =
                         target + 1 + e * stride + edw;
                     for (uint16_t p = 0; p < epc; ++p)
                        free_ptr_target(elem_ptrs + p);
                  }

                  free_list_->free(target, 1 + count);
               }
               else if (elem_sz == 6)
               {
                  // Pointer list: each element is a pointer
                  for (uint32_t i = 0; i < count; ++i)
                     free_ptr_target(target + i);
                  if (count > 0)
                     free_list_->free(target, count);
               }
               else if (elem_sz == 1)
               {
                  // Bit list
                  uint32_t words = (count + 63) / 64;
                  if (words > 0)
                     free_list_->free(target, words);
               }
               else
               {
                  // Scalar list (byte=2, 2-byte=3, 4-byte=4, 8-byte=5)
                  static constexpr uint32_t sizes[] = {0, 0, 1, 2, 4, 8,
                                                       8, 0};
                  uint64_t total_bytes =
                      static_cast<uint64_t>(count) * sizes[elem_sz];
                  uint32_t words =
                      static_cast<uint32_t>((total_bytes + 7) / 8);
                  if (words > 0)
                     free_list_->free(target, words);
               }
            }
            // tag 2,3 = far/inter-segment (not used in flat messages)
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
         Buffer*          buf_;
         capnp_free_list* free_list_;
         uint32_t         struct_data_byte_;  // byte offset of struct data section from seg start
         uint16_t         data_words_;
         uint16_t         ptr_count_;

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
         capnp_field_handle(Buffer* b, capnp_free_list* fl, uint32_t sdb,
                            uint16_t dw, uint16_t pc)
             : buf_(b),
               free_list_(fl),
               struct_data_byte_(sdb),
               data_words_(dw),
               ptr_count_(pc)
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

         // ── Write: string (free old, allocate, repoint) ─────────────────
         capnp_field_handle& operator=(std::string_view v)
            requires(std::is_same_v<FieldType, std::string> && can_grow)
         {
            using layout       = capnp_layout<Root>;
            constexpr auto loc = layout::loc(FieldIdx);
            static_assert(loc.is_ptr, "string field must be a pointer");

            uint32_t          ptr_word = ptrs_start_word() + loc.offset;
            capnp_seg_builder sb(*buf_, free_list_);
            sb.free_ptr_target(ptr_word);

            if (v.empty())
            {
               uint64_t zero = 0;
               std::memcpy(buf_->data() + 8 + ptr_word * 8, &zero, 8);
               return *this;
            }

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

         // ── Write: vector (free old, allocate, repoint) ─────────────────
         capnp_field_handle& operator=(const FieldType& v)
            requires(is_vector<FieldType>::value && can_grow)
         {
            using layout       = capnp_layout<Root>;
            constexpr auto loc = layout::loc(FieldIdx);
            static_assert(loc.is_ptr, "vector field must be a pointer");

            uint32_t          ptr_word = ptrs_start_word() + loc.offset;
            capnp_seg_builder sb(*buf_, free_list_);
            sb.free_ptr_target(ptr_word);

            if (v.empty())
            {
               uint64_t zero = 0;
               std::memcpy(buf_->data() + 8 + ptr_word * 8, &zero, 8);
               return *this;
            }

            using E = typename is_vector<FieldType>::element_type;
            pack_vec<E>(sb, ptr_word, v);
            return *this;
         }

         // ── Write: nested struct (free old, allocate, repoint) ────────────
         capnp_field_handle& operator=(const FieldType& v)
            requires(Reflected<FieldType> && !std::is_enum_v<FieldType> &&
                     !is_variant_type<FieldType>::value &&
                     !is_data_type<FieldType>() && can_grow)
         {
            using layout       = capnp_layout<Root>;
            constexpr auto loc = layout::loc(FieldIdx);
            static_assert(loc.is_ptr, "struct field must be a pointer");

            uint32_t          ptr_word = ptrs_start_word() + loc.offset;
            capnp_seg_builder sb(*buf_, free_list_);
            sb.free_ptr_target(ptr_word);

            using FL     = capnp_layout<FieldType>;
            uint32_t cd  = sb.alloc(FL::data_words);
            uint32_t cp_ = sb.alloc(FL::ptr_count);
            sb.write_struct_ptr(ptr_word, cd, FL::data_words, FL::ptr_count);
            pack_struct(sb, cd, cp_, v);
            return *this;
         }

         // ── Write: variant (free old, set discriminant + alternative) ────
         capnp_field_handle& operator=(const FieldType& v)
            requires(is_variant_type<FieldType>::value && can_grow)
         {
            using layout       = capnp_layout<Root>;
            capnp_seg_builder sb(*buf_, free_list_);

            uint32_t dw = data_start_word();
            uint32_t pw = ptrs_start_word();

            // Read old discriminant and free old alternative's pointer data
            constexpr auto disc_loc = layout::loc(FieldIdx);
            {
               uint16_t old_disc;
               std::memcpy(&old_disc,
                           reinterpret_cast<uint8_t*>(buf_->data()) + 8 +
                               dw * 8 + disc_loc.offset,
                           sizeof(old_disc));

               [&]<size_t... Js>(std::index_sequence<Js...>)
               {
                  ((old_disc == Js
                        ? [&]
                      {
                         constexpr auto aloc =
                             layout::alt_loc(FieldIdx, Js);
                         if constexpr (aloc.is_ptr)
                            sb.free_ptr_target(pw + aloc.offset);
                         return true;
                      }()
                        : false) ||
                   ...);
               }(std::make_index_sequence<
                   std::variant_size_v<FieldType>>{});
            }

            // Write new discriminant
            uint16_t disc = static_cast<uint16_t>(v.index());
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
      // Bridges PSIO1_REFLECT's proxy pattern to capnp_field_handle.
      // For struct fields → returns a nested proxy (enabling drill-in).
      // For leaf fields → returns a capnp_field_handle (enabling read/write).

      template <typename T, typename Buffer>
      class capnp_proxy_obj
      {
         Buffer*          buf_;
         capnp_free_list* free_list_;
         uint32_t         struct_data_byte_;  // data section byte offset from seg start
         uint16_t         data_words_;
         uint16_t         ptr_count_;

        public:
         capnp_proxy_obj(Buffer* b, capnp_free_list* fl, uint32_t sdb,
                         uint16_t dw, uint16_t pc)
             : buf_(b),
               free_list_(fl),
               struct_data_byte_(sdb),
               data_words_(dw),
               ptr_count_(pc)
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
                  return proxy_type{
                      inner_proxy{buf_, free_list_, 0, 0, 0}};
               }

               int32_t  off = ptr_offset(word);
               uint16_t dw  = static_cast<uint16_t>((word >> 32) & 0xFFFF);
               uint16_t pc  = static_cast<uint16_t>((word >> 48) & 0xFFFF);
               uint32_t target_byte =
                   ptr_byte + 8 + static_cast<int32_t>(off) * 8;

               using inner_proxy = capnp_proxy_obj<F, Buffer>;
               using proxy_type =
                   typename reflect<F>::template proxy<inner_proxy>;
               return proxy_type{
                   inner_proxy{buf_, free_list_, target_byte, dw, pc}};
            }
            else
            {
               // Leaf field → return field handle
               return capnp_field_handle<T, F, Buffer, idx>{
                   buf_, free_list_, struct_data_byte_, data_words_,
                   ptr_count_};
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

   // ══════════════════════════════════════════════════════════════════════════
   // Dynamic field access — format-generic runtime name→value navigation
   //
   //   auto dv = dynamic_view(ref);
   //   double score    = dv["score"_f];       // implicit conversion
   //   auto   customer = dv["customer"_f];    // nested struct cursor
   //   std::string_view name = customer["name"_f];
   //   auto   item     = dv["items"_f][3];    // vector element
   //
   // ══════════════════════════════════════════════════════════════════════════

   // ── Constexpr xxh64 — compile-time and runtime field name hashing ─────
   //
   // Follows the same pattern as the existing xxh32.hpp in external/psitri.
   // Produces identical output to XXH64() from the vendored xxhash.h.
   // Used for hash-based schema field lookup in dynamic_view.

   struct xxh64
   {
      static constexpr uint64_t hash(const char* input, size_t len, uint64_t seed = 0)
      {
         return (len >= 32) ? finalize(h32bytes(input, len, seed), input + (len & ~31u), len & 31)
                            : finalize(seed + PRIME5 + len, input, len);
      }

     private:
      static constexpr uint64_t PRIME1 = 0x9E3779B185EBCA87ULL;
      static constexpr uint64_t PRIME2 = 0xC2B2AE3D27D4EB4FULL;
      static constexpr uint64_t PRIME3 = 0x165667B19E3779F9ULL;
      static constexpr uint64_t PRIME4 = 0x85EBCA77C2B2AE63ULL;
      static constexpr uint64_t PRIME5 = 0x27D4EB2F165667C5ULL;

      static constexpr uint64_t rotl(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }

      static constexpr uint64_t round(uint64_t acc, uint64_t input)
      {
         return rotl(acc + input * PRIME2, 31) * PRIME1;
      }

      static constexpr uint64_t merge_round(uint64_t acc, uint64_t val)
      {
         return (acc ^ round(0, val)) * PRIME1 + PRIME4;
      }

      static constexpr uint64_t read64(const char* p)
      {
         return uint64_t(uint8_t(p[0])) | (uint64_t(uint8_t(p[1])) << 8) |
                (uint64_t(uint8_t(p[2])) << 16) | (uint64_t(uint8_t(p[3])) << 24) |
                (uint64_t(uint8_t(p[4])) << 32) | (uint64_t(uint8_t(p[5])) << 40) |
                (uint64_t(uint8_t(p[6])) << 48) | (uint64_t(uint8_t(p[7])) << 56);
      }

      static constexpr uint32_t read32(const char* p)
      {
         return uint32_t(uint8_t(p[0])) | (uint32_t(uint8_t(p[1])) << 8) |
                (uint32_t(uint8_t(p[2])) << 16) | (uint32_t(uint8_t(p[3])) << 24);
      }

      static constexpr uint64_t avalanche(uint64_t h)
      {
         h ^= h >> 33;
         h *= PRIME2;
         h ^= h >> 29;
         h *= PRIME3;
         h ^= h >> 32;
         return h;
      }

      static constexpr uint64_t finalize(uint64_t h, const char* p, size_t len)
      {
         if (len >= 8)
            return finalize(
                rotl(h ^ round(0, read64(p)), 27) * PRIME1 + PRIME4, p + 8, len - 8);
         if (len >= 4)
            return finalize(
                rotl(h ^ (read32(p) * PRIME1), 23) * PRIME2 + PRIME3, p + 4, len - 4);
         if (len > 0)
            return finalize(
                rotl(h ^ (uint8_t(*p) * PRIME5), 11) * PRIME1, p + 1, len - 1);
         return avalanche(h);
      }

      static constexpr uint64_t h32bytes(const char*    p,
                                          size_t         len,
                                          uint64_t       v1,
                                          uint64_t       v2,
                                          uint64_t       v3,
                                          uint64_t       v4)
      {
         if (len >= 32)
            return h32bytes(p + 32, len - 32,
                            round(v1, read64(p)),
                            round(v2, read64(p + 8)),
                            round(v3, read64(p + 16)),
                            round(v4, read64(p + 24)));
         return merge_round(
                    merge_round(
                        merge_round(
                            merge_round(
                                rotl(v1, 1) + rotl(v2, 7) + rotl(v3, 12) + rotl(v4, 18),
                                v1),
                            v2),
                        v3),
                    v4) +
                len;
      }

      static constexpr uint64_t h32bytes(const char* p, size_t len, uint64_t seed)
      {
         return h32bytes(p, len,
                         seed + PRIME1 + PRIME2,
                         seed + PRIME2,
                         seed,
                         seed - PRIME1);
      }
   };

   // Convenience: hash a string_view
   constexpr uint64_t xxh64_hash(std::string_view s)
   {
      return xxh64::hash(s.data(), s.size());
   }

   // ── Field name wrapper ─────────────────────────────────────────────────
   //
   // Produced by the _f literal suffix.  Stores pre-computed hash so
   // schema lookup is a single integer binary search + collision check.
   struct field_name
   {
      std::string_view name;
      uint64_t         hash;
      constexpr field_name(std::string_view n) : name(n), hash(xxh64_hash(n)) {}
      constexpr field_name(std::string_view n, uint64_t h) : name(n), hash(h) {}
   };

   // User-defined literal: "foo"_f → field_name{"foo"}
   constexpr field_name operator""_f(const char* s, size_t len)
   {
      return {std::string_view(s, len)};
   }

   // Type tag for dynamically-accessed values
   enum class dynamic_type : uint8_t
   {
      t_void,
      t_bool,
      t_i8,
      t_i16,
      t_i32,
      t_i64,
      t_u8,
      t_u16,
      t_u32,
      t_u64,
      t_f32,
      t_f64,
      t_text,
      t_data,
      t_vector,
      t_struct,
      t_variant,
   };

   // Rich type descriptor returned by dynamic_view::type()
   struct dynamic_type_info
   {
      dynamic_type kind        = dynamic_type::t_void;
      dynamic_type active_kind = dynamic_type::t_void;  // for variants
      uint8_t      variant_index = 0;                   // for variants
      uint8_t      byte_size     = 0;                   // for scalars
   };

   // ── Runtime schema descriptors ─────────────────────────────────────────

   struct dynamic_schema;  // forward

   // Per-field descriptor for variant alternatives
   struct dynamic_alt_desc
   {
      dynamic_type          type      = dynamic_type::t_void;
      bool                  is_ptr    = false;
      uint32_t              offset    = 0;
      uint8_t               bit_index = 0;
      uint8_t               byte_size = 0;
      const dynamic_schema* nested    = nullptr;
   };

   // Per-field descriptor
   struct dynamic_field_desc
   {
      uint64_t              name_hash = 0;     // xxh64 of field name
      const char*           name      = nullptr;  // original name (collision verify / debug)
      dynamic_type          type      = dynamic_type::t_void;
      bool                  is_ptr    = false;
      uint32_t              offset    = 0;     // byte offset in data section, or ptr index
      uint8_t               bit_index = 0;     // for bools
      uint8_t               byte_size = 0;     // scalar size in bytes (0 for ptr/bool)
      const dynamic_schema* nested    = nullptr;  // for struct fields / struct vector elements

      // Variant support
      const dynamic_alt_desc* alternatives = nullptr;
      uint8_t                 alt_count    = 0;
      uint32_t                disc_offset  = 0;  // discriminant byte offset
   };

   // Maximum fields supported for SIMD tag lookup.
   // Structs with more fields fall back to binary search on hash.
   static constexpr size_t max_simd_fields = 64;

   // Per-type schema: hash-sorted field table with SIMD tag bytes
   struct dynamic_schema
   {
      const dynamic_field_desc* sorted_fields = nullptr;  // sorted by name_hash
      size_t                    field_count    = 0;
      const char* const*        ordered_names = nullptr;  // declaration order
      uint16_t                  data_words     = 0;
      uint16_t                  ptr_count      = 0;
      const uint8_t*            tags           = nullptr;  // low byte of each hash, parallel to sorted_fields

      // Find by pre-computed hash + name (for collision safety)
      const dynamic_field_desc* find(uint64_t hash, std::string_view name) const
      {
         uint8_t tag = static_cast<uint8_t>(hash);

         // Linear scan on tag bytes — SIMD-friendly for typical struct sizes.
         // Most structs have <32 fields; one or two NEON/SSE loads cover it.
         for (size_t i = 0; i < field_count; ++i)
         {
            if (tags[i] == tag)
            {
               if (sorted_fields[i].name_hash == hash &&
                   name == sorted_fields[i].name)
                  return &sorted_fields[i];
            }
         }
         return nullptr;
      }

      // Find by field_name (has pre-computed hash)
      const dynamic_field_desc* find(field_name fn) const
      {
         return find(fn.hash, fn.name);
      }

      // Find by string_view (computes hash on the fly)
      const dynamic_field_desc* find(std::string_view name) const
      {
         return find(xxh64_hash(name), name);
      }
   };

   // ── Capnp schema builder ───────────────────────────────────────────────

   namespace capnp_detail
   {
      template <typename F>
      consteval dynamic_type type_tag_for()
      {
         if constexpr (std::is_same_v<F, bool>)
            return dynamic_type::t_bool;
         else if constexpr (std::is_same_v<F, int8_t>)
            return dynamic_type::t_i8;
         else if constexpr (std::is_same_v<F, int16_t>)
            return dynamic_type::t_i16;
         else if constexpr (std::is_same_v<F, int32_t>)
            return dynamic_type::t_i32;
         else if constexpr (std::is_same_v<F, int64_t>)
            return dynamic_type::t_i64;
         else if constexpr (std::is_same_v<F, uint8_t>)
            return dynamic_type::t_u8;
         else if constexpr (std::is_same_v<F, uint16_t>)
            return dynamic_type::t_u16;
         else if constexpr (std::is_same_v<F, uint32_t>)
            return dynamic_type::t_u32;
         else if constexpr (std::is_same_v<F, uint64_t>)
            return dynamic_type::t_u64;
         else if constexpr (std::is_same_v<F, float>)
            return dynamic_type::t_f32;
         else if constexpr (std::is_same_v<F, double>)
            return dynamic_type::t_f64;
         else if constexpr (std::is_enum_v<F>)
            return type_tag_for<std::underlying_type_t<F>>();
         else if constexpr (std::is_same_v<F, std::string>)
            return dynamic_type::t_text;
         else if constexpr (is_vector<F>::value)
         {
            if constexpr (std::is_same_v<typename is_vector<F>::element_type, uint8_t>)
               return dynamic_type::t_data;
            else
               return dynamic_type::t_vector;
         }
         else if constexpr (is_variant_type<F>::value)
            return dynamic_type::t_variant;
         else if constexpr (Reflected<F>)
            return dynamic_type::t_struct;
         else
            return dynamic_type::t_void;
      }

      template <typename F>
      consteval uint8_t scalar_byte_size()
      {
         if constexpr (std::is_same_v<F, bool>)
            return 0;
         else if constexpr (std::is_enum_v<F>)
            return sizeof(std::underlying_type_t<F>);
         else if constexpr (std::is_arithmetic_v<F>)
            return sizeof(F);
         else
            return 0;
      }
   }  // namespace capnp_detail

   // Forward declaration — each reflected type gets a constexpr schema
   template <typename T>
      requires Reflected<T>
   struct cp_schema;

   namespace capnp_detail
   {
      // Helper: get the nested schema pointer for a type.
      // Returns nullptr for non-struct types.
      template <typename F>
      consteval const dynamic_schema* nested_schema_ptr()
      {
         if constexpr (Reflected<F> && !std::is_enum_v<F> && !std::is_arithmetic_v<F>
                       && !std::is_same_v<F, bool>)
            return &cp_schema<F>::schema;
         else
            return nullptr;
      }

      // Helper: get the element schema pointer for vector types
      template <typename F>
      consteval const dynamic_schema* element_schema_ptr()
      {
         if constexpr (is_vector<F>::value)
         {
            using E = typename is_vector<F>::element_type;
            if constexpr (Reflected<E> && !std::is_enum_v<E>)
               return &cp_schema<E>::schema;
            else
               return nullptr;
         }
         else
            return nullptr;
      }

      // Build variant alternative descriptors
      template <typename T, size_t FieldIdx, typename Variant, size_t... Js>
      consteval auto build_alt_descs(std::index_sequence<Js...>)
      {
         using layout = capnp_layout<T>;
         std::array<dynamic_alt_desc, sizeof...(Js)> alts{};
         (
             [&]
             {
                using A         = std::variant_alternative_t<Js, Variant>;
                auto aloc       = layout::alt_loc(FieldIdx, Js);
                alts[Js].type   = type_tag_for<A>();
                alts[Js].is_ptr = aloc.is_ptr;
                alts[Js].offset = aloc.offset;
                alts[Js].bit_index = aloc.bit_index;
                alts[Js].byte_size = scalar_byte_size<A>();
                alts[Js].nested    = nested_schema_ptr<A>();
             }(),
             ...);
         return alts;
      }
   }  // namespace capnp_detail

   // Constexpr schema for a reflected type in capnp format
   template <typename T>
      requires Reflected<T>
   struct cp_schema
   {
      static constexpr size_t N = capnp_layout<T>::num_members;

      // Original-order field names
      static constexpr auto names = reflect<T>::data_member_names;

      // Build field descriptor array sorted by name_hash
      static constexpr auto build_fields()
      {
         std::array<dynamic_field_desc, N> arr{};
         apply_members(
             (typename reflect<T>::data_members*)nullptr,
             [&]<typename... Ms>(Ms... members)
             {
                auto tup = std::make_tuple(members...);
                [&]<size_t... Is>(std::index_sequence<Is...>)
                {
                   (
                       [&]
                       {
                          using F = std::remove_cvref_t<
                              decltype(std::declval<T>().*std::get<Is>(tup))>;
                          using layout = capnp_layout<T>;
                          constexpr auto loc = layout::loc(Is);

                          const char* fname = reflect<T>::data_member_names[Is];
                          size_t      flen  = 0;
                          for (const char* p = fname; *p; ++p)
                             ++flen;

                          arr[Is].name_hash = xxh64::hash(fname, flen);
                          arr[Is].name      = fname;
                          arr[Is].type      = capnp_detail::type_tag_for<F>();
                          arr[Is].is_ptr    = loc.is_ptr;
                          arr[Is].offset    = loc.offset;
                          arr[Is].bit_index = loc.bit_index;
                          arr[Is].byte_size = capnp_detail::scalar_byte_size<F>();

                          // Nested schema for struct fields
                          if constexpr (Reflected<F> && !std::is_enum_v<F>
                                        && !std::is_arithmetic_v<F>
                                        && !std::is_same_v<F, bool>)
                             arr[Is].nested = &cp_schema<F>::schema;

                          // Element schema for vector-of-struct fields
                          if constexpr (capnp_detail::is_vector<F>::value)
                          {
                             using E = typename capnp_detail::is_vector<F>::element_type;
                             if constexpr (Reflected<E> && !std::is_enum_v<E>)
                                arr[Is].nested = &cp_schema<E>::schema;
                          }

                          // Variant support
                          if constexpr (capnp_detail::is_variant_type<F>::value)
                          {
                             constexpr size_t AC = std::variant_size_v<F>;
                             arr[Is].alt_count   = static_cast<uint8_t>(AC);
                             arr[Is].disc_offset = loc.offset;
                             arr[Is].alternatives = alt_descs<Is>.data();
                          }
                       }(),
                       ...);
                }(std::make_index_sequence<sizeof...(Ms)>{});
             });

         // Sort by name_hash for tag-byte scanning
         for (size_t i = 1; i < N; ++i)
            for (size_t j = i; j > 0 && arr[j].name_hash < arr[j - 1].name_hash; --j)
               std::swap(arr[j], arr[j - 1]);

         return arr;
      }

      // Build tag byte array (low byte of each hash, parallel to sorted_fields)
      static constexpr auto build_tags()
      {
         auto fields = build_fields();
         std::array<uint8_t, N> t{};
         for (size_t i = 0; i < N; ++i)
            t[i] = static_cast<uint8_t>(fields[i].name_hash);
         return t;
      }

      // Variant alt desc arrays — one per variant field
      // We need a static constexpr array for each variant field so that
      // dynamic_field_desc::alternatives can point to it.
      template <size_t FieldIdx>
      static constexpr auto build_alts_for_field()
      {
         using F = std::tuple_element_t<FieldIdx, struct_tuple_t<T>>;
         if constexpr (capnp_detail::is_variant_type<F>::value)
         {
            constexpr size_t AC = std::variant_size_v<F>;
            return capnp_detail::build_alt_descs<T, FieldIdx, F>(
                std::make_index_sequence<AC>{});
         }
         else
         {
            return std::array<dynamic_alt_desc, 0>{};
         }
      }

      // We need one constexpr array per field index; use a variable template.
      template <size_t FieldIdx>
      static constexpr auto alt_descs = build_alts_for_field<FieldIdx>();

      static constexpr auto sorted = build_fields();
      static constexpr auto tag_bytes = build_tags();
      static constexpr dynamic_schema schema{
          sorted.data(), N,
          names,
          capnp_layout<T>::result.data_words,
          capnp_layout<T>::result.ptr_count,
          tag_bytes.data()};
   };

   // ── dynamic_view — format-generic chainable cursor ─────────────────────

   // Forward declarations
   template <typename T, typename Buffer = std::vector<uint8_t>>
      requires Reflected<T>
   class capnp_ref;

   template <typename Format>
   class dynamic_vector;

   template <typename Format>
   class dynamic_view
   {
      dynamic_type          type_   = dynamic_type::t_void;
      const dynamic_schema* schema_ = nullptr;

      // Struct cursor state
      const uint8_t* data_       = nullptr;
      uint16_t       data_words_ = 0;
      uint16_t       ptr_count_  = 0;

      // Leaf value (only valid when type_ is a leaf type)
      union
      {
         bool     bval_;
         int64_t  ival_;
         uint64_t uval_;
         double   fval_;
      };
      std::string_view sval_{};

      // Vector state (valid when type_ == t_vector)
      uint32_t              vec_count_       = 0;
      uint32_t              vec_elem_stride_ = 0;
      uint16_t              vec_elem_dw_     = 0;
      uint16_t              vec_elem_pc_     = 0;
      dynamic_type          vec_elem_type_   = dynamic_type::t_void;
      const dynamic_schema* vec_elem_schema_ = nullptr;

      // Variant state
      uint8_t variant_index_ = 0;
      bool    is_variant_    = false;

      // ── Private helpers ──────────────────────────────────────────────

      const uint8_t* ptr_slot(uint32_t ptr_index) const
      {
         return data_ + static_cast<uint32_t>(data_words_) * 8 + ptr_index * 8;
      }

      // Read a scalar from the data section
      void read_scalar(const dynamic_field_desc& field)
      {
         type_ = field.type;
         if (!data_ || field.offset + field.byte_size > data_words_ * 8u)
         {
            uval_ = 0;
            return;
         }
         switch (field.type)
         {
            case dynamic_type::t_bool:
            {
               uint8_t byte = data_[field.offset];
               bval_ = (byte >> field.bit_index) & 1;
               break;
            }
            case dynamic_type::t_i8:
            {
               int8_t v = 0;
               std::memcpy(&v, data_ + field.offset, 1);
               ival_ = v;
               break;
            }
            case dynamic_type::t_i16:
            {
               int16_t v = 0;
               std::memcpy(&v, data_ + field.offset, 2);
               ival_ = v;
               break;
            }
            case dynamic_type::t_i32:
            {
               int32_t v = 0;
               std::memcpy(&v, data_ + field.offset, 4);
               ival_ = v;
               break;
            }
            case dynamic_type::t_i64:
            {
               int64_t v = 0;
               std::memcpy(&v, data_ + field.offset, 8);
               ival_ = v;
               break;
            }
            case dynamic_type::t_u8:
            {
               uint8_t v = 0;
               std::memcpy(&v, data_ + field.offset, 1);
               uval_ = v;
               break;
            }
            case dynamic_type::t_u16:
            {
               uint16_t v = 0;
               std::memcpy(&v, data_ + field.offset, 2);
               uval_ = v;
               break;
            }
            case dynamic_type::t_u32:
            {
               uint32_t v = 0;
               std::memcpy(&v, data_ + field.offset, 4);
               uval_ = v;
               break;
            }
            case dynamic_type::t_u64:
            {
               uint64_t v = 0;
               std::memcpy(&v, data_ + field.offset, 8);
               uval_ = v;
               break;
            }
            case dynamic_type::t_f32:
            {
               float v = 0;
               std::memcpy(&v, data_ + field.offset, 4);
               fval_ = v;
               break;
            }
            case dynamic_type::t_f64:
            {
               double v = 0;
               std::memcpy(&v, data_ + field.offset, 8);
               fval_ = v;
               break;
            }
            default:
               uval_ = 0;
               break;
         }
      }

      // Read a scalar from a variant alternative descriptor
      void read_scalar_alt(const dynamic_alt_desc& alt)
      {
         type_ = alt.type;
         if (!data_)
         {
            uval_ = 0;
            return;
         }
         // Reuse the same switch logic via a temporary field_desc
         dynamic_field_desc tmp{};
         tmp.type      = alt.type;
         tmp.offset    = alt.offset;
         tmp.bit_index = alt.bit_index;
         tmp.byte_size = alt.byte_size;
         read_scalar(tmp);
      }

      // Read a pointer-section field
      void read_pointer(const dynamic_field_desc& field)
      {
         const uint8_t* slot = ptr_slot(field.offset);

         switch (field.type)
         {
            case dynamic_type::t_text:
               type_ = dynamic_type::t_text;
               sval_ = capnp_detail::read_text(slot);
               break;

            case dynamic_type::t_struct:
            {
               auto p      = capnp_detail::resolve_struct_ptr(slot);
               type_       = dynamic_type::t_struct;
               data_       = p.data;
               data_words_ = p.data_words;
               ptr_count_  = p.ptr_count;
               schema_     = field.nested;
               break;
            }

            case dynamic_type::t_data:
            case dynamic_type::t_vector:
            {
               auto info         = capnp_detail::resolve_list_ptr(slot);
               type_             = field.type;
               data_             = info.data;
               vec_count_        = info.count;
               vec_elem_stride_  = info.elem_stride;
               vec_elem_dw_      = info.elem_data_words;
               vec_elem_pc_      = info.elem_ptr_count;
               vec_elem_schema_  = field.nested;  // struct element schema (may be null)
               // Determine element type from the element schema/stride
               if (info.elem_data_words > 0 || info.elem_ptr_count > 0)
                  vec_elem_type_ = dynamic_type::t_struct;
               else if (info.elem_stride == 8)
                  vec_elem_type_ = dynamic_type::t_text;  // pointer list → text
               else
                  vec_elem_type_ = dynamic_type::t_void;  // scalar list
               break;
            }

            default:
               type_ = dynamic_type::t_void;
               break;
         }
      }

      // Read a pointer-section field from a variant alt
      void read_pointer_alt(const dynamic_alt_desc& alt)
      {
         const uint8_t* slot = ptr_slot(alt.offset);
         switch (alt.type)
         {
            case dynamic_type::t_text:
               type_ = dynamic_type::t_text;
               sval_ = capnp_detail::read_text(slot);
               break;
            case dynamic_type::t_struct:
            {
               auto p      = capnp_detail::resolve_struct_ptr(slot);
               type_       = dynamic_type::t_struct;
               data_       = p.data;
               data_words_ = p.data_words;
               ptr_count_  = p.ptr_count;
               schema_     = alt.nested;
               break;
            }
            case dynamic_type::t_vector:
            {
               auto info         = capnp_detail::resolve_list_ptr(slot);
               type_             = dynamic_type::t_vector;
               data_             = info.data;
               vec_count_        = info.count;
               vec_elem_stride_  = info.elem_stride;
               vec_elem_dw_      = info.elem_data_words;
               vec_elem_pc_      = info.elem_ptr_count;
               vec_elem_schema_  = alt.nested;
               if (info.elem_data_words > 0 || info.elem_ptr_count > 0)
                  vec_elem_type_ = dynamic_type::t_struct;
               else
                  vec_elem_type_ = dynamic_type::t_void;
               break;
            }
            default:
               type_ = dynamic_type::t_void;
               break;
         }
      }

      // Resolve a variant field — read discriminant, pick alternative
      void read_variant(const dynamic_field_desc& field)
      {
         is_variant_ = true;
         uint16_t disc = 0;
         if (data_ && field.disc_offset + 1 < data_words_ * 8u)
            std::memcpy(&disc, data_ + field.disc_offset, 2);

         variant_index_ = static_cast<uint8_t>(disc);
         if (disc >= field.alt_count)
         {
            type_ = dynamic_type::t_void;
            return;
         }

         const auto& alt = field.alternatives[disc];
         if (alt.type == dynamic_type::t_void)  // monostate
         {
            type_ = dynamic_type::t_void;
            return;
         }

         if (alt.is_ptr)
            read_pointer_alt(alt);
         else
            read_scalar_alt(alt);
      }

     public:
      dynamic_view() = default;

      // Construct from a capnp_ptr + schema
      dynamic_view(capnp_ptr p, const dynamic_schema* s)
          : type_(dynamic_type::t_struct)
          , schema_(s)
          , data_(p.data)
          , data_words_(p.data_words)
          , ptr_count_(p.ptr_count)
      {
         uval_ = 0;
      }

      // Construct from view<T, cp>
      template <typename T>
         requires Reflected<T>
      dynamic_view(view<T, cp> v)
          : dynamic_view(v.data(), &cp_schema<T>::schema)
      {
      }

      // Construct from capnp_ref<T> — defined after capnp_ref
      template <typename T, typename Buffer>
         requires Reflected<T>
      dynamic_view(const capnp_ref<T, Buffer>& ref);

      // ── Navigation ──────────────────────────────────────────────────

      // Direct field access from a pre-resolved descriptor.
      // Used by compiled_path::eval() to skip hash lookup entirely.
      dynamic_view resolve_field(const dynamic_field_desc& field) const
      {
         dynamic_view result;
         result.data_       = data_;
         result.data_words_ = data_words_;
         result.ptr_count_  = ptr_count_;

         if (field.type == dynamic_type::t_variant)
            result.read_variant(field);
         else if (field.is_ptr)
            result.read_pointer(field);
         else
            result.read_scalar(field);

         return result;
      }

      // Access a named field — throws if field not found
      dynamic_view operator[](field_name fn) const
      {
         if (type_ != dynamic_type::t_struct || !schema_)
            throw std::runtime_error(
                "dynamic_view: cannot access field on non-struct");

         auto* field = schema_->find(fn);
         if (!field)
            throw std::runtime_error(
                std::string("dynamic_view: field not found: ") +
                std::string(fn.name));

         return resolve_field(*field);
      }

      // Access a vector element by index
      dynamic_view operator[](size_t idx) const
      {
         if (type_ != dynamic_type::t_vector)
            throw std::runtime_error(
                "dynamic_view: cannot index non-vector");
         if (idx >= vec_count_)
            throw std::out_of_range("dynamic_view: index out of range");

         dynamic_view result;
         result.uval_ = 0;

         if (vec_elem_dw_ > 0 || vec_elem_pc_ > 0)
         {
            // Composite list — struct elements
            uint32_t stride = (vec_elem_dw_ + vec_elem_pc_) * 8u;
            result.type_       = dynamic_type::t_struct;
            result.data_       = data_ + idx * stride;
            result.data_words_ = vec_elem_dw_;
            result.ptr_count_  = vec_elem_pc_;
            result.schema_     = vec_elem_schema_;
         }
         else if (vec_elem_stride_ == 8 && vec_elem_type_ == dynamic_type::t_text)
         {
            // Pointer list — text elements
            const uint8_t* slot = data_ + idx * 8;
            result.type_ = dynamic_type::t_text;
            result.sval_ = capnp_detail::read_text(slot);
         }
         else
         {
            // Scalar list — read raw bytes
            const uint8_t* elem = data_ + idx * vec_elem_stride_;
            switch (vec_elem_stride_)
            {
               case 1:
               {
                  uint8_t v;
                  std::memcpy(&v, elem, 1);
                  result.type_ = dynamic_type::t_u8;
                  result.uval_ = v;
                  break;
               }
               case 2:
               {
                  uint16_t v;
                  std::memcpy(&v, elem, 2);
                  result.type_ = dynamic_type::t_u16;
                  result.uval_ = v;
                  break;
               }
               case 4:
               {
                  uint32_t v;
                  std::memcpy(&v, elem, 4);
                  result.type_ = dynamic_type::t_u32;
                  result.uval_ = v;
                  break;
               }
               case 8:
               {
                  uint64_t v;
                  std::memcpy(&v, elem, 8);
                  result.type_ = dynamic_type::t_u64;
                  result.uval_ = v;
                  break;
               }
               default:
                  result.type_ = dynamic_type::t_void;
                  break;
            }
         }

         return result;
      }

      // ── Existence check ─────────────────────────────────────────────

      bool exists() const { return type_ != dynamic_type::t_void; }

      // ── Type introspection ──────────────────────────────────────────

      dynamic_type_info type() const
      {
         dynamic_type_info info;
         if (is_variant_)
         {
            info.kind          = dynamic_type::t_variant;
            info.active_kind   = type_;
            info.variant_index = variant_index_;
         }
         else
         {
            info.kind = type_;
         }
         return info;
      }

      // Variant discriminant index
      size_t index() const { return variant_index_; }

      // ── Size (for strings and vectors) ──────────────────────────────

      size_t size() const
      {
         if (type_ == dynamic_type::t_text)
            return sval_.size();
         if (type_ == dynamic_type::t_vector || type_ == dynamic_type::t_data)
            return vec_count_;
         return 0;
      }

      // ── Schema introspection ────────────────────────────────────────

      // Field names in declaration order
      std::span<const char* const> field_names() const
      {
         if (schema_)
            return {schema_->ordered_names, schema_->field_count};
         return {};
      }

      // Field index by name (declaration order), throws if not found
      size_t field_index(std::string_view name) const
      {
         if (!schema_)
            throw std::runtime_error("dynamic_view: no schema");
         for (size_t i = 0; i < schema_->field_count; ++i)
            if (name == schema_->ordered_names[i])
               return i;
         throw std::runtime_error(
             std::string("dynamic_view: field not found: ") +
             std::string(name));
      }

      // ── Implicit conversion operators ───────────────────────────────

      explicit operator bool() const
      {
         if (type_ != dynamic_type::t_bool)
            throw std::runtime_error("dynamic_view: not a bool");
         return bval_;
      }

      operator int8_t() const
      {
         if (type_ != dynamic_type::t_i8)
            throw std::runtime_error("dynamic_view: not an int8");
         return static_cast<int8_t>(ival_);
      }

      operator int16_t() const
      {
         if (type_ != dynamic_type::t_i16)
            throw std::runtime_error("dynamic_view: not an int16");
         return static_cast<int16_t>(ival_);
      }

      operator int32_t() const
      {
         if (type_ != dynamic_type::t_i32)
            throw std::runtime_error("dynamic_view: not an int32");
         return static_cast<int32_t>(ival_);
      }

      operator int64_t() const
      {
         if (type_ != dynamic_type::t_i64)
            throw std::runtime_error("dynamic_view: not an int64");
         return ival_;
      }

      operator uint8_t() const
      {
         if (type_ != dynamic_type::t_u8)
            throw std::runtime_error("dynamic_view: not a uint8");
         return static_cast<uint8_t>(uval_);
      }

      operator uint16_t() const
      {
         if (type_ != dynamic_type::t_u16)
            throw std::runtime_error("dynamic_view: not a uint16");
         return static_cast<uint16_t>(uval_);
      }

      operator uint32_t() const
      {
         if (type_ != dynamic_type::t_u32)
            throw std::runtime_error("dynamic_view: not a uint32");
         return static_cast<uint32_t>(uval_);
      }

      operator uint64_t() const
      {
         if (type_ != dynamic_type::t_u64)
            throw std::runtime_error("dynamic_view: not a uint64");
         return uval_;
      }

      operator float() const
      {
         if (type_ != dynamic_type::t_f32)
            throw std::runtime_error("dynamic_view: not a float");
         return static_cast<float>(fval_);
      }

      operator double() const
      {
         if (type_ != dynamic_type::t_f64)
            throw std::runtime_error("dynamic_view: not a double");
         return fval_;
      }

      operator std::string_view() const
      {
         if (type_ != dynamic_type::t_text)
            throw std::runtime_error("dynamic_view: not text");
         return sval_;
      }

      // Duck-typed extraction to a reflected struct
      template <typename U>
         requires Reflected<U>
      U as() const
      {
         if (type_ != dynamic_type::t_struct || !schema_)
            throw std::runtime_error("dynamic_view: not a struct");

         U result{};
         apply_members(
             (typename reflect<U>::data_members*)nullptr,
             [&]<typename... Ms>(Ms... members)
             {
                auto tup = std::make_tuple(members...);
                [&]<size_t... Is>(std::index_sequence<Is...>)
                {
                   (
                       [&]
                       {
                          auto fname = reflect<U>::data_member_names[Is];
                          auto* field = schema_->find(fname);
                          if (field)
                          {
                             using F = std::remove_cvref_t<
                                 decltype(std::declval<U>().*std::get<Is>(tup))>;
                             auto child = (*this)[field_name{fname}];
                             if constexpr (std::is_same_v<F, std::string>)
                                result.*std::get<Is>(tup) = std::string(
                                    static_cast<std::string_view>(child));
                             else if constexpr (std::is_same_v<F, bool>)
                                result.*std::get<Is>(tup) = static_cast<bool>(child);
                             else if constexpr (std::is_arithmetic_v<F>)
                                result.*std::get<Is>(tup) = static_cast<F>(child);
                             else if constexpr (Reflected<F> && !std::is_enum_v<F>)
                                result.*std::get<Is>(tup) = child.template as<F>();
                          }
                       }(),
                       ...);
                }(std::make_index_sequence<sizeof...(Ms)>{});
             });
         return result;
      }

      // ── Comparison ──────────────────────────────────────────────────

      int compare(const dynamic_view& rhs) const
      {
         if (type_ != rhs.type_)
            return static_cast<int>(type_) - static_cast<int>(rhs.type_);
         switch (type_)
         {
            case dynamic_type::t_bool:
               return int(bval_) - int(rhs.bval_);
            case dynamic_type::t_i8:
            case dynamic_type::t_i16:
            case dynamic_type::t_i32:
            case dynamic_type::t_i64:
               return (ival_ < rhs.ival_) ? -1 : (ival_ > rhs.ival_) ? 1 : 0;
            case dynamic_type::t_u8:
            case dynamic_type::t_u16:
            case dynamic_type::t_u32:
            case dynamic_type::t_u64:
               return (uval_ < rhs.uval_) ? -1 : (uval_ > rhs.uval_) ? 1 : 0;
            case dynamic_type::t_f32:
            case dynamic_type::t_f64:
               return (fval_ < rhs.fval_) ? -1 : (fval_ > rhs.fval_) ? 1 : 0;
            case dynamic_type::t_text:
               return sval_.compare(rhs.sval_);
            default:
               return 0;
         }
      }

      bool operator<(const dynamic_view& rhs) const { return compare(rhs) < 0; }
      bool operator>(const dynamic_view& rhs) const { return compare(rhs) > 0; }
      bool operator<=(const dynamic_view& rhs) const { return compare(rhs) <= 0; }
      bool operator>=(const dynamic_view& rhs) const { return compare(rhs) >= 0; }
      bool operator==(const dynamic_view& rhs) const { return compare(rhs) == 0; }
      bool operator!=(const dynamic_view& rhs) const { return compare(rhs) != 0; }

      // ── Path navigation ─────────────────────────────────────────────

      // operator/ chains — programmatic path building
      dynamic_view operator/(const char* name) const { return (*this)[field_name{name}]; }
      dynamic_view operator/(std::string_view name) const { return (*this)[field_name{name}]; }
      dynamic_view operator/(field_name fn) const { return (*this)[fn]; }
      dynamic_view operator/(size_t idx) const { return (*this)[idx]; }

      // .path("a.b.c[1].d") — runtime string path traversal
      // Grammar: segment ('.' segment | '[' index ']')*
      //   segment = identifier
      //   index   = digits
      dynamic_view path(std::string_view p) const
      {
         dynamic_view cur = *this;
         size_t       i   = 0;
         while (i < p.size())
         {
            if (p[i] == '.')
               ++i;  // skip dot separator

            if (i >= p.size())
               break;

            if (p[i] == '[')
            {
               // Parse array index: [digits]
               ++i;  // skip '['
               size_t idx = 0;
               while (i < p.size() && p[i] >= '0' && p[i] <= '9')
               {
                  idx = idx * 10 + (p[i] - '0');
                  ++i;
               }
               if (i >= p.size() || p[i] != ']')
                  throw std::runtime_error(
                      "dynamic_view::path: expected ']' in: " + std::string(p));
               ++i;  // skip ']'
               cur = cur[idx];
            }
            else
            {
               // Parse field name: alphanumeric + underscore
               size_t start = i;
               while (i < p.size() && p[i] != '.' && p[i] != '[')
                  ++i;
               cur = cur[field_name{p.substr(start, i - start)}];
            }
         }
         return cur;
      }

      // Grant dynamic_vector access to private state
      friend class dynamic_vector<Format>;
   };

   // ── dynamic_vector — format-generic list cursor ────────────────────────

   template <typename Format>
   class dynamic_vector
   {
      const uint8_t*        data_        = nullptr;
      uint32_t              count_       = 0;
      uint32_t              elem_stride_ = 0;
      uint16_t              elem_dw_     = 0;
      uint16_t              elem_pc_     = 0;
      dynamic_type          elem_type_   = dynamic_type::t_void;
      const dynamic_schema* elem_schema_ = nullptr;

     public:
      dynamic_vector() = default;

      // Construct from a dynamic_view that holds a vector
      dynamic_vector(const dynamic_view<Format>& dv)
      {
         if (dv.type_ != dynamic_type::t_vector && dv.type_ != dynamic_type::t_data)
            throw std::runtime_error("dynamic_vector: source is not a vector");
         data_        = dv.data_;
         count_       = dv.vec_count_;
         elem_stride_ = dv.vec_elem_stride_;
         elem_dw_     = dv.vec_elem_dw_;
         elem_pc_     = dv.vec_elem_pc_;
         elem_type_   = dv.vec_elem_type_;
         elem_schema_ = dv.vec_elem_schema_;
      }

      size_t size() const { return count_; }
      bool   empty() const { return count_ == 0; }

      dynamic_view<Format> operator[](size_t idx) const
      {
         if (idx >= count_)
            throw std::out_of_range("dynamic_vector: index out of range");

         dynamic_view<Format> result;
         result.uval_ = 0;

         if (elem_dw_ > 0 || elem_pc_ > 0)
         {
            uint32_t stride    = (elem_dw_ + elem_pc_) * 8u;
            result.type_       = dynamic_type::t_struct;
            result.data_       = data_ + idx * stride;
            result.data_words_ = elem_dw_;
            result.ptr_count_  = elem_pc_;
            result.schema_     = elem_schema_;
         }
         else if (elem_stride_ == 8 && elem_type_ == dynamic_type::t_text)
         {
            const uint8_t* slot = data_ + idx * 8;
            result.type_ = dynamic_type::t_text;
            result.sval_ = capnp_detail::read_text(slot);
         }
         else
         {
            const uint8_t* elem = data_ + idx * elem_stride_;
            switch (elem_stride_)
            {
               case 1:
               {
                  uint8_t v;
                  std::memcpy(&v, elem, 1);
                  result.type_ = dynamic_type::t_u8;
                  result.uval_ = v;
                  break;
               }
               case 2:
               {
                  uint16_t v;
                  std::memcpy(&v, elem, 2);
                  result.type_ = dynamic_type::t_u16;
                  result.uval_ = v;
                  break;
               }
               case 4:
               {
                  uint32_t v;
                  std::memcpy(&v, elem, 4);
                  result.type_ = dynamic_type::t_u32;
                  result.uval_ = v;
                  break;
               }
               case 8:
               {
                  uint64_t v;
                  std::memcpy(&v, elem, 8);
                  result.type_ = dynamic_type::t_u64;
                  result.uval_ = v;
                  break;
               }
               default:
                  result.type_ = dynamic_type::t_void;
                  break;
            }
         }

         return result;
      }

      // Iterator for range-based for loops
      class iterator
      {
         const dynamic_vector* vec_;
         size_t                idx_;

        public:
         iterator(const dynamic_vector* v, size_t i) : vec_(v), idx_(i) {}

         dynamic_view<Format> operator*() const { return (*vec_)[idx_]; }
         iterator&            operator++()
         {
            ++idx_;
            return *this;
         }
         bool operator!=(const iterator& rhs) const { return idx_ != rhs.idx_; }
         bool operator==(const iterator& rhs) const { return idx_ == rhs.idx_; }
      };

      iterator begin() const { return {this, 0}; }
      iterator end() const { return {this, count_}; }
   };

   // Forward declaration for hashed_path::compile()
   template <typename Format>
   class compiled_path;

   // ── hashed_path — pre-parsed, pre-hashed path (schema-independent) ──────
   //
   // Parses a dotted path once and pre-computes xxh64 hashes for each field
   // name segment.  Schema-independent: can be evaluated against any schema
   // that has matching field names.  eval() uses pre-computed hashes for
   // tag-byte lookup — no string parsing or hashing per row.
   //
   //   auto hp = hashed_path("customer.score");
   //   for (auto& row : rows)
   //      double score = hp.eval<cp>(row_view);  // tag scan, no parse/hash

   class hashed_path
   {
     public:
      struct step
      {
         uint64_t    hash    = 0;       // pre-computed xxh64 (for field steps)
         const char* name    = nullptr;  // owned copy for collision verify
         size_t      index   = 0;       // array index (for index steps)
         bool        is_index = false;
      };

     private:
      static constexpr size_t inline_cap = 8;
      step    inline_[inline_cap];
      step*   steps_    = inline_;
      size_t  count_    = 0;
      size_t  capacity_ = inline_cap;

      // Owned string storage for field names
      char*  name_buf_  = nullptr;
      size_t name_used_ = 0;
      size_t name_cap_  = 0;

      char* alloc_name(std::string_view s)
      {
         size_t needed = s.size() + 1;
         if (name_used_ + needed > name_cap_)
         {
            size_t new_cap = (name_cap_ == 0) ? 64 : name_cap_ * 2;
            while (new_cap < name_used_ + needed)
               new_cap *= 2;
            auto* buf = new char[new_cap];
            if (name_buf_)
            {
               for (size_t i = 0; i < name_used_; ++i)
                  buf[i] = name_buf_[i];
               // Fix up existing step name pointers
               for (size_t i = 0; i < count_; ++i)
                  if (!steps_[i].is_index)
                     steps_[i].name = buf + (steps_[i].name - name_buf_);
               delete[] name_buf_;
            }
            name_buf_ = buf;
            name_cap_ = new_cap;
         }
         char* dst = name_buf_ + name_used_;
         for (size_t i = 0; i < s.size(); ++i)
            dst[i] = s[i];
         dst[s.size()] = '\0';
         name_used_ += needed;
         return dst;
      }

      void push(step s)
      {
         if (count_ == capacity_)
         {
            size_t new_cap = capacity_ * 2;
            auto*  buf     = new step[new_cap];
            for (size_t i = 0; i < count_; ++i)
               buf[i] = steps_[i];
            if (steps_ != inline_)
               delete[] steps_;
            steps_    = buf;
            capacity_ = new_cap;
         }
         steps_[count_++] = s;
      }

      void free_resources()
      {
         if (steps_ != inline_)
            delete[] steps_;
         delete[] name_buf_;
      }

     public:
      hashed_path() = default;

      // Parse and pre-hash a dotted path
      explicit hashed_path(std::string_view path)
      {
         size_t i = 0;
         while (i < path.size())
         {
            if (path[i] == '.')
               ++i;

            if (i >= path.size())
               break;

            if (path[i] == '[')
            {
               ++i;
               size_t idx = 0;
               while (i < path.size() && path[i] >= '0' && path[i] <= '9')
               {
                  idx = idx * 10 + (path[i] - '0');
                  ++i;
               }
               if (i >= path.size() || path[i] != ']')
                  throw std::runtime_error(
                      "hashed_path: expected ']' in: " + std::string(path));
               ++i;
               push({0, nullptr, idx, true});
            }
            else
            {
               size_t start = i;
               while (i < path.size() && path[i] != '.' && path[i] != '[')
                  ++i;
               auto seg  = path.substr(start, i - start);
               auto hash = xxh64_hash(seg);
               auto* owned_name = alloc_name(seg);
               push({hash, owned_name, 0, false});
            }
         }
      }

      ~hashed_path() { free_resources(); }

      // Move-only
      hashed_path(hashed_path&& o) noexcept
          : count_(o.count_), capacity_(o.capacity_),
            name_buf_(o.name_buf_), name_used_(o.name_used_), name_cap_(o.name_cap_)
      {
         if (o.steps_ == o.inline_)
         {
            for (size_t i = 0; i < count_; ++i)
               inline_[i] = o.inline_[i];
            steps_ = inline_;
         }
         else
         {
            steps_   = o.steps_;
            o.steps_ = o.inline_;
         }
         o.count_    = 0;
         o.name_buf_ = nullptr;
         o.name_used_ = 0;
         o.name_cap_  = 0;
      }

      hashed_path& operator=(hashed_path&& o) noexcept
      {
         if (this != &o)
         {
            free_resources();
            count_     = o.count_;
            capacity_  = o.capacity_;
            name_buf_  = o.name_buf_;
            name_used_ = o.name_used_;
            name_cap_  = o.name_cap_;
            if (o.steps_ == o.inline_)
            {
               for (size_t i = 0; i < count_; ++i)
                  inline_[i] = o.inline_[i];
               steps_ = inline_;
            }
            else
            {
               steps_   = o.steps_;
               o.steps_ = o.inline_;
            }
            o.count_     = 0;
            o.name_buf_  = nullptr;
            o.name_used_ = 0;
            o.name_cap_  = 0;
         }
         return *this;
      }

      hashed_path(const hashed_path&)            = delete;
      hashed_path& operator=(const hashed_path&) = delete;

      size_t      depth() const { return count_; }
      const step* steps() const { return steps_; }

      // Evaluate against a dynamic_view using pre-computed hashes.
      // Each field step does a tag-byte scan (no parsing, no hashing).
      template <typename Format>
      dynamic_view<Format> eval(dynamic_view<Format> root) const
      {
         dynamic_view<Format> cur = root;
         for (size_t i = 0; i < count_; ++i)
         {
            if (steps_[i].is_index)
            {
               cur = cur[steps_[i].index];
            }
            else
            {
               // Use pre-computed hash for fast schema lookup
               cur = cur[field_name{std::string_view(steps_[i].name),
                                    steps_[i].hash}];
            }
         }
         return cur;
      }

      // Compile against a specific schema for even faster eval.
      // Returns a compiled_path with pre-resolved field descriptors.
      template <typename Format>
      compiled_path<Format> compile(const dynamic_schema& schema) const;
   };

   // ── compiled_path — parse-once, eval-many field accessor ─────────────────
   //
   // Pre-resolves a dotted path ("customer.score", "items[0].product") against
   // a schema at query-compile time.  eval() then walks pre-resolved field
   // descriptors with no hashing or string ops — just pointer arithmetic.
   //
   //   auto accessor = compiled_path<cp>(schema, "customer.score");
   //   for (auto& row : rows)
   //      double score = accessor.eval(row_ptr);  // no string ops per row

   template <typename Format>
   class compiled_path
   {
     public:
      struct step
      {
         const dynamic_field_desc* field = nullptr;  // pre-resolved (for field steps)
         size_t                    index = 0;         // array index (for index steps)
         bool                      is_index = false;
      };

     private:
      // Inline storage for short paths (most queries access 1-4 levels deep)
      static constexpr size_t inline_cap = 8;
      step    inline_[inline_cap];
      step*   steps_    = inline_;
      size_t  count_    = 0;
      size_t  capacity_ = inline_cap;

      // The root schema this path was compiled against
      const dynamic_schema* root_schema_ = nullptr;

      void push(step s)
      {
         if (count_ == capacity_)
         {
            size_t new_cap = capacity_ * 2;
            auto*  buf     = new step[new_cap];
            for (size_t i = 0; i < count_; ++i)
               buf[i] = steps_[i];
            if (steps_ != inline_)
               delete[] steps_;
            steps_    = buf;
            capacity_ = new_cap;
         }
         steps_[count_++] = s;
      }

     public:
      compiled_path() = default;

      // Compile a dotted path against a schema
      compiled_path(const dynamic_schema& root, std::string_view path)
          : root_schema_(&root)
      {
         const dynamic_schema* cur_schema = &root;
         size_t                i          = 0;

         while (i < path.size())
         {
            if (path[i] == '.')
               ++i;

            if (i >= path.size())
               break;

            if (path[i] == '[')
            {
               ++i;
               size_t idx = 0;
               while (i < path.size() && path[i] >= '0' && path[i] <= '9')
               {
                  idx = idx * 10 + (path[i] - '0');
                  ++i;
               }
               if (i >= path.size() || path[i] != ']')
                  throw std::runtime_error(
                      "compiled_path: expected ']' in: " + std::string(path));
               ++i;
               push({nullptr, idx, true});
               // After indexing into a vector, the element schema (if struct) is
               // already stored in the preceding field step's field->nested.
            }
            else
            {
               size_t start = i;
               while (i < path.size() && path[i] != '.' && path[i] != '[')
                  ++i;
               auto seg = path.substr(start, i - start);

               if (!cur_schema)
                  throw std::runtime_error(
                      "compiled_path: cannot navigate into non-struct at: " +
                      std::string(seg));

               auto* field = cur_schema->find(seg);
               if (!field)
                  throw std::runtime_error(
                      "compiled_path: field not found: " + std::string(seg));

               push({field, 0, false});

               // Advance schema for next segment
               cur_schema = field->nested;
            }
         }
      }

      ~compiled_path()
      {
         if (steps_ != inline_)
            delete[] steps_;
      }

      // Move-only
      compiled_path(compiled_path&& o) noexcept
          : count_(o.count_), capacity_(o.capacity_), root_schema_(o.root_schema_)
      {
         if (o.steps_ == o.inline_)
         {
            for (size_t i = 0; i < count_; ++i)
               inline_[i] = o.inline_[i];
            steps_ = inline_;
         }
         else
         {
            steps_   = o.steps_;
            o.steps_ = o.inline_;
         }
         o.count_ = 0;
      }

      compiled_path& operator=(compiled_path&& o) noexcept
      {
         if (this != &o)
         {
            if (steps_ != inline_)
               delete[] steps_;
            count_       = o.count_;
            capacity_    = o.capacity_;
            root_schema_ = o.root_schema_;
            if (o.steps_ == o.inline_)
            {
               for (size_t i = 0; i < count_; ++i)
                  inline_[i] = o.inline_[i];
               steps_ = inline_;
            }
            else
            {
               steps_   = o.steps_;
               o.steps_ = o.inline_;
            }
            o.count_ = 0;
         }
         return *this;
      }

      compiled_path(const compiled_path&)            = delete;
      compiled_path& operator=(const compiled_path&) = delete;

      size_t      depth() const { return count_; }
      const step* steps() const { return steps_; }

      // Evaluate the pre-resolved path against a root dynamic_view.
      // No hashing, no string comparison — just pointer walks.
      dynamic_view<Format> eval(dynamic_view<Format> root) const
      {
         dynamic_view<Format> cur = root;
         for (size_t i = 0; i < count_; ++i)
         {
            if (steps_[i].is_index)
            {
               cur = cur[steps_[i].index];
            }
            else
            {
               // Direct field access using pre-resolved descriptor
               // This bypasses the hash lookup entirely
               cur = cur.resolve_field(*steps_[i].field);
            }
         }
         return cur;
      }

      friend class hashed_path;
   };

   // Deferred definition: hashed_path::compile()
   template <typename Format>
   compiled_path<Format> hashed_path::compile(const dynamic_schema& schema) const
   {
      compiled_path<Format> result;
      const dynamic_schema* cur_schema = &schema;

      for (size_t i = 0; i < count_; ++i)
      {
         if (steps_[i].is_index)
         {
            result.push({nullptr, steps_[i].index, true});
         }
         else
         {
            if (!cur_schema)
               throw std::runtime_error(
                   "hashed_path::compile: cannot navigate into non-struct at: " +
                   std::string(steps_[i].name));

            auto* field = cur_schema->find(steps_[i].hash,
                                           std::string_view(steps_[i].name));
            if (!field)
               throw std::runtime_error(
                   "hashed_path::compile: field not found: " +
                   std::string(steps_[i].name));

            result.push({field, 0, false});
            cur_schema = field->nested;
         }
      }
      result.root_schema_ = &schema;
      return result;
   }

   // ── capnp_ref: top-level typed handle over a capnp message ────────────────

   template <typename T, typename Buffer>
      requires Reflected<T>
   class capnp_ref
   {
      Buffer                          buf_;
      capnp_detail::capnp_free_list   free_list_;

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
         return proxy_t{proxy_obj_t{&buf_, &free_list_, data_byte, dw, pc}};
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
      bool validate() const { return validate_capnp(buf_.data(), buf_.size()); }

      /// Free-list statistics
      const capnp_detail::capnp_free_list& free_list() const
      {
         return free_list_;
      }

      /// Get the root struct as a capnp_ptr for dynamic field access
      capnp_ptr root_ptr() const
      {
         return capnp_detail::resolve_struct_ptr(
             reinterpret_cast<const uint8_t*>(buf_.data()) + 8);
      }

      /// Raw buffer access
      const uint8_t* data() const
      {
         return reinterpret_cast<const uint8_t*>(buf_.data());
      }
      size_t         size() const { return buf_.size(); }
      Buffer&        buffer() { return buf_; }
      const Buffer&  buffer() const { return buf_; }
   };

   // Deferred definition: dynamic_view constructor from capnp_ref
   template <typename Format>
   template <typename T, typename Buffer>
      requires Reflected<T>
   dynamic_view<Format>::dynamic_view(const capnp_ref<T, Buffer>& ref)
       : dynamic_view(ref.root_ptr(), &cp_schema<T>::schema)
   {
   }

}  // namespace psio1
