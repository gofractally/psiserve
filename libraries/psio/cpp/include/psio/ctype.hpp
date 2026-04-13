#pragma once

// CView<T> and COwned<T> — Canonical ABI type projections from PSIO_REFLECT.
//
// Given a reflected C++ type T, CView<T> replaces owning types with non-owning
// views (string → string_view, vector<U> → span<CView<U>>), and COwned<T>
// replaces them with owning wrappers that free cabi_realloc'd memory.
//
// Usage:
//   struct Transfer {
//       std::string from;
//       std::string to;
//       uint64_t amount;
//       std::vector<std::string> memo_lines;
//   };
//   PSIO_REFLECT(Transfer, from, to, amount, memo_lines)
//
//   // CView<Transfer> ≈ { string_view from, to; uint64_t amount; span<string_view> memo_lines; }
//   // COwned<Transfer> ≈ same layout but destructor frees all allocations

#include <psio/reflect.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

namespace psio {

   // ── Forward declarations ──────────────────────────────────────────────────

   template <typename T>
   struct CView;

   template <typename T>
   struct COwned;

   // ── Type classification traits ────────────────────────────────────────────

   namespace detail {

      template <typename T>
      constexpr bool is_scalar_v = std::is_arithmetic_v<T> || std::is_enum_v<T>;

      template <typename T> struct is_std_string_ct : std::false_type {};
      template <> struct is_std_string_ct<std::string> : std::true_type {};

      template <typename T> struct is_std_vector_ct : std::false_type {};
      template <typename U> struct is_std_vector_ct<std::vector<U>> : std::true_type {};

      template <typename T> struct is_std_optional_ct : std::false_type {};
      template <typename U> struct is_std_optional_ct<std::optional<U>> : std::true_type {};

      template <typename T> struct vector_elem_ct;
      template <typename U> struct vector_elem_ct<std::vector<U>> { using type = U; };

      template <typename T> struct optional_elem_ct;
      template <typename U> struct optional_elem_ct<std::optional<U>> { using type = U; };

      // ── CView field type mapping ──

      template <typename T, typename Enable = void>
      struct cview_field_map;

      // Scalars: pass through
      template <typename T>
      struct cview_field_map<T, std::enable_if_t<is_scalar_v<T>>> {
         using type = T;
      };

      // std::string → std::string_view
      template <>
      struct cview_field_map<std::string, void> {
         using type = std::string_view;
      };

      // std::vector<scalar> → std::span<const scalar>
      template <typename U>
      struct cview_field_map<std::vector<U>, std::enable_if_t<is_scalar_v<U>>> {
         using type = std::span<const U>;
      };

      // std::vector<string> → std::span<const std::string_view>
      template <>
      struct cview_field_map<std::vector<std::string>, void> {
         using type = std::span<const std::string_view>;
      };

      // std::vector<reflected struct> → std::span<const CView<U>>
      template <typename U>
      struct cview_field_map<std::vector<U>, std::enable_if_t<
         !is_scalar_v<U> && !is_std_string_ct<U>::value && Reflected<U>>> {
         using type = std::span<const CView<U>>;
      };

      // std::optional<T> → const mapped_type*
      template <typename U>
      struct cview_field_map<std::optional<U>, void> {
         using type = const typename cview_field_map<U>::type*;
      };

      // Reflected struct → CView<U>
      template <typename U>
      struct cview_field_map<U, std::enable_if_t<
         !is_scalar_v<U> &&
         !is_std_string_ct<U>::value &&
         !is_std_vector_ct<U>::value &&
         !is_std_optional_ct<U>::value &&
         Reflected<U>>> {
         using type = CView<U>;
      };

      template <typename T>
      using cview_t = typename cview_field_map<std::remove_cvref_t<T>>::type;

      // ── is_viewable: zero-alloc CView possible? ──

      template <typename T, typename Enable = void>
      struct is_viewable_impl : std::true_type {};  // scalars

      template <>
      struct is_viewable_impl<std::string, void> : std::true_type {};

      template <typename U>
      struct is_viewable_impl<std::vector<U>, std::enable_if_t<is_scalar_v<U>>>
         : std::true_type {};

      // vector<string> or vector<compound> → NOT viewable (need descriptor alloc)
      template <typename U>
      struct is_viewable_impl<std::vector<U>, std::enable_if_t<!is_scalar_v<U>>>
         : std::false_type {};

