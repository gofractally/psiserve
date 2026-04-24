// Phase 3 — buffer/view/mutable_view + access-surface rule.

#include <psio3/buffer.hpp>
#include <psio3/mutable_view.hpp>
#include <psio3/storage.hpp>
#include <psio3/view.hpp>

#include <catch.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {

   struct toy_fmt
   {
   };

}  // namespace

TEST_CASE("buffer<T, Fmt, owning> holds bytes, round-trips through bytes()",
          "[buffer]")
{
   std::vector<char> raw = {'\x01', '\x02', '\x03', '\x04'};
   psio3::buffer<std::uint32_t, toy_fmt> b{raw};

   auto span = psio3::bytes(b);
   REQUIRE(span.size() == 4);
   REQUIRE(static_cast<unsigned char>(span[0]) == 0x01);
   REQUIRE(static_cast<unsigned char>(span[3]) == 0x04);
   REQUIRE(psio3::size(b) == 4);
}

TEST_CASE("buffer storage_kind tag reflects the template parameter",
          "[buffer]")
{
   using OwningBuf = psio3::buffer<std::uint32_t, toy_fmt, psio3::storage::owning>;
   using ConstBuf  = psio3::buffer<std::uint32_t, toy_fmt, psio3::storage::const_borrow>;
   using MutBuf    = psio3::buffer<std::uint32_t, toy_fmt, psio3::storage::mut_borrow>;

   STATIC_REQUIRE(OwningBuf::storage_kind == psio3::storage::owning);
   STATIC_REQUIRE(ConstBuf::storage_kind  == psio3::storage::const_borrow);
   STATIC_REQUIRE(MutBuf::storage_kind    == psio3::storage::mut_borrow);
}

TEST_CASE("const_borrow buffer borrows without copying", "[buffer]")
{
   std::vector<char> raw = {'\x10', '\x20', '\x30', '\x40'};
   psio3::buffer<std::uint32_t, toy_fmt, psio3::storage::const_borrow> b{
      std::span<const char>{raw.data(), raw.size()}};

   auto span = psio3::bytes(b);
   REQUIRE(span.data() == raw.data());  // no copy
   REQUIRE(span.size() == raw.size());
}

TEST_CASE("to_buffer upgrades any storage variant to owning", "[buffer]")
{
   std::vector<char> raw = {'\xAA', '\xBB'};
   psio3::buffer<std::uint32_t, toy_fmt, psio3::storage::const_borrow> borrow{
      std::span<const char>{raw.data(), raw.size()}};
   auto owned = psio3::to_buffer(borrow);
   REQUIRE(decltype(owned)::storage_kind == psio3::storage::owning);
   REQUIRE(psio3::bytes(owned).size() == 2);

   // Mutating the source must NOT affect the owned buffer.
   raw[0] = '\x00';
   REQUIRE(static_cast<unsigned char>(psio3::bytes(owned)[0]) == 0xAA);
}

TEST_CASE("format_of returns the format tag by value", "[buffer]")
{
   psio3::buffer<std::uint32_t, toy_fmt> b;
   auto fmt = psio3::format_of(b);
   STATIC_REQUIRE(std::is_same_v<decltype(fmt), toy_fmt>);
}

TEST_CASE("view<Primitive, Fmt> exposes .get() shape-terminator",
          "[view][access-surface]")
{
   std::vector<char> raw = {'\x01', '\x00', '\x00', '\x00'};  // LE u32 = 1
   psio3::view<std::uint32_t, toy_fmt> v{
      std::span<const char>{raw.data(), raw.size()}};

   REQUIRE(v.get() == 1u);
}

TEST_CASE("view<string, Fmt> exposes .view_() yielding a string_view",
          "[view][access-surface]")
{
   std::string raw = "hello";
   psio3::view<std::string, toy_fmt> v{
      std::span<const char>{raw.data(), raw.size()}};

   REQUIRE(v.view_() == "hello");
   REQUIRE(psio3::size(v) == 5);
}

TEST_CASE("as_view(buffer) produces a borrowing view", "[view]")
{
   std::vector<char> raw = {'\x01', '\x02', '\x03', '\x04'};
   psio3::buffer<std::uint32_t, toy_fmt> buf{raw};

   auto v = psio3::as_view(buf);
   REQUIRE(psio3::bytes(v).data() == psio3::bytes(buf).data());
   REQUIRE(psio3::size(v) == psio3::size(buf));
}

// ── Access-surface rule verification ──────────────────────────────────────
//
// Per design § 5.5 the view's `.` surface should expose ONLY shape-
// appropriate accessors (Primitive → .get(), string → .view_()), not
// library internals like `.data()` / `.size()` / `.format()`. Those
// live as free functions.
//
// We verify this via SFINAE-style detection: attempting to call a
// member function the view SHOULDN'T have must fail SFINAE, i.e.
// not be detected.

namespace detect {
   template <typename, typename = void>
   struct has_data_method : std::false_type
   {
   };
   template <typename T>
   struct has_data_method<T, std::void_t<decltype(std::declval<T>().data())>>
      : std::true_type
   {
   };

   template <typename, typename = void>
   struct has_size_method : std::false_type
   {
   };
   template <typename T>
   struct has_size_method<T, std::void_t<decltype(std::declval<T>().size())>>
      : std::true_type
   {
   };

   template <typename, typename = void>
   struct has_format_method : std::false_type
   {
   };
   template <typename T>
   struct has_format_method<T, std::void_t<decltype(std::declval<T>().format())>>
      : std::true_type
   {
   };
}

TEST_CASE("view<T, Fmt> exposes NO storage / format methods on . surface",
          "[view][access-surface]")
{
   using V = psio3::view<std::uint32_t, toy_fmt>;

   // These must all be FALSE: the view does not expose storage or
   // format operations as methods. Users must call psio3::bytes(v),
   // psio3::size(v), psio3::format_of(v) as free functions.
   STATIC_REQUIRE(!detect::has_data_method<V>::value);
   STATIC_REQUIRE(!detect::has_size_method<V>::value);
   STATIC_REQUIRE(!detect::has_format_method<V>::value);
}

TEST_CASE("buffer<T, Fmt> exposes NO storage / format methods on . surface",
          "[buffer][access-surface]")
{
   using B = psio3::buffer<std::uint32_t, toy_fmt>;

   STATIC_REQUIRE(!detect::has_data_method<B>::value);
   STATIC_REQUIRE(!detect::has_size_method<B>::value);
   STATIC_REQUIRE(!detect::has_format_method<B>::value);
}
