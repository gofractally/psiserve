#include <catch2/catch.hpp>
#include <psio/frac_ref.hpp>

// ── Test structs ──────────────────────────────────────────────────────────────

struct Simple
{
   uint32_t    id;
   std::string name;
   uint32_t    count;
};
PSIO_REFLECT(Simple, id, name, count)

struct AllFixed
{
   uint32_t x;
   uint32_t y;
   uint32_t z;
};
PSIO_REFLECT(AllFixed, definitionWillNotChange(), x, y, z)

struct MultiVar
{
   uint32_t    id;
   std::string first;
   std::string second;
   std::string third;
};
PSIO_REFLECT(MultiVar, id, first, second, third)

struct Address
{
   std::string street;
   std::string city;
};
PSIO_REFLECT(Address, street, city)

struct Person
{
   uint32_t    id;
   Address     addr;
   std::string note;
};
PSIO_REFLECT(Person, id, addr, note)

struct Deep
{
   uint32_t x;
   Person   person;
};
PSIO_REFLECT(Deep, x, person)

// ── Helper ────────────────────────────────────────────────────────────────────

template <typename T>
std::vector<char> pack(const T& v)
{
   return psio::to_frac(v);
}

template <typename T>
T roundtrip(const std::vector<char>& buf)
{
   return psio::from_frac<T>(std::span<const char>(buf.data(), buf.size()));
}

// ── frac_layout compile-time tests ────────────────────────────────────────────

TEST_CASE("frac_layout: compile-time metadata", "[frac_ref]")
{
   SECTION("Simple struct layout")
   {
      using L = psio::frac_layout<Simple>;
      STATIC_REQUIRE(L::num_members == 3);
      STATIC_REQUIRE(L::has_header == true);
      STATIC_REQUIRE(L::hdr_size == 2);
      STATIC_REQUIRE(L::fixed_sizes[0] == 4);  // uint32_t
      STATIC_REQUIRE(L::fixed_sizes[1] == 4);  // string offset
      STATIC_REQUIRE(L::fixed_sizes[2] == 4);  // uint32_t
      STATIC_REQUIRE(L::is_variable[0] == false);
      STATIC_REQUIRE(L::is_variable[1] == true);
      STATIC_REQUIRE(L::is_variable[2] == false);
      STATIC_REQUIRE(L::offset_of(0) == 0);
      STATIC_REQUIRE(L::offset_of(1) == 4);
      STATIC_REQUIRE(L::offset_of(2) == 8);
      STATIC_REQUIRE(L::simd_eligible == true);
   }

   SECTION("AllFixed struct layout")
   {
      using L = psio::frac_layout<AllFixed>;
      STATIC_REQUIRE(L::has_header == false);
      STATIC_REQUIRE(L::hdr_size == 0);
      STATIC_REQUIRE(L::simd_eligible == true);
   }

   SECTION("MultiVar struct layout")
   {
      using L = psio::frac_layout<MultiVar>;
      STATIC_REQUIRE(L::num_members == 4);
      STATIC_REQUIRE(L::is_variable[0] == false);
      STATIC_REQUIRE(L::is_variable[1] == true);
      STATIC_REQUIRE(L::is_variable[2] == true);
      STATIC_REQUIRE(L::is_variable[3] == true);
   }
}

// ── Read-only frac_ref (span<const char>) ─────────────────────────────────────

TEST_CASE("frac_ref: read-only view over span<const char>", "[frac_ref]")
{
   Simple orig{42, "hello", 99};
   auto   buf = pack(orig);
   auto   ref = psio::frac_ref<Simple, std::span<const char>>(
       std::span<const char>(buf.data(), buf.size()));

   SECTION("read fixed field")
   {
      uint32_t id = ref.fields().id();
      REQUIRE(id == 42);
   }

   SECTION("read variable field")
   {
      std::string name = ref.fields().name();
      REQUIRE(name == "hello");
   }

   SECTION("read second fixed field")
   {
      uint32_t count = ref.fields().count();
      REQUIRE(count == 99);
   }

   SECTION("validate")
   {
      REQUIRE(ref.validate() != psio::validation_t::invalid);
   }

   SECTION("full unpack round-trip")
   {
      auto unpacked = ref.unpack();
      REQUIRE(unpacked.id == 42);
      REQUIRE(unpacked.name == "hello");
      REQUIRE(unpacked.count == 99);
   }
}

