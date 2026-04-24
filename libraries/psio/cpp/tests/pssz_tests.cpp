// pSSZ (PsiSSZ) round-trip and size-trait tests. See .issues/pssz-
// format-design.md for the format spec.

#include <catch2/catch.hpp>

#include <psio/bitset.hpp>
#include <psio/bounded.hpp>
#include <psio/from_pssz.hpp>
#include <psio/pssz_view.hpp>
#include <psio/reflect.hpp>
#include <psio/to_pssz.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using psio::frac_format_pssz16;
using psio::frac_format_pssz32;
using psio::frac_format_pssz8;

TEST_CASE("pssz: primitive round-trip", "[pssz]")
{
   auto rt = [](auto val) {
      auto b = psio::convert_to_pssz<frac_format_pssz32>(val);
      return psio::convert_from_pssz<frac_format_pssz32, decltype(val)>(b);
   };
   REQUIRE(rt(std::uint32_t{0xDEADBEEF}) == 0xDEADBEEF);
   REQUIRE(rt(std::int64_t{-42}) == -42);
   REQUIRE(rt(3.14159) == 3.14159);
   REQUIRE(rt(true) == true);
   REQUIRE(rt(false) == false);
}

TEST_CASE("pssz: string / vector — no length prefix, size from span", "[pssz]")
{
   std::string s = "hello";
   auto        b = psio::convert_to_pssz<frac_format_pssz32>(s);
   REQUIRE(b.size() == 5);  // no length prefix
   REQUIRE(psio::convert_from_pssz<frac_format_pssz32, std::string>(b) == s);

   std::vector<std::uint64_t> v = {1, 2, 3};
   auto                       bv = psio::convert_to_pssz<frac_format_pssz32>(v);
   REQUIRE(bv.size() == 24);  // 3 × 8 bytes, no offsets needed (fixed element)
   REQUIRE(psio::convert_from_pssz<frac_format_pssz32, decltype(v)>(bv) == v);
}

TEST_CASE("pssz: optional needs selector iff min == 0", "[pssz]")
{
   // Fixed-size inner T — no selector byte. None = 0 bytes, Some =
   // sizeof(T). Span adjacency disambiguates.
   static_assert(!psio::pssz_optional_needs_selector<std::uint32_t>);
   // Variable inner T — selector required.
   static_assert(psio::pssz_optional_needs_selector<std::string>);
   static_assert(psio::pssz_optional_needs_selector<std::vector<std::uint8_t>>);

   // std::optional<uint32_t> at top level: wrapped in... well, we need an
   // enclosing container so the span boundary is meaningful. Top-level
   // from_pssz doesn't differentiate None from empty for no-selector
   // optionals — by design, that's the enclosing-container's job.

   // std::optional<string> at top level: selector IS present.
   std::optional<std::string> some = std::string("x");
   auto                       sb   = psio::convert_to_pssz<frac_format_pssz32>(some);
   REQUIRE(sb.size() == 2);  // 1 selector + 1 char
   auto rt_some =
       psio::convert_from_pssz<frac_format_pssz32, std::optional<std::string>>(sb);
   REQUIRE(rt_some.has_value());
   REQUIRE(*rt_some == "x");

   std::optional<std::string> none;
   auto                       nb = psio::convert_to_pssz<frac_format_pssz32>(none);
   REQUIRE(nb.size() == 1);  // just the selector
   auto rt_none =
       psio::convert_from_pssz<frac_format_pssz32, std::optional<std::string>>(nb);
   REQUIRE(!rt_none.has_value());
}

namespace pssz_test
{
   struct BPoint
   {
      double x, y;
   };
   PSIO_REFLECT(BPoint, definitionWillNotChange(), x, y)

   struct UserProfile
   {
      std::uint64_t              id;
      std::string                name;
      std::optional<std::string> bio;
      std::uint32_t              age;
      double                     score;
      std::vector<std::string>   tags;
      bool                       verified;
   };
   PSIO_REFLECT(UserProfile, id, name, bio, age, score, tags, verified)

