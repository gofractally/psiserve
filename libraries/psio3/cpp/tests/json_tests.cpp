// Phase 9 — JSON format.
//
// Exercises the v3 architecture against a text format. Scope matches the
// other format tests: primitives, strings, vectors, arrays, optionals,
// reflected records.

#include <psio3/json.hpp>
#include <psio3/reflect.hpp>

#include <catch.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

struct JsonPoint
{
   std::int32_t x;
   std::int32_t y;
};
PSIO3_REFLECT(JsonPoint, x, y)

struct JsonPerson
{
   std::string  name;
   std::int32_t age;
};
PSIO3_REFLECT(JsonPerson, name, age)

struct JsonNested
{
   JsonPerson  owner;
   std::string title;
};
PSIO3_REFLECT(JsonNested, owner, title)

TEST_CASE("json encodes bool", "[json][primitive]")
{
   REQUIRE(psio3::encode(psio3::json{}, true) == "true");
   REQUIRE(psio3::encode(psio3::json{}, false) == "false");
}

TEST_CASE("json round-trips bool", "[json][primitive]")
{
   auto s = psio3::encode(psio3::json{}, true);
   auto v = psio3::decode<bool>(psio3::json{}, std::span<const char>{s});
   REQUIRE(v == true);
}

TEST_CASE("json round-trips integers", "[json][primitive]")
{
   auto s = psio3::encode(psio3::json{}, std::int32_t{-42});
   REQUIRE(s == "-42");
   auto v =
      psio3::decode<std::int32_t>(psio3::json{}, std::span<const char>{s});
   REQUIRE(v == -42);
}

TEST_CASE("json round-trips large unsigned integer", "[json][primitive]")
{
   std::uint64_t big = 0xDEADBEEFCAFEBABE;
   auto          s   = psio3::encode(psio3::json{}, big);
   auto          v   = psio3::decode<std::uint64_t>(
      psio3::json{}, std::span<const char>{s});
   REQUIRE(v == big);
}

TEST_CASE("json escapes string quotes and newlines", "[json][string]")
{
   std::string in = "a\"b\nc";
   auto        s  = psio3::encode(psio3::json{}, in);
   REQUIRE(s == R"("a\"b\nc")");

   auto back =
      psio3::decode<std::string>(psio3::json{}, std::span<const char>{s});
   REQUIRE(back == in);
}

TEST_CASE("json round-trips std::vector<int>", "[json][vector]")
{
   std::vector<std::int32_t> v{1, 2, 3};
   auto                      s = psio3::encode(psio3::json{}, v);
   REQUIRE(s == "[1,2,3]");

   auto back = psio3::decode<std::vector<std::int32_t>>(
      psio3::json{}, std::span<const char>{s});
   REQUIRE(back == v);
}

TEST_CASE("json round-trips empty std::vector", "[json][vector]")
{
   std::vector<std::int32_t> v;
   auto                      s = psio3::encode(psio3::json{}, v);
   REQUIRE(s == "[]");
   auto back = psio3::decode<std::vector<std::int32_t>>(
      psio3::json{}, std::span<const char>{s});
   REQUIRE(back.empty());
}

TEST_CASE("json round-trips std::array", "[json][array]")
{
   std::array<std::uint32_t, 3> arr{{10, 20, 30}};
   auto                         s = psio3::encode(psio3::json{}, arr);
   REQUIRE(s == "[10,20,30]");
   auto back = psio3::decode<std::array<std::uint32_t, 3>>(
      psio3::json{}, std::span<const char>{s});
   REQUIRE(back == arr);
}

TEST_CASE("json round-trips std::optional<T>", "[json][optional]")
{
   std::optional<std::int32_t> some = 7;
   auto                        s    = psio3::encode(psio3::json{}, some);
   REQUIRE(s == "7");
   auto back = psio3::decode<std::optional<std::int32_t>>(
      psio3::json{}, std::span<const char>{s});
   REQUIRE(back.has_value());
   REQUIRE(*back == 7);

   std::optional<std::int32_t> none;
   auto                        s2 = psio3::encode(psio3::json{}, none);
   REQUIRE(s2 == "null");
   auto back2 = psio3::decode<std::optional<std::int32_t>>(
      psio3::json{}, std::span<const char>{s2});
   REQUIRE(!back2.has_value());
}