// ── Fixed-field writes (vector<char>) ─────────────────────────────────────────

TEST_CASE("frac_ref: fixed-size field mutation", "[frac_ref]")
{
   Simple orig{10, "test", 20};
   auto   buf = pack(orig);
   auto   doc = psio::frac_ref<Simple, std::vector<char>>(std::move(buf));

   SECTION("write fixed field and verify")
   {
      doc.fields().id() = 42;
      uint32_t id = doc.fields().id();
      REQUIRE(id == 42);

      // Other fields unchanged
      std::string name = doc.fields().name();
      REQUIRE(name == "test");
      uint32_t count = doc.fields().count();
      REQUIRE(count == 20);
   }

   SECTION("write second fixed field")
   {
      doc.fields().count() = 999;
      REQUIRE(uint32_t(doc.fields().count()) == 999);
      REQUIRE(uint32_t(doc.fields().id()) == 10);
   }

   SECTION("validates after fixed mutation")
   {
      doc.fields().id() = 100;
      REQUIRE(doc.validate() != psio::validation_t::invalid);

      // Verify via full deserialization
      auto unpacked = doc.unpack();
      REQUIRE(unpacked.id == 100);
      REQUIRE(unpacked.name == "test");
   }
}

// ── Variable-size field writes ────────────────────────────────────────────────

TEST_CASE("frac_ref: variable-size field mutation (same length)", "[frac_ref]")
{
   Simple orig{1, "hello", 2};
   auto   buf = pack(orig);
   auto   doc = psio::frac_ref<Simple, std::vector<char>>(std::move(buf));

   doc.fields().name() = std::string("world");  // same length as "hello"

   REQUIRE(std::string(doc.fields().name()) == "world");
   REQUIRE(uint32_t(doc.fields().id()) == 1);
   REQUIRE(uint32_t(doc.fields().count()) == 2);
   REQUIRE(doc.validate() != psio::validation_t::invalid);
}

TEST_CASE("frac_ref: variable-size field mutation (grow)", "[frac_ref]")
{
   Simple orig{1, "hi", 2};
   auto   buf = pack(orig);
   auto   doc = psio::frac_ref<Simple, std::vector<char>>(std::move(buf));

   doc.fields().name() = std::string("hello world");  // longer

   REQUIRE(std::string(doc.fields().name()) == "hello world");
   REQUIRE(uint32_t(doc.fields().id()) == 1);
   REQUIRE(uint32_t(doc.fields().count()) == 2);
   REQUIRE(doc.validate() != psio::validation_t::invalid);

   auto unpacked = doc.unpack();
   REQUIRE(unpacked.name == "hello world");
}

TEST_CASE("frac_ref: variable-size field mutation (shrink)", "[frac_ref]")
{
   Simple orig{1, "hello world", 2};
   auto   buf = pack(orig);
   auto   doc = psio::frac_ref<Simple, std::vector<char>>(std::move(buf));

   doc.fields().name() = std::string("hi");  // shorter

   REQUIRE(std::string(doc.fields().name()) == "hi");
   REQUIRE(uint32_t(doc.fields().id()) == 1);
   REQUIRE(uint32_t(doc.fields().count()) == 2);
   REQUIRE(doc.validate() != psio::validation_t::invalid);
}

// ── Multiple variable fields ─────────────────────────────────────────────────