   struct BoundedProfile
   {
      std::uint64_t                              id;
      psio::bounded_string<64>                    name;
      psio::bounded_string<256>                   bio;
      std::uint32_t                              age;
      psio::bounded_list<psio::bounded_string<32>, 8> tags;
   };
   PSIO_REFLECT(BoundedProfile, id, name, bio, age, tags)
}  // namespace pssz_test

TEST_CASE("pssz: reflected DWNC memcpy fast path", "[pssz]")
{
   pssz_test::BPoint p{1.5, -2.5};
   auto              b = psio::convert_to_pssz<frac_format_pssz32>(p);
   REQUIRE(b.size() == 16);  // DWNC: no header, memcpy sizeof(BPoint)
   auto rt = psio::convert_from_pssz<frac_format_pssz32, pssz_test::BPoint>(b);
   REQUIRE(rt.x == 1.5);
   REQUIRE(rt.y == -2.5);
}

TEST_CASE("pssz: reflected extensible container round-trip", "[pssz]")
{
   pssz_test::UserProfile u{42,
                            "Alice",
                            std::string("Bio"),
                            30,
                            99.5,
                            {"t1", "t2"},
                            true};
   auto                   b = psio::convert_to_pssz<frac_format_pssz32>(u);
   auto                   rt =
       psio::convert_from_pssz<frac_format_pssz32, pssz_test::UserProfile>(b);
   REQUIRE(rt.id == u.id);
   REQUIRE(rt.name == u.name);
   REQUIRE(rt.bio.has_value());
   REQUIRE(*rt.bio == *u.bio);
   REQUIRE(rt.age == u.age);
   REQUIRE(rt.score == u.score);
   REQUIRE(rt.tags == u.tags);
   REQUIRE(rt.verified == u.verified);

   // None optional round-trip.
   pssz_test::UserProfile u2   = u;
   u2.bio                      = std::nullopt;
   u2.tags                     = {};
   auto                   b2   = psio::convert_to_pssz<frac_format_pssz32>(u2);
   auto                   rt2  =
       psio::convert_from_pssz<frac_format_pssz32, pssz_test::UserProfile>(b2);
   REQUIRE(!rt2.bio.has_value());
   REQUIRE(rt2.tags.empty());
}

TEST_CASE("pssz: auto_pssz_format picks narrowest width", "[pssz]")
{
   // Unbounded types fall back to pssz32.
   static_assert(std::is_same_v<psio::auto_pssz_format_t<pssz_test::UserProfile>,
                                 frac_format_pssz32>);

   // DWNC all-fixed small struct → max = sizeof(T) = 16, fits in pssz8.
   static_assert(std::is_same_v<psio::auto_pssz_format_t<pssz_test::BPoint>,
                                 frac_format_pssz8>);

   // Fully bounded extensible struct → pssz16 or pssz32 based on total.
   constexpr auto bp_max = psio::max_encoded_size<pssz_test::BoundedProfile>();
   static_assert(bp_max.has_value());
   // id(8) + age(4) + 4 header + 4+64 name + 4+256 bio + 4+ (8*(4+32)) tags
   //  = 12 + 4 + 68 + 260 + 4 + 288 = 636 — fits in pssz16 (< 64 KiB).
   static_assert(std::is_same_v<psio::auto_pssz_format_t<pssz_test::BoundedProfile>,
                                 frac_format_pssz16>);
}

TEST_CASE("generic bounded<T,N> basic usage", "[bounded]")
{
   // String variant.
   psio::bounded<std::string, 64> s{std::string("hello")};
   REQUIRE(s.size() == 5);
   REQUIRE(s.view() == "hello");
   REQUIRE_THROWS(psio::bounded<std::string, 4>(std::string("too long")));

   // Vector variant.
   psio::bounded<std::vector<int>, 8> v{std::vector<int>{1, 2, 3}};
   REQUIRE(v.size() == 3);
   REQUIRE(v.storage() == std::vector<int>{1, 2, 3});

   // Detection trait.
   static_assert(psio::is_bounded_v<psio::bounded<std::string, 32>>);
   static_assert(!psio::is_bounded_v<std::string>);
   static_assert(!psio::is_bounded_v<int>);
}

