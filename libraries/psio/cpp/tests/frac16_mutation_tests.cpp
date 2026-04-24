// frac16 mutation tests: exercise field_handle write paths at frac_format_16.
//
// Mirrors the mutation sections of frac_ref_tests.cpp, but against
// frac16_ref<T, std::vector<char>>. The parametrized patching machinery
// (write_size<Format>, splice_buffer, patch_all<..., Format, ...>,
// patch_level_simd with Format::size_type slots) must behave correctly.

#include <catch2/catch.hpp>

#include <psio/frac_ref.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// ── Test structs ─────────────────────────────────────────────────────────

struct M16Simple
{
   std::uint32_t id;
   std::string   name;
   std::uint32_t count;
};
PSIO_REFLECT(M16Simple, id, name, count)

struct M16AllFixed
{
   std::uint32_t x;
   std::uint32_t y;
   std::uint32_t z;
};
PSIO_REFLECT(M16AllFixed, definitionWillNotChange(), x, y, z)

struct M16MultiVar
{
   std::uint32_t id;
   std::string   first;
   std::string   second;
   std::string   third;
};
PSIO_REFLECT(M16MultiVar, id, first, second, third)

struct M16Address
{
   std::string street;
   std::string city;
};
PSIO_REFLECT(M16Address, street, city)

struct M16Person
{
   std::uint32_t id;
   M16Address    addr;
   std::string   note;
};
PSIO_REFLECT(M16Person, id, addr, note)

// ── Helpers ──────────────────────────────────────────────────────────────

template <typename T>
static std::vector<char> pack16(const T& v)
{
   return psio::to_frac16(v);
}

template <typename T>
using ref16_rw = psio::frac16_ref<T, std::vector<char>>;

// ── Fixed-size field mutation ────────────────────────────────────────────

TEST_CASE("frac16 mutation: fixed field overwrite", "[fracpack16][mutation]")
{
   M16Simple orig{10, "test", 20};
   auto      doc = ref16_rw<M16Simple>(pack16(orig));

   SECTION("write id")
   {
      doc.fields().id() = std::uint32_t{42};
      REQUIRE(std::uint32_t(doc.fields().id()) == 42);
      REQUIRE(std::string(doc.fields().name()) == "test");
      REQUIRE(std::uint32_t(doc.fields().count()) == 20);
      REQUIRE(doc.validate() != psio::validation_t::invalid);
   }

   SECTION("write count and verify via full unpack")
   {
      doc.fields().count() = std::uint32_t{999};
      auto out             = doc.unpack();
      REQUIRE(out.id == 10);
      REQUIRE(out.name == "test");
      REQUIRE(out.count == 999);
   }
}

// ── Variable-size field mutation ─────────────────────────────────────────

TEST_CASE("frac16 mutation: variable field (same length)", "[fracpack16][mutation]")
{
   M16Simple orig{1, "hello", 2};
   auto      doc = ref16_rw<M16Simple>(pack16(orig));

   doc.fields().name() = std::string("world");
   REQUIRE(std::string(doc.fields().name()) == "world");
   REQUIRE(std::uint32_t(doc.fields().id()) == 1);
   REQUIRE(std::uint32_t(doc.fields().count()) == 2);
   REQUIRE(doc.validate() != psio::validation_t::invalid);
}

TEST_CASE("frac16 mutation: variable field (grow)", "[fracpack16][mutation]")
{
   M16Simple orig{1, "hi", 2};
   auto      doc = ref16_rw<M16Simple>(pack16(orig));

   doc.fields().name() = std::string("hello world");
   auto out            = doc.unpack();
   REQUIRE(out.id == 1);
   REQUIRE(out.name == "hello world");
   REQUIRE(out.count == 2);
   REQUIRE(doc.validate() != psio::validation_t::invalid);
}

TEST_CASE("frac16 mutation: variable field (shrink)", "[fracpack16][mutation]")
{
   M16Simple orig{1, "hello world", 2};
   auto      doc = ref16_rw<M16Simple>(pack16(orig));

   doc.fields().name() = std::string("hi");
   auto out            = doc.unpack();
   REQUIRE(out.id == 1);
   REQUIRE(out.name == "hi");
   REQUIRE(out.count == 2);
   REQUIRE(doc.validate() != psio::validation_t::invalid);
}

// ── Multiple variable fields (sibling offset patching) ───────────────────

TEST_CASE("frac16 mutation: multiple variable fields — patch siblings",
          "[fracpack16][mutation]")
{
   M16MultiVar orig{100, "aaa", "bbb", "ccc"};
   auto        doc = ref16_rw<M16MultiVar>(pack16(orig));

   SECTION("grow the first variable field — second & third must re-anchor")
   {
      doc.fields().first() = std::string("FIRST_MUCH_LONGER_STRING");
      REQUIRE(std::string(doc.fields().first()) == "FIRST_MUCH_LONGER_STRING");
      REQUIRE(std::string(doc.fields().second()) == "bbb");
      REQUIRE(std::string(doc.fields().third()) == "ccc");
      REQUIRE(doc.validate() != psio::validation_t::invalid);
   }

   SECTION("shrink the middle — third must re-anchor")
   {
      doc.fields().second() = std::string("X");
      REQUIRE(std::string(doc.fields().first()) == "aaa");
      REQUIRE(std::string(doc.fields().second()) == "X");
      REQUIRE(std::string(doc.fields().third()) == "ccc");
      REQUIRE(doc.validate() != psio::validation_t::invalid);
   }

   SECTION("mutate all three in sequence")
   {
      doc.fields().first()  = std::string("X");
      doc.fields().second() = std::string("YYYYYY");
      doc.fields().third()  = std::string("ZZ");
      auto out              = doc.unpack();
      REQUIRE(out.id == 100);
      REQUIRE(out.first == "X");
      REQUIRE(out.second == "YYYYYY");
      REQUIRE(out.third == "ZZ");
      REQUIRE(doc.validate() != psio::validation_t::invalid);
   }
}