      template <typename U>
      struct is_viewable_impl<std::optional<U>, void> : is_viewable_impl<U> {};

      // Reflected struct: viewable if all fields viewable
      template <typename T>
      struct is_viewable_impl<T, std::enable_if_t<
         !is_scalar_v<T> && !is_std_string_ct<T>::value &&
         !is_std_vector_ct<T>::value && !is_std_optional_ct<T>::value &&
         Reflected<T>>>
      {
         static constexpr bool compute() {
            bool result = true;
            apply_members(
               (typename reflect<T>::data_members*)nullptr,
               [&](auto... ptrs) {
                  ((void)(result = result && is_viewable_impl<
                     std::remove_cvref_t<typename MemberPtrType<decltype(ptrs)>::ValueType>
                  >::value), ...);
               }
            );
            return result;
         }
         static constexpr bool value = compute();
      };

      // ── Build tuple of CView field types ──

      template <typename T>
      struct cview_tuple_builder {
         template <auto... M>
         static auto build(MemberList<M...>*) {
            return std::tuple<cview_t<typename MemberPtrType<decltype(M)>::ValueType>...>{};
         }
         using type = decltype(build((typename reflect<T>::data_members*)nullptr));
      };

   } // namespace detail

   // ── Compile-time viewability ──────────────────────────────────────────────

   template <typename T>
   inline constexpr bool is_viewable = detail::is_viewable_impl<std::remove_cvref_t<T>>::value;

   // ── CView<T> ─────────────────────────────────────────────────────────────

   template <typename T>
   struct CView {
      using tuple_type = typename detail::cview_tuple_builder<T>::type;
      tuple_type fields;

      template <size_t I>
      auto& get() { return std::get<I>(fields); }

      template <size_t I>
      const auto& get() const { return std::get<I>(fields); }

      CView() = default;

      // Construct from RichType (borrows data, may alloc descriptor arrays)
      explicit CView(const T& rich) {
         apply_members(
            (typename reflect<T>::data_members*)nullptr,
            [&](auto... ptrs) {
               // C++20 generic lambda with explicit template params.
               // Pairs each tuple index with its corresponding member pointer,
               // so init_field always gets matching dst/src types.
               [&]<size_t... Is>(std::index_sequence<Is...>) {
                  (init_field(std::get<Is>(fields), rich.*ptrs), ...);
               }(std::index_sequence_for<decltype(ptrs)...>{});
            }
         );
      }

      ~CView() {
         for (auto* p : owned_bufs_)
            ::operator delete(p);
      }

      CView(CView&& o) noexcept : fields(std::move(o.fields)), owned_bufs_(std::move(o.owned_bufs_)) {
         o.owned_bufs_.clear();
      }
      CView& operator=(CView&& o) noexcept {
         if (this != &o) {
            for (auto* p : owned_bufs_) ::operator delete(p);
            fields = std::move(o.fields);
            owned_bufs_ = std::move(o.owned_bufs_);
            o.owned_bufs_.clear();
         }
         return *this;
      }
      CView(const CView&) = delete;
      CView& operator=(const CView&) = delete;

      T promote() const {
         T result;
         apply_members(
            (typename reflect<T>::data_members*)nullptr,
            [&](auto... ptrs) {
               [&]<size_t... Is>(std::index_sequence<Is...>) {
                  (promote_field(result.*ptrs, std::get<Is>(fields)), ...);
               }(std::index_sequence_for<decltype(ptrs)...>{});
            }
         );
         return result;
      }

   private:
      std::vector<void*> owned_bufs_;

      // ── Per-type init ──

      // Scalar
      template <typename V>
      static void init_field(V& dst, const V& src)
         requires(detail::is_scalar_v<V>)
      { dst = src; }

      // string → string_view
      static void init_field(std::string_view& dst, const std::string& src)
      { dst = src; }

      // vector<scalar> → span
      template <typename U>
      static void init_field(std::span<const U>& dst, const std::vector<U>& src)
         requires(detail::is_scalar_v<U>)
      { dst = std::span<const U>(src.data(), src.size()); }