TEST_CASE("frac_ref: multiple variable fields", "[frac_ref]")
{
   MultiVar orig{100, "aaa", "bbb", "ccc"};
   auto     buf = pack(orig);
   auto     doc = psio::frac_ref<MultiVar, std::vector<char>>(std::move(buf));

   SECTION("mutate first variable field")
   {
      doc.fields().first() = std::string("FIRST_LONGER");
      REQUIRE(std::string(doc.fields().first()) == "FIRST_LONGER");
      REQUIRE(std::string(doc.fields().second()) == "bbb");
      REQUIRE(std::string(doc.fields().third()) == "ccc");
      REQUIRE(doc.validate() != psio::validation_t::invalid);
   }

   SECTION("mutate middle variable field")
   {
      doc.fields().second() = std::string("MIDDLE");
      REQUIRE(std::string(doc.fields().first()) == "aaa");
      REQUIRE(std::string(doc.fields().second()) == "MIDDLE");
      REQUIRE(std::string(doc.fields().third()) == "ccc");
      REQUIRE(doc.validate() != psio::validation_t::invalid);
   }

   SECTION("mutate last variable field")
   {
      doc.fields().third() = std::string("LAST_MUCH_LONGER_STRING");
      REQUIRE(std::string(doc.fields().first()) == "aaa");
      REQUIRE(std::string(doc.fields().second()) == "bbb");
      REQUIRE(std::string(doc.fields().third()) == "LAST_MUCH_LONGER_STRING");
      REQUIRE(doc.validate() != psio::validation_t::invalid);
   }

   SECTION("mutate all variable fields sequentially")
   {
      doc.fields().first() = std::string("X");
      doc.fields().second() = std::string("YYYYYY");
      doc.fields().third() = std::string("ZZ");

      auto unpacked = doc.unpack();
      REQUIRE(unpacked.id == 100);
      REQUIRE(unpacked.first == "X");
      REQUIRE(unpacked.second == "YYYYYY");
      REQUIRE(unpacked.third == "ZZ");
      REQUIRE(doc.validate() != psio::validation_t::invalid);
   }
}

// ── Nested struct mutation ────────────────────────────────────────────────────

TEST_CASE("frac_ref: nested struct read", "[frac_ref]")
{
   Person orig{1, {"123 Main St", "Springfield"}, "a note"};
   auto   buf = pack(orig);
   auto   doc = psio::frac_ref<Person, std::vector<char>>(std::move(buf));

   REQUIRE(uint32_t(doc.fields().id()) == 1);
   REQUIRE(std::string(doc.fields().note()) == "a note");

   // Nested reads through proxy chain
   std::string street = doc.fields().addr().street();
   std::string city   = doc.fields().addr().city();
   REQUIRE(street == "123 Main St");
   REQUIRE(city == "Springfield");
}

TEST_CASE("frac_ref: nested struct mutation", "[frac_ref]")
{
   Person orig{1, {"123 Main St", "Springfield"}, "a note"};
   auto   buf = pack(orig);
   auto   doc = psio::frac_ref<Person, std::vector<char>>(std::move(buf));

   SECTION("mutate nested field (same size)")
   {
      doc.fields().addr().street() = std::string("456 Oak Ave.");
      REQUIRE(std::string(doc.fields().addr().street()) == "456 Oak Ave.");
      REQUIRE(std::string(doc.fields().addr().city()) == "Springfield");
      REQUIRE(std::string(doc.fields().note()) == "a note");
      REQUIRE(doc.validate() != psio::validation_t::invalid);
   }

   SECTION("mutate nested field (grow)")
   {
      doc.fields().addr().street() = std::string("999 Very Long Street Name Boulevard");

      auto unpacked = doc.unpack();
      REQUIRE(unpacked.id == 1);
      REQUIRE(unpacked.addr.street == "999 Very Long Street Name Boulevard");
      REQUIRE(unpacked.addr.city == "Springfield");
      REQUIRE(unpacked.note == "a note");
      REQUIRE(doc.validate() != psio::validation_t::invalid);
   }

   SECTION("mutate nested field (shrink)")
   {
      doc.fields().addr().city() = std::string("SF");

      auto unpacked = doc.unpack();
      REQUIRE(unpacked.addr.street == "123 Main St");
      REQUIRE(unpacked.addr.city == "SF");
      REQUIRE(unpacked.note == "a note");
      REQUIRE(doc.validate() != psio::validation_t::invalid);
   }

   SECTION("mutate root and nested sequentially")
   {
      doc.fields().id() = 42;
      doc.fields().addr().street() = std::string("new street");
      doc.fields().note() = std::string("new note that is longer");

      auto unpacked = doc.unpack();
      REQUIRE(unpacked.id == 42);
      REQUIRE(unpacked.addr.street == "new street");
      REQUIRE(unpacked.addr.city == "Springfield");
      REQUIRE(unpacked.note == "new note that is longer");
      REQUIRE(doc.validate() != psio::validation_t::invalid);
   }
}

