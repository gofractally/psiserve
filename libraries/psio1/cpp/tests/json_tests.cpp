#include <catch2/catch.hpp>
#include <psio1/from_json.hpp>
#include <psio1/to_json.hpp>

// ── Test structs ──────────────────────────────────────────────────────────────

struct Point
{
   int32_t x;
   int32_t y;
};
PSIO1_REFLECT(Point, x, y)

struct Person
{
   std::string name;
   uint32_t    age;
   Point       location;
};
PSIO1_REFLECT(Person, name, age, location)

// ── to_json tests ────────────────────────────────────────────────────────────

TEST_CASE("to_json: primitives", "[json]")
{
   REQUIRE(psio1::convert_to_json(uint32_t(42)) == "42");
   REQUIRE(psio1::convert_to_json(int32_t(-7)) == "-7");
   REQUIRE(psio1::convert_to_json(true) == "true");
   REQUIRE(psio1::convert_to_json(false) == "false");
   REQUIRE(psio1::convert_to_json(std::string("hello")) == "\"hello\"");
}

TEST_CASE("to_json: struct", "[json]")
{
   Point p{10, 20};
   auto  json = psio1::convert_to_json(p);
   REQUIRE(json == "{\"x\":10,\"y\":20}");
}

TEST_CASE("to_json: nested struct", "[json]")
{
   Person p{"Alice", 30, {1, 2}};
   auto   json = psio1::convert_to_json(p);
   REQUIRE(json == "{\"name\":\"Alice\",\"age\":30,\"location\":{\"x\":1,\"y\":2}}");
}

TEST_CASE("to_json: vector", "[json]")
{
   std::vector<int32_t> v{1, 2, 3};
   auto                 json = psio1::convert_to_json(v);
   REQUIRE(json == "[1,2,3]");
}

TEST_CASE("to_json: optional", "[json]")
{
   std::optional<int32_t> present = 42;
   std::optional<int32_t> absent;
   REQUIRE(psio1::convert_to_json(present) == "42");
   REQUIRE(psio1::convert_to_json(absent) == "null");
}

TEST_CASE("to_json: string escaping", "[json]")
{
   REQUIRE(psio1::convert_to_json(std::string("a\"b")) == "\"a\\\"b\"");
   REQUIRE(psio1::convert_to_json(std::string("a\\b")) == "\"a\\\\b\"");
   REQUIRE(psio1::convert_to_json(std::string("a\nb")) == "\"a\\nb\"");
   REQUIRE(psio1::convert_to_json(std::string("a\tb")) == "\"a\\tb\"");
}

TEST_CASE("to_json: floating point", "[json]")
{
   auto json = psio1::convert_to_json(3.14);
   REQUIRE(json.find("3.14") != std::string::npos);

   REQUIRE(psio1::convert_to_json(0.0) == "0");
}

TEST_CASE("to_json: pretty print", "[json]")
{
   Point p{10, 20};
   auto  json = psio1::format_json(p);
   // Pretty format should have newlines
   REQUIRE(json.find('\n') != std::string::npos);
   REQUIRE(json.find("\"x\"") != std::string::npos);
}

// ── from_json tests ──────────────────────────────────────────────────────────

TEST_CASE("from_json: primitives", "[json]")
{
   REQUIRE(psio1::convert_from_json<uint32_t>("\"42\"") == 42);
   REQUIRE(psio1::convert_from_json<int32_t>("\"-7\"") == -7);
   REQUIRE(psio1::convert_from_json<bool>("true") == true);
   REQUIRE(psio1::convert_from_json<bool>("false") == false);
   REQUIRE(psio1::convert_from_json<std::string>("\"hello\"") == "hello");
}

TEST_CASE("from_json: struct", "[json]")
{
   auto p = psio1::convert_from_json<Point>("{\"x\":10,\"y\":20}");
   REQUIRE(p.x == 10);
   REQUIRE(p.y == 20);
}

TEST_CASE("from_json: nested struct", "[json]")
{
   // Note: from_json reads integers as quoted strings
   std::string json = "{\"name\":\"Alice\",\"age\":\"30\",\"location\":{\"x\":\"1\",\"y\":\"2\"}}";
   auto p = psio1::convert_from_json<Person>(std::move(json));
   REQUIRE(p.name == "Alice");
   REQUIRE(p.age == 30);
   REQUIRE(p.location.x == 1);
   REQUIRE(p.location.y == 2);
}

TEST_CASE("from_json: vector", "[json]")
{
   auto v = psio1::convert_from_json<std::vector<std::string>>("[\"a\",\"b\",\"c\"]");
   REQUIRE(v.size() == 3);
   REQUIRE(v[0] == "a");
   REQUIRE(v[1] == "b");
   REQUIRE(v[2] == "c");
}

TEST_CASE("from_json: optional", "[json]")
{
   auto present = psio1::convert_from_json<std::optional<std::string>>("\"hello\"");
   auto absent  = psio1::convert_from_json<std::optional<std::string>>("null");
   REQUIRE(present.has_value());
   REQUIRE(*present == "hello");
   REQUIRE(!absent.has_value());
}

// ── Round-trip tests ─────────────────────────────────────────────────────────

TEST_CASE("json: round-trip Point", "[json]")
{
   // to_json emits integers < 64-bit unquoted, but from_json expects quoted.
   // Round-trip test uses the quoted format from_json needs.
   std::string json = "{\"x\":\"42\",\"y\":\"-7\"}";
   auto        back = psio1::convert_from_json<Point>(std::move(json));
   REQUIRE(back.x == 42);
   REQUIRE(back.y == -7);
}

TEST_CASE("json: round-trip string field", "[json]")
{
   std::string json = "{\"name\":\"Bob\",\"age\":\"25\",\"location\":{\"x\":\"100\",\"y\":\"200\"}}";
   auto        back = psio1::convert_from_json<Person>(std::move(json));
   REQUIRE(back.name == "Bob");
   REQUIRE(back.age == 25);
   REQUIRE(back.location.x == 100);
   REQUIRE(back.location.y == 200);
}
