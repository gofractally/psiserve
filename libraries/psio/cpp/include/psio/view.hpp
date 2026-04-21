// psio/view.hpp — Unified zero-copy view over serialized data
//
// view<T, Fmt> provides named field accessors over any wire format
// (FlatBuffer, fracpack, etc.) driven by PSIO_REFLECT.  The Format
// tag determines how bytes are navigated; the view API is identical
// regardless of format.
//
//   auto fv = psio::view<Order, psio::fb>::from_buffer(fb_buf);
//   auto pv = psio::view<Order, psio::frac>::from_buffer(frac_buf);
//
//   // Same API, zero overhead, compiler sees everything:
//   fv.customer().email()    // string_view
//   pv.customer().email()    // string_view
//
// Container views mirror their std counterparts:
//   v.items()[0]             // vec_view — index access
//   v.ids().contains(42)     // set_view — sorted lookup
//   v.counts().at("k")       // map_view — key lookup
//   for (auto [k,v] : v.counts())  // iteration + structured bindings
//
// Format tag interface — Fmt must provide:
//   using ptr_t;
//   static ptr_t root<T>(const void* buf);
//   static auto  field<T, N>(ptr_t data);
//
// Container reader interface (used by container view specializations):
//   Defined per-format via partial specialization of container views.

#pragma once

#include <psio/reflect.hpp>

#include <compare>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace psio {

// ── View-type mapping: schema type → zero-copy read type ────────────────

template <typename T>
struct view_type
{
   using type = T;
};
template <>
struct view_type<std::string>
{
   using type = std::string_view;
};

template <typename T>
using view_type_t = typename view_type<T>::type;

// ── Index-based iterator for container views ────────────────────────────
//
// Container is the concrete container view (vec_view, set_view, map_view).
// ReadFn(const Container&, uint32_t) returns the element at index i.
// This is a random-access iterator so range-for, algorithms, and
// structured bindings all work naturally.

template <typename Container, typename ReadFn>
class index_iterator
{
   const Container* c_ = nullptr;
   uint32_t         i_ = 0;

  public:
   using iterator_category = std::random_access_iterator_tag;
   using difference_type   = int32_t;
   using value_type        = decltype(ReadFn{}(std::declval<const Container&>(), 0));
   using reference         = value_type;
   using pointer           = void;

   index_iterator() = default;
   index_iterator(const Container* c, uint32_t i) : c_(c), i_(i) {}

   reference operator*() const { return ReadFn{}(*c_, i_); }
   reference operator[](difference_type n) const { return ReadFn{}(*c_, i_ + n); }

   index_iterator& operator++()
   {
      ++i_;
      return *this;
   }
   index_iterator operator++(int)
   {
      auto tmp = *this;
      ++i_;
      return tmp;
   }
   index_iterator& operator--()
   {
      --i_;
      return *this;
   }
   index_iterator operator--(int)
   {
      auto tmp = *this;
      --i_;
      return tmp;
   }
   index_iterator& operator+=(difference_type n)
   {
      i_ += n;
      return *this;
   }
   index_iterator& operator-=(difference_type n)
   {
      i_ -= n;
      return *this;
   }

   friend index_iterator operator+(index_iterator it, difference_type n) { return {it.c_, it.i_ + static_cast<uint32_t>(n)}; }
   friend index_iterator operator+(difference_type n, index_iterator it) { return {it.c_, it.i_ + static_cast<uint32_t>(n)}; }
   friend index_iterator operator-(index_iterator it, difference_type n) { return {it.c_, it.i_ - static_cast<uint32_t>(n)}; }
   friend difference_type operator-(index_iterator a, index_iterator b) { return static_cast<difference_type>(a.i_) - static_cast<difference_type>(b.i_); }

   auto operator<=>(const index_iterator& o) const = default;
};

// ── Forward declarations ────────────────────────────────────────────────

template <typename T, typename Fmt>
class view;

template <typename K, typename Fmt>
class set_view;

template <typename K, typename V, typename Fmt>
class map_view;

template <typename E, typename Fmt>
class vec_view;

// ── view_proxy — the ProxyObject for PSIO_REFLECT's proxy<> ────────────
//
// Each named accessor (v.name(), v.id(), etc.) calls get<I, MemberPtr>()
// which delegates to Fmt::field<T, I>(ptr_).

template <typename T, typename Fmt>
class view_proxy
{
   typename Fmt::ptr_t ptr_;

  public:
   explicit view_proxy(typename Fmt::ptr_t p = {}) : ptr_(p) {}
   typename Fmt::ptr_t ptr() const { return ptr_; }

   template <int I, auto MemberPtr>
   auto get() const
   {
      return Fmt::template field<T, static_cast<size_t>(I)>(ptr_);
   }
};

// ── view<T, Fmt> — the unified read-only struct view ───────────────────

template <typename T, typename Fmt>
class view : public reflect<T>::template proxy<view_proxy<T, Fmt>>
{
   using base = typename reflect<T>::template proxy<view_proxy<T, Fmt>>;