// ── AllFixed struct ───────────────────────────────────────────────────────────

TEST_CASE("frac_ref: all-fixed struct (definitionWillNotChange)", "[frac_ref]")
{
   AllFixed orig{10, 20, 30};
   auto     buf = pack(orig);
   auto     doc = psio::frac_ref<AllFixed, std::vector<char>>(std::move(buf));

   REQUIRE(uint32_t(doc.fields().x()) == 10);
   REQUIRE(uint32_t(doc.fields().y()) == 20);
   REQUIRE(uint32_t(doc.fields().z()) == 30);

   doc.fields().y() = 99;
   REQUIRE(uint32_t(doc.fields().y()) == 99);
   REQUIRE(uint32_t(doc.fields().x()) == 10);
   REQUIRE(uint32_t(doc.fields().z()) == 30);

   auto unpacked = doc.unpack();
   REQUIRE(unpacked.x == 10);
   REQUIRE(unpacked.y == 99);
   REQUIRE(unpacked.z == 30);
}

// ── SIMD patch mask compile-time tests ────────────────────────────────────────

TEST_CASE("frac_ref: SIMD patch mask correctness", "[frac_ref]")
{
   SECTION("MultiVar: mutate first (index 1), patch second and third")
   {
      using mask = psio::frac_detail::patch_mask<MultiVar, 1>;
      STATIC_REQUIRE(mask::mask[0] == 0);  // id (fixed, before)
      STATIC_REQUIRE(mask::mask[1] == 0);  // first (the mutated field)
      STATIC_REQUIRE(mask::mask[2] == 1);  // second (after, variable)
      STATIC_REQUIRE(mask::mask[3] == 1);  // third (after, variable)
   }

   SECTION("MultiVar: mutate third (index 3), no patches needed")
   {
      using mask = psio::frac_detail::patch_mask<MultiVar, 3>;
      STATIC_REQUIRE(mask::mask[0] == 0);
      STATIC_REQUIRE(mask::mask[1] == 0);
      STATIC_REQUIRE(mask::mask[2] == 0);
      STATIC_REQUIRE(mask::mask[3] == 0);
   }

   SECTION("Simple: mutate name (index 1), no var fields after")
   {
      using mask = psio::frac_detail::patch_mask<Simple, 1>;
      STATIC_REQUIRE(mask::mask[0] == 0);  // id (fixed)
      STATIC_REQUIRE(mask::mask[1] == 0);  // name (mutated)
      STATIC_REQUIRE(mask::mask[2] == 0);  // count (fixed)
   }
}

// ── Compile-time capability checks ───────────────────────────────────────────

TEST_CASE("frac_ref: capability tier verification", "[frac_ref]")
{
   // These are compile-time checks — the test just verifies the traits
   using ro_traits = psio::buf_traits<std::span<const char>>;
   STATIC_REQUIRE(ro_traits::is_const == true);
   STATIC_REQUIRE(ro_traits::can_write_fixed == false);
   STATIC_REQUIRE(ro_traits::can_splice == false);

   using rw_traits = psio::buf_traits<std::span<char>>;
   STATIC_REQUIRE(rw_traits::is_const == false);
   STATIC_REQUIRE(rw_traits::can_write_fixed == true);
   STATIC_REQUIRE(rw_traits::can_splice == false);

   using vec_traits = psio::buf_traits<std::vector<char>>;
   STATIC_REQUIRE(vec_traits::is_const == false);
   STATIC_REQUIRE(vec_traits::can_write_fixed == true);
   STATIC_REQUIRE(vec_traits::can_splice == true);
}