// ── Nested struct mutation (multi-level patching) ────────────────────────

TEST_CASE("frac16 mutation: nested struct field", "[fracpack16][mutation]")
{
   M16Person orig{1, {"123 Main", "Springfield"}, "note"};
   auto      doc = ref16_rw<M16Person>(pack16(orig));

   SECTION("grow inner field — outer must re-anchor trailing note")
   {
      doc.fields().addr().street() = std::string("999 Very Long Avenue Boulevard");
      auto out                     = doc.unpack();
      REQUIRE(out.id == 1);
      REQUIRE(out.addr.street == "999 Very Long Avenue Boulevard");
      REQUIRE(out.addr.city == "Springfield");
      REQUIRE(out.note == "note");
      REQUIRE(doc.validate() != psio::validation_t::invalid);
   }

   SECTION("shrink inner city")
   {
      doc.fields().addr().city() = std::string("X");
      auto out                   = doc.unpack();
      REQUIRE(out.addr.street == "123 Main");
      REQUIRE(out.addr.city == "X");
      REQUIRE(out.note == "note");
      REQUIRE(doc.validate() != psio::validation_t::invalid);
   }

   SECTION("mutate outer + inner sequentially")
   {
      doc.fields().id()            = std::uint32_t{42};
      doc.fields().addr().street() = std::string("new street name");
      doc.fields().note()          = std::string("new note that is much longer");
      auto out                     = doc.unpack();
      REQUIRE(out.id == 42);
      REQUIRE(out.addr.street == "new street name");
      REQUIRE(out.addr.city == "Springfield");
      REQUIRE(out.note == "new note that is much longer");
      REQUIRE(doc.validate() != psio::validation_t::invalid);
   }
}

// ── AllFixed / definitionWillNotChange struct ────────────────────────────

TEST_CASE("frac16 mutation: all-fixed (definitionWillNotChange) struct",
          "[fracpack16][mutation]")
{
   M16AllFixed orig{10, 20, 30};
   auto        doc = ref16_rw<M16AllFixed>(pack16(orig));

   doc.fields().y() = std::uint32_t{99};
   REQUIRE(std::uint32_t(doc.fields().x()) == 10);
   REQUIRE(std::uint32_t(doc.fields().y()) == 99);
   REQUIRE(std::uint32_t(doc.fields().z()) == 30);

   auto out = doc.unpack();
   REQUIRE(out.x == 10);
   REQUIRE(out.y == 99);
   REQUIRE(out.z == 30);
}

// ── SIMD patch mask compile-time correctness at frac16 ───────────────────

TEST_CASE("frac16 mutation: patch_mask correctness", "[fracpack16][mutation]")
{
   SECTION("MultiVar at frac16 — mutate first, patch second+third")
   {
      using mask = psio::frac_detail::patch_mask<M16MultiVar, 1, psio::frac_format_16>;
      STATIC_REQUIRE(mask::mask[0] == 0);  // id (fixed)
      STATIC_REQUIRE(mask::mask[1] == 0);  // first (mutated)
      STATIC_REQUIRE(mask::mask[2] == 1);  // second (after, variable)
      STATIC_REQUIRE(mask::mask[3] == 1);  // third (after, variable)
   }

   SECTION("MultiVar at frac16 — mutate last, no successor patches")
   {
      using mask = psio::frac_detail::patch_mask<M16MultiVar, 3, psio::frac_format_16>;
      STATIC_REQUIRE(mask::mask[0] == 0);
      STATIC_REQUIRE(mask::mask[1] == 0);
      STATIC_REQUIRE(mask::mask[2] == 0);
      STATIC_REQUIRE(mask::mask[3] == 0);
   }
}

// ── Layout metadata sanity at frac16 ─────────────────────────────────────

TEST_CASE("frac16 mutation: layout simd_eligible under frac_format_16",
          "[fracpack16][mutation][layout]")
{
   // At frac16, a struct with all u16-wide slots is simd_eligible.
   using Lsimple16  = psio::frac_layout<M16Simple, psio::frac_format_16>;
   // id is u32 (4B), name slot is u16 (2B), count is u32 (4B).
   // Slots are mixed widths → not eligible under frac16.
   STATIC_REQUIRE(Lsimple16::simd_eligible == false);

   using Lmulti16 = psio::frac_layout<M16MultiVar, psio::frac_format_16>;
   // id is u32 (4B), three string slots are u16 (2B) — mixed → not eligible.
   STATIC_REQUIRE(Lmulti16::simd_eligible == false);

   using Lfixed16 = psio::frac_layout<M16AllFixed, psio::frac_format_16>;
   // All u32 (4B), format size_bytes=2 → not eligible under frac16
   // (eligibility requires fixed_size == Format::size_bytes for every member).
   STATIC_REQUIRE(Lfixed16::simd_eligible == false);
}

// ── Zero-copy str_view / data_span at frac16 ─────────────────────────────

TEST_CASE("frac16 mutation: zero-copy str_view after mutation",
          "[fracpack16][mutation][view]")
{
   M16Simple orig{1, "hello", 2};
   auto      doc = ref16_rw<M16Simple>(pack16(orig));

   doc.fields().name() = std::string("worldwide");
   // Read back via str_view (format-aware)
   std::string_view sv = doc.fields().name().str_view();
   REQUIRE(sv == "worldwide");
}