      // vector<string> → span<string_view>
      void init_field(std::span<const std::string_view>& dst,
                      const std::vector<std::string>& src) {
         if (src.empty()) { dst = {}; return; }
         auto* buf = new std::string_view[src.size()];
         owned_bufs_.push_back(buf);
         for (size_t i = 0; i < src.size(); i++)
            buf[i] = src[i];
         dst = std::span<const std::string_view>(buf, src.size());
      }

      // vector<reflected> → span<CView<U>>
      template <typename U>
      void init_field(std::span<const CView<U>>& dst, const std::vector<U>& src)
         requires(Reflected<U>)
      {
         if (src.empty()) { dst = {}; return; }
         auto* buf = static_cast<CView<U>*>(::operator new(sizeof(CView<U>) * src.size()));
         owned_bufs_.push_back(buf);
         for (size_t i = 0; i < src.size(); i++)
            new (&buf[i]) CView<U>(src[i]);
         dst = std::span<const CView<U>>(buf, src.size());
      }

      // optional<T> → const view_type*
      template <typename U, typename ViewField>
      void init_field(const ViewField*& dst, const std::optional<U>& src) {
         if (!src.has_value()) { dst = nullptr; return; }
         auto* buf = new ViewField;
         owned_bufs_.push_back(buf);
         init_field(*buf, *src);
         dst = buf;
      }

      // Reflected struct → CView<U>
      template <typename U>
      static void init_field(CView<U>& dst, const U& src)
         requires(Reflected<U>)
      { dst = CView<U>(src); }

      // ── Per-type promote ──

      // Scalar
      template <typename V>
      static void promote_field(V& dst, const V& src)
         requires(detail::is_scalar_v<V>)
      { dst = src; }

      // string_view → string
      static void promote_field(std::string& dst, std::string_view src)
      { dst = std::string(src); }

      // span → vector<scalar>
      template <typename U>
      static void promote_field(std::vector<U>& dst, std::span<const U> src)
         requires(detail::is_scalar_v<U>)
      { dst.assign(src.begin(), src.end()); }

      // span<string_view> → vector<string>
      static void promote_field(std::vector<std::string>& dst,
                                std::span<const std::string_view> src) {
         dst.clear();
         dst.reserve(src.size());
         for (auto sv : src) dst.emplace_back(sv);
      }

      // span<CView<U>> → vector<U>
      template <typename U>
      static void promote_field(std::vector<U>& dst, std::span<const CView<U>> src)
         requires(Reflected<U>)
      {
         dst.clear();
         dst.reserve(src.size());
         for (auto& cv : src) dst.push_back(cv.promote());
      }

      // const view_type* → optional<T>
      template <typename U, typename ViewField>
      static void promote_field(std::optional<U>& dst, const ViewField* src) {
         if (!src) { dst.reset(); return; }
         U val;
         promote_field(val, *src);
         dst = std::move(val);
      }

      // CView<U> → U
      template <typename U>
      static void promote_field(U& dst, const CView<U>& src)
         requires(Reflected<U>)
      { dst = src.promote(); }
   };

   // ── COwned<T> ────────────────────────────────────────────────────────────

   template <typename T>
   struct COwned {
      CView<T> view;

      struct alloc_entry { void* ptr; size_t size; };
      std::vector<alloc_entry> allocs;

      using free_fn_t = void(*)(void*, size_t);
      free_fn_t free_fn = nullptr;

      COwned() = default;

      explicit COwned(CView<T>&& v, free_fn_t fn = nullptr)
         : view(std::move(v)), free_fn(fn) {}

      ~COwned() {
         if (free_fn)
            for (auto& a : allocs)
               free_fn(a.ptr, a.size);
      }

      COwned(COwned&& o) noexcept
         : view(std::move(o.view)), allocs(std::move(o.allocs)), free_fn(o.free_fn)
      { o.free_fn = nullptr; }

      COwned& operator=(COwned&& o) noexcept {
         if (this != &o) {
            this->~COwned();
            new (this) COwned(std::move(o));
         }
         return *this;
      }
      COwned(const COwned&) = delete;
      COwned& operator=(const COwned&) = delete;

      template <size_t I> auto& get() { return view.template get<I>(); }
      template <size_t I> const auto& get() const { return view.template get<I>(); }

      void track(void* ptr, size_t size) { allocs.push_back({ptr, size}); }

      T promote() const { return view.promote(); }
   };

} // namespace psio
