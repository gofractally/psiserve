#pragma once
// Bounded collection types with compile-time capacity (analogous to SSZ List[T, N]).
//
//   bounded_length_t<N> — smallest unsigned integer type that can hold value N
//   bounded_list<T, N>  — variable-length sequence of up to N Ts
//   bounded_string<N>   — variable-length UTF-8 string up to N bytes
//   bounded_bytes<N>    — alias for bounded_list<uint8_t, N>
//
// Wire encoding (per format):
//   - fracpack:  [length_prefix: bounded_length_t<N*sizeof(T)>][data]
//                smaller prefix than std::vector's u32 when N fits in u8/u16
//   - bincode:   [u64 count][data]  (spec-mandated u64 prefix; bound validated on unpack)
//   - borsh:     [u32 count][data]  (spec-mandated u32 prefix; bound validated on unpack)
//   - pack_bin:  [varuint count][data]  (same as std::vector; bound validated on unpack)
//
// The compile-time bound N is:
//   - A hard invariant: unpacking rejects count > N
//   - A schema-emission hint: exported as maxCount/maxLength annotation
//   - An encoding optimization for fracpack
//   - A prerequisite for SSZ's Merkle tree depth

#include <psio1/check.hpp>
#include <psio1/ext_int.hpp>
#include <psio1/stream.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace psio1
{
   // ── bounded_length_t<N>: smallest unsigned int that can represent N ────────

   template <std::size_t N>
   struct bounded_length_impl
   {
      using type = std::conditional_t<
          (N < (std::size_t{1} << 8)),
          std::uint8_t,
          std::conditional_t<
              (N < (std::size_t{1} << 16)),
              std::uint16_t,
              std::conditional_t<(N < (std::size_t{1} << 32)), std::uint32_t, std::uint64_t>>>;
   };

   template <std::size_t N>
   using bounded_length_t = typename bounded_length_impl<N>::type;

   static_assert(std::is_same_v<bounded_length_t<0>,   std::uint8_t>);
   static_assert(std::is_same_v<bounded_length_t<255>, std::uint8_t>);
   static_assert(std::is_same_v<bounded_length_t<256>, std::uint16_t>);
   static_assert(std::is_same_v<bounded_length_t<65535>, std::uint16_t>);
   static_assert(std::is_same_v<bounded_length_t<65536>, std::uint32_t>);

   // ── bounded_list<T, N> ─────────────────────────────────────────────────────

   template <typename T, std::size_t N>
   class bounded_list
   {
     public:
      using value_type = T;
      using size_type  = std::size_t;

      static constexpr std::size_t max_size_v = N;

      constexpr bounded_list() = default;
      bounded_list(std::initializer_list<T> init) : data_(init) { check_bound(); }
      explicit bounded_list(std::vector<T> v) : data_(std::move(v)) { check_bound(); }

      const std::vector<T>& storage() const noexcept { return data_; }
      std::vector<T>&       storage() noexcept { return data_; }

      std::size_t size() const noexcept { return data_.size(); }
      bool        empty() const noexcept { return data_.empty(); }
      static constexpr std::size_t max_size() noexcept { return N; }

      const T* data() const noexcept { return data_.data(); }
      T*       data() noexcept { return data_.data(); }

      const T& operator[](std::size_t i) const noexcept { return data_[i]; }
      T&       operator[](std::size_t i) noexcept { return data_[i]; }

      auto begin() const noexcept { return data_.begin(); }
      auto end() const noexcept { return data_.end(); }
      auto begin() noexcept { return data_.begin(); }
      auto end() noexcept { return data_.end(); }

      void push_back(const T& v)
      {
         data_.push_back(v);
         check_bound();
      }
      void push_back(T&& v)
      {
         data_.push_back(std::move(v));
         check_bound();
      }
      void resize(std::size_t n)
      {
         check(n <= N, "bounded_list overflow");
         data_.resize(n);
      }
      void clear() noexcept { data_.clear(); }

      bool operator==(const bounded_list&) const = default;

     private:
      void check_bound() const { check(data_.size() <= N, "bounded_list overflow"); }

      std::vector<T> data_;
   };

   // ── bounded_string<N> ──────────────────────────────────────────────────────

   template <std::size_t N>
   class bounded_string
   {
     public:
      using value_type = char;
      using size_type  = std::size_t;

      static constexpr std::size_t max_size_v = N;

      constexpr bounded_string() = default;
      bounded_string(const char* s) : data_(s) { check_bound(); }
      bounded_string(std::string s) : data_(std::move(s)) { check_bound(); }
      bounded_string(std::string_view sv) : data_(sv) { check_bound(); }

      const std::string& storage() const noexcept { return data_; }
      std::string&       storage() noexcept { return data_; }

      std::size_t size() const noexcept { return data_.size(); }
      bool        empty() const noexcept { return data_.empty(); }
      static constexpr std::size_t max_size() noexcept { return N; }

      const char* data() const noexcept { return data_.data(); }
      char*       data() noexcept { return data_.data(); }

      std::string_view view() const noexcept { return data_; }

      void clear() noexcept { data_.clear(); }

      bool operator==(const bounded_string&) const = default;

     private:
      void check_bound() const { check(data_.size() <= N, "bounded_string overflow"); }

      std::string data_;
   };

   // ── bounded_bytes<N>: alias for bounded_list<uint8_t, N> ───────────────────

   template <std::size_t N>
   using bounded_bytes = bounded_list<std::uint8_t, N>;

   // ── Type traits for dispatch ──────────────────────────────────────────────

   template <typename T>
   struct is_bounded_list : std::false_type
   {
   };
   template <typename T, std::size_t N>
   struct is_bounded_list<bounded_list<T, N>> : std::true_type
   {
   };

   template <typename T>
   inline constexpr bool is_bounded_list_v = is_bounded_list<T>::value;

   template <typename T>
   struct is_bounded_string : std::false_type
   {
   };
   template <std::size_t N>
   struct is_bounded_string<bounded_string<N>> : std::true_type
   {
   };

   template <typename T>
   inline constexpr bool is_bounded_string_v = is_bounded_string<T>::value;

   // ── Generic bounded<T, N> wrapper ─────────────────────────────────────────
   //
   // A single class that takes any container-like T (std::vector<U>,
   // std::string, std::array, …) plus a compile-time capacity cap N. Format
   // code can read `.storage()` → T& generically, freeing each encoder from
   // writing separate specializations for bounded_list, bounded_string, and
   // bounded_bytes. Legacy aliases below preserve source compatibility.
   //
   // Construction is forwarded: anything that can construct T can construct
   // bounded<T, N>. Examples:
   //   bounded<std::string, 64> s("hello");
   //   bounded<std::vector<int>, 8> v{1, 2, 3};
   //
   // Methods present only when T supports them (view() for string, etc.)
   // are SFINAE'd via requires, so bounded<T,N> has no vestigial members.
   template <typename T, std::size_t N>
   class bounded
   {
     public:
      using inner_type = T;
      static constexpr std::size_t max_size_v = N;

      constexpr bounded() = default;

      template <typename U>
         requires(std::constructible_from<T, U> &&
                  !std::is_same_v<std::remove_cvref_t<U>, bounded>)
      bounded(U&& u) : data_(std::forward<U>(u)) { check_bound(); }

      template <typename U>
      bounded(std::initializer_list<U> il)
         requires std::constructible_from<T, std::initializer_list<U>>
         : data_(il)
      {
         check_bound();
      }

      const T& storage() const noexcept { return data_; }
      T&       storage() noexcept { return data_; }

      std::size_t size() const noexcept { return data_.size(); }
      bool        empty() const noexcept { return data_.empty(); }
      static constexpr std::size_t max_size() noexcept { return N; }

      auto* data() noexcept
         requires requires(T& t) { t.data(); }
      {
         return data_.data();
      }
      auto* data() const noexcept
         requires requires(const T& t) { t.data(); }
      {
         return data_.data();
      }

      // String-only: view() returns string_view
      std::string_view view() const noexcept
         requires std::is_same_v<T, std::string>
      {
         return data_;
      }

      bool operator==(const bounded&) const = default;

     private:
      void check_bound() const { check(data_.size() <= N, "bounded overflow"); }
      T data_{};
   };

   // Detection trait
   template <typename T>
   struct is_bounded : std::false_type
   {
   };
   template <typename T, std::size_t N>
   struct is_bounded<bounded<T, N>> : std::true_type
   {
   };
   template <typename T>
   inline constexpr bool is_bounded_v = is_bounded<T>::value;

}  // namespace psio1