TEST_CASE("json round-trips reflected records", "[json][record]")
{
   JsonPoint p{3, -5};
   auto      s = psio3::encode(psio3::json{}, p);
   REQUIRE(s == R"({"x":3,"y":-5})");
   auto back =
      psio3::decode<JsonPoint>(psio3::json{}, std::span<const char>{s});
   REQUIRE(back.x == 3);
   REQUIRE(back.y == -5);
}

TEST_CASE("json round-trips records with string fields", "[json][record]")
{
   JsonPerson pp{"Alice", 30};
   auto       s = psio3::encode(psio3::json{}, pp);
   REQUIRE(s == R"({"name":"Alice","age":30})");
   auto back =
      psio3::decode<JsonPerson>(psio3::json{}, std::span<const char>{s});
   REQUIRE(back.name == "Alice");
   REQUIRE(back.age == 30);
}

TEST_CASE("json decoder tolerates whitespace and key reordering",
          "[json][record]")
{
   std::string text = R"(  { "age" : 30 , "name" : "Bob" } )";
   auto        back =
      psio3::decode<JsonPerson>(psio3::json{}, std::span<const char>{text});
   REQUIRE(back.name == "Bob");
   REQUIRE(back.age == 30);
}

TEST_CASE("json round-trips nested records", "[json][record][nested]")
{
   JsonNested n{{"Carol", 25}, "Dr"};
   auto       s = psio3::encode(psio3::json{}, n);
   REQUIRE(s == R"({"owner":{"name":"Carol","age":25},"title":"Dr"})");
   auto back =
      psio3::decode<JsonNested>(psio3::json{}, std::span<const char>{s});
   REQUIRE(back.owner.name == "Carol");
   REQUIRE(back.owner.age == 25);
   REQUIRE(back.title == "Dr");
}

TEST_CASE("json validate accepts well-formed input", "[json][validate]")
{
   std::string s  = R"({"x":1,"y":2})";
   auto        st = psio3::validate<JsonPoint>(psio3::json{},
                                        std::span<const char>{s});
   REQUIRE(st.ok());
}

TEST_CASE("json validate rejects empty input", "[json][validate]")
{
   char tiny = '\0';
   auto st = psio3::validate<JsonPoint>(psio3::json{},
                                         std::span<const char>{&tiny, 0});
   REQUIRE(!st.ok());
}

TEST_CASE("json scoped sugar matches generic CPO", "[json][format_tag_base]")
{
   JsonPoint p{1, 2};
   auto      a = psio3::encode(psio3::json{}, p);
   auto      b = psio3::json::encode(p);
   REQUIRE(a == b);

   auto v = psio3::json::decode<JsonPoint>(std::span<const char>{a});
   REQUIRE(v.x == 1);
   REQUIRE(v.y == 2);
}

TEST_CASE("json encodes std::variant as [index, value]", "[json][variant]")
{
   using V = std::variant<std::int32_t, std::string>;
   REQUIRE(psio3::encode(psio3::json{}, V{std::int32_t{42}}) ==
           "[0,42]");
   REQUIRE(psio3::encode(psio3::json{}, V{std::string("hi")}) ==
           "[1,\"hi\"]");
}

TEST_CASE("json round-trips std::variant", "[json][variant][round-trip]")
{
   using V = std::variant<std::int32_t, std::string>;
   V     in = std::string("payload");
   auto  bv = psio3::encode(psio3::json{}, in);
   auto  out =
      psio3::decode<V>(psio3::json{}, std::span<const char>{bv});
   REQUIRE(out.index() == 1);
   REQUIRE(std::get<1>(out) == "payload");
}