  public:
   view() : base(typename Fmt::ptr_t{}) {}
   explicit view(typename Fmt::ptr_t p) : base(p) {}

   explicit operator bool() const { return this->psio_get_proxy().ptr() != nullptr; }

   typename Fmt::ptr_t data() const { return this->psio_get_proxy().ptr(); }

   static view from_buffer(const void* buf)
   {
      return view(Fmt::template root<T>(buf));
   }

   template <size_t N>
   auto get() const
   {
      return Fmt::template field<T, N>(this->psio_get_proxy().ptr());
   }
};

// ── Free-function get<I> for view — enables tuple_call / structured bindings

template <std::size_t I, typename T, typename Fmt>
auto get(const view<T, Fmt>& v)
{
   return v.template get<I>();
}

}  // namespace psio (temporarily close for std specialization)

// ── tuple_size / tuple_element for view<tuple<...>, Fmt> ────────────────
// Enables std::apply, structured bindings, and view_call on tuple views.

template <typename... Ts, typename Fmt>
struct std::tuple_size<psio::view<std::tuple<Ts...>, Fmt>>
   : std::integral_constant<std::size_t, sizeof...(Ts)> {};

template <typename... Ts, typename Fmt>
struct std::tuple_size<psio::view<const std::tuple<Ts...>, Fmt>>
   : std::integral_constant<std::size_t, sizeof...(Ts)> {};

template <std::size_t I, typename... Ts, typename Fmt>
struct std::tuple_element<I, psio::view<std::tuple<Ts...>, Fmt>>
{
   using type = decltype(std::declval<psio::view<std::tuple<Ts...>, Fmt>>().template get<I>());
};

template <std::size_t I, typename... Ts, typename Fmt>
struct std::tuple_element<I, psio::view<const std::tuple<Ts...>, Fmt>>
{
   using type = decltype(std::declval<psio::view<const std::tuple<Ts...>, Fmt>>().template get<I>());
};

namespace psio {   // reopen

// ── CRTP bases for sorted container algorithms ──────────────────────────
//
// Derived classes provide: read_key(i), size()  [and read_value(i) for maps]
// The CRTP base provides: contains(), find(), lower_bound()

template <typename Derived>
class sorted_set_algo
{
   const Derived& self() const { return static_cast<const Derived&>(*this); }

  public:
   bool contains(const auto& key) const { return find(key) != self().size(); }

   uint32_t find(const auto& key) const
   {
      uint32_t lo = 0, hi = self().size();
      while (lo < hi)
      {
         uint32_t mid = lo + (hi - lo) / 2;
         auto     mk  = self().read_key(mid);
         if (mk < key)
            lo = mid + 1;
         else if (key < mk)
            hi = mid;
         else
            return mid;
      }
      return self().size();
   }

   uint32_t lower_bound(const auto& key) const
   {
      uint32_t lo = 0, hi = self().size();
      while (lo < hi)
      {
         uint32_t mid = lo + (hi - lo) / 2;
         if (self().read_key(mid) < key)
            lo = mid + 1;
         else
            hi = mid;
      }
      return lo;
   }
};

template <typename Derived>
class sorted_map_algo
{
   const Derived& self() const { return static_cast<const Derived&>(*this); }

  public:
   uint32_t find_index(const auto& key) const
   {
      uint32_t lo = 0, hi = self().size();
      while (lo < hi)
      {
         uint32_t mid = lo + (hi - lo) / 2;
         auto     mk  = self().read_key(mid);
         if (mk < key)
            lo = mid + 1;
         else if (key < mk)
            hi = mid;
         else
            return mid;
      }
      return self().size();
   }

   bool contains(const auto& key) const { return find_index(key) < self().size(); }

   uint32_t lower_bound(const auto& key) const
   {
      uint32_t lo = 0, hi = self().size();
      while (lo < hi)
      {
         uint32_t mid = lo + (hi - lo) / 2;
         if (self().read_key(mid) < key)
            lo = mid + 1;
         else
            hi = mid;
      }
      return lo;
   }
};

// ── set_view<K, Fmt> — primary template ─────────────────────────────────
//
// Specialized per format in the format's header.  Each specialization
// inherits sorted_set_algo and provides read_key(i), size().
//
// API mirrors std::set (const):
//   size(), contains(), find()→iterator, lower_bound()→iterator,
//   begin()/end() for sorted iteration.  No operator[].

template <typename K, typename Fmt>
class set_view;  // specialized per format

// ── map_view<K, V, Fmt> — primary template ──────────────────────────────
//
// API mirrors std::map (const):
//   size(), contains(), at(key), find(key)→iterator,
//   lower_bound()→iterator, begin()/end() yielding key-value pairs.
//   No operator[] on const view (std::map's operator[] is non-const).

template <typename K, typename V, typename Fmt>
class map_view;  // specialized per format

// ── vec_view<E, Fmt> — primary template ─────────────────────────────────
//
// API mirrors std::vector (const):
//   size(), empty(), operator[](uint32_t), at(uint32_t),
//   begin()/end() for iteration.

template <typename E, typename Fmt>
class vec_view;  // specialized per format

}  // namespace psio