TEST_CASE("pssz: view primitive and string", "[pssz][view]")
{
   auto b_u32 = psio::convert_to_pssz<frac_format_pssz32>(std::uint32_t{0xCAFEBABE});
   auto v_u32 = psio::pssz_view_of<std::uint32_t, frac_format_pssz32>(b_u32);
   REQUIRE(v_u32.get() == 0xCAFEBABE);

   std::string s = "hello, world";
   auto        bs = psio::convert_to_pssz<frac_format_pssz32>(s);
   auto        vs = psio::pssz_view_of<std::string, frac_format_pssz32>(bs);
   REQUIRE(vs.view() == s);
   REQUIRE(vs.size() == s.size());
}

TEST_CASE("pssz: view reflected DWNC struct (BPoint)", "[pssz][view]")
{
   pssz_test::BPoint p{1.5, -2.5};
   auto              buf = psio::convert_to_pssz<frac_format_pssz32>(p);
   auto              v   = psio::pssz_view_of<pssz_test::BPoint,
                                   frac_format_pssz32>(buf);
   // Named accessors from PSIO_REFLECT (v.x(), v.y() etc.) — same
   // proxy pattern as frac_view / ssz_view. No field<I>() needed.
   REQUIRE(double(v.x()) == 1.5);
   REQUIRE(double(v.y()) == -2.5);
}

TEST_CASE("pssz: view reflected extensible struct (UserProfile)", "[pssz][view]")
{
   pssz_test::UserProfile u{42,
                            "Alice Johnson",
                            std::string("Bio text here"),
                            30,
                            99.5,
                            {"tag1", "tag2", "tag3"},
                            true};
   auto buf = psio::convert_to_pssz<frac_format_pssz32>(u);
   auto v   = psio::pssz_view_of<pssz_test::UserProfile,
                                   frac_format_pssz32>(buf);

   REQUIRE(std::uint64_t(v.id()) == 42);
   REQUIRE(v.name().view() == "Alice Johnson");
   auto opt_bio = v.bio();
   REQUIRE(opt_bio.has_value());
   REQUIRE((*opt_bio).view() == "Bio text here");
   REQUIRE(std::uint32_t(v.age()) == 30);
   REQUIRE(double(v.score()) == 99.5);
   auto tags = v.tags();
   REQUIRE(tags.size() == 3);
   REQUIRE(tags[0].view() == "tag1");
   REQUIRE(tags[2].view() == "tag3");
   REQUIRE(bool(v.verified()) == true);
}

TEST_CASE("pssz: bounded type uses narrower pssz16 successfully", "[pssz]")
{
   pssz_test::BoundedProfile bp{
       42, psio::bounded_string<64>{std::string("Alice Johnson")},
       psio::bounded_string<256>{std::string("Software engineer")},
       30,
       psio::bounded_list<psio::bounded_string<32>, 8>{
           std::vector<psio::bounded_string<32>>{
               psio::bounded_string<32>{std::string("tag1")},
               psio::bounded_string<32>{std::string("tag2")}}}};

   using F = psio::auto_pssz_format_t<pssz_test::BoundedProfile>;
   static_assert(std::is_same_v<F, frac_format_pssz16>);

   auto b = psio::convert_to_pssz<F>(bp);
   auto rt =
       psio::convert_from_pssz<F, pssz_test::BoundedProfile>(b);
   REQUIRE(rt.id == bp.id);
   REQUIRE(rt.name.storage() == bp.name.storage());
   REQUIRE(rt.bio.storage() == bp.bio.storage());
   REQUIRE(rt.age == bp.age);
   REQUIRE(rt.tags.size() == bp.tags.size());
}
