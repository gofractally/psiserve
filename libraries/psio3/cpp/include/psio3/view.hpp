#pragma once
//
// psio3/view.hpp — typed zero-copy view over a (T, Fmt) byte span.
//
// `psio3::view<T, Fmt, Store>` is the non-owning (or buffer-borrowing)
// read surface for encoded data. The access-surface rule (design
// § 5.5):
//
//   - Record T: `v.<field>()` per reflected field (lands with the
//     first format that wires up generation, phase 6).
//   - Primitive / string / other shapes: a shape-appropriate
//     terminator method (`.get()` on primitives, `.view_()` on
//     strings).
//   - Storage, format, and materialization operations are free
//     functions in `psio3::` (not methods) — see below.
//
// Phase 3 ships primitive + string views plus the free-function API.
// Record view accessor generation is phase 6's concern (per format).

#include <psio3/buffer.hpp>
#include <psio3/shapes.hpp>
#include <psio3/storage.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace psio3 {

   // Primary template — shape-specific specializations follow.
   template <typename T, typename Fmt, storage Store = storage::const_borrow>
   class view;

   // ── Primitive view ────────────────────────────────────────────────────
   template <Primitive T, typename Fmt, storage Store>
   class view<T, Fmt, Store>
   {
      std::span<const char> data_;

    public:
      using element_type = T;
      using format_type  = Fmt;
      static constexpr storage storage_kind = Store;

      view() = default;
      explicit view(std::span<const char> s) noexcept : data_(s) {}

      // Library-internal accessor; see buffer.hpp for the access-
      // surface rule rationale.
      [[nodiscard]] std::span<const char> _psio3_data() const noexcept
      {
         return data_;
      }

      // .get() — shape terminator. Phase 3 default: LE raw bytes →
      // primitive. Format CPOs (phase 5) will override this by
      // forwarding to `psio3::decode<T>(Fmt{}, bytes(*this))`.
      [[nodiscard]] T get() const noexcept
      {
         T out{};
         if (data_.size() >= sizeof(T))
         {
            for (std::size_t i = 0; i < sizeof(T); ++i)
               reinterpret_cast<unsigned char*>(&out)[i] =
                   static_cast<unsigned char>(data_[i]);
         }
         return out;
      }
   };

   // ── Byte-sequence / string view ───────────────────────────────────────
   template <typename Fmt, storage Store>
   class view<std::string, Fmt, Store>
   {
      std::span<const char> data_;

    public:
      using element_type = std::string;
      using format_type  = Fmt;
      static constexpr storage storage_kind = Store;

      view() = default;
      explicit view(std::span<const char> s) noexcept : data_(s) {}

      [[nodiscard]] std::span<const char> _psio3_data() const noexcept
      {
         return data_;
      }

      // Shape accessor: bytes-as-string_view, valid for the lifetime
      // of the backing storage.
      [[nodiscard]] std::string_view view_() const noexcept
      {
         return std::string_view{data_.data(), data_.size()};
      }
   };

   // ── Free-function storage API for views ───────────────────────────────

   template <typename T, typename Fmt, storage S>
      requires requires(const view<T, Fmt, S>& v) { v._psio3_data(); }
   [[nodiscard]] std::span<const char> bytes(const view<T, Fmt, S>& v) noexcept
   {
      return v._psio3_data();
   }

   template <typename T, typename Fmt, storage S>
      requires requires(const view<T, Fmt, S>& v) { v._psio3_data(); }
   [[nodiscard]] std::size_t size(const view<T, Fmt, S>& v) noexcept
   {
      return v._psio3_data().size();
   }

   template <typename T, typename Fmt, storage S>
   [[nodiscard]] constexpr Fmt format_of(const view<T, Fmt, S>&) noexcept
   {
      return {};
   }

   // ── Construct a view from a buffer ────────────────────────────────────
   template <typename T, typename Fmt, storage S>
   [[nodiscard]] view<T, Fmt, storage::const_borrow>
   as_view(const buffer<T, Fmt, S>& b) noexcept
   {
      return view<T, Fmt, storage::const_borrow>{bytes(b)};
   }

}  // namespace psio3
