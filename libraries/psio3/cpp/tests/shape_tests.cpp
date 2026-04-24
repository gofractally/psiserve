// Phase 2 — shape concept tests.

#include <psio3/shapes.hpp>
#include <psio3/wrappers.hpp>

#include <catch.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {

   enum class Color { red, green, blue };

   struct Rec
   {
      int a;
      int b;
   };
   PSIO3_REFLECT(Rec, a, b)

   struct NotRecord
   {
      int x;
   };

}  // namespace

TEST_CASE("Primitive concept matches arithmetic + bool", "[shapes]")
{
   STATIC_REQUIRE((psio3::Primitive<std::uint32_t>));
   STATIC_REQUIRE((psio3::Primitive<std::int64_t>));
   STATIC_REQUIRE((psio3::Primitive<float>));
   STATIC_REQUIRE((psio3::Primitive<double>));
   STATIC_REQUIRE((psio3::Primitive<bool>));

   STATIC_REQUIRE((!psio3::Primitive<Color>));           // enum — separate concept
   STATIC_REQUIRE((!psio3::Primitive<std::string>));     // string — separate
   STATIC_REQUIRE((!psio3::Primitive<std::vector<int>>));
}

TEST_CASE("Enum concept matches std::is_enum", "[shapes]")
{
   STATIC_REQUIRE((psio3::Enum<Color>));
   STATIC_REQUIRE((!psio3::Enum<int>));
   STATIC_REQUIRE((!psio3::Enum<Rec>));
}

TEST_CASE("FixedSequence matches std::array and C arrays", "[shapes]")
{
   using A = std::array<int, 4>;

   STATIC_REQUIRE((psio3::FixedSequence<A>));
   STATIC_REQUIRE((psio3::fixed_size_of_v<A> == 4));
   STATIC_REQUIRE((std::is_same_v<psio3::element_of_t<A>, int>));

   STATIC_REQUIRE((psio3::FixedSequence<int[7]>));
   STATIC_REQUIRE((psio3::fixed_size_of_v<int[7]> == 7));

   STATIC_REQUIRE((!psio3::FixedSequence<std::vector<int>>));
}

TEST_CASE("VariableSequence matches std::vector, not std::array/std::string",
          "[shapes]")
{
   STATIC_REQUIRE((psio3::VariableSequence<std::vector<int>>));
   STATIC_REQUIRE((std::is_same_v<psio3::element_of_t<std::vector<int>>, int>));

   STATIC_REQUIRE((!psio3::VariableSequence<std::array<int, 4>>));
   STATIC_REQUIRE((!psio3::VariableSequence<std::string>));
   STATIC_REQUIRE((!psio3::VariableSequence<int>));
}

TEST_CASE("Optional / Variant concepts", "[shapes]")
{
   STATIC_REQUIRE((psio3::Optional<std::optional<int>>));
   STATIC_REQUIRE((!psio3::Optional<int>));

   STATIC_REQUIRE((psio3::Variant<std::variant<int, double>>));
   STATIC_REQUIRE((!psio3::Variant<int>));
}

TEST_CASE("Bitfield concept matches psio3 bitvector/bitlist", "[shapes]")
{
   STATIC_REQUIRE((psio3::Bitfield<psio3::bitvector<16>>));
   STATIC_REQUIRE((psio3::Bitfield<psio3::bitlist<32>>));
   STATIC_REQUIRE((!psio3::Bitfield<std::vector<bool>>));
}

TEST_CASE("Record matches reflected types", "[shapes]")
{
   STATIC_REQUIRE((psio3::Record<Rec>));
   STATIC_REQUIRE((!psio3::Record<NotRecord>));
}

TEST_CASE("Shape umbrella concept", "[shapes]")
{
   STATIC_REQUIRE((psio3::Shape<int>));
   STATIC_REQUIRE((psio3::Shape<std::string>));
   STATIC_REQUIRE((psio3::Shape<std::array<int, 3>>));
   STATIC_REQUIRE((psio3::Shape<std::vector<int>>));
   STATIC_REQUIRE((psio3::Shape<std::optional<int>>));
   STATIC_REQUIRE((psio3::Shape<std::variant<int, double>>));
   STATIC_REQUIRE((psio3::Shape<Color>));
   STATIC_REQUIRE((psio3::Shape<Rec>));
   STATIC_REQUIRE((!psio3::Shape<NotRecord>));  // no reflection, no shape match
}
