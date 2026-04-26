// WIT Canonical ABI std::variant tests — layout, pack/unpack, view, validate, rebase.

#include <catch2/catch.hpp>
#include <psio1/wit_view.hpp>

#include <cstring>
#include <string>
#include <variant>
#include <vector>

// ── Test types ──────────────────────────────────────────────────────────────

struct WVPoint
{
   int32_t x;
   int32_t y;
};
PSIO1_REFLECT(WVPoint, definitionWillNotChange(), x, y)

inline bool operator==(const WVPoint& a, const WVPoint& b)
{
   return a.x == b.x && a.y == b.y;
}

using SimpleVariant  = std::variant<int32_t, double>;
using MonoVariant    = std::variant<std::monostate, int32_t, std::string>;
using StructVariant  = std::variant<int32_t, WVPoint>;

struct WVWithVariant
{
   uint32_t     id;
   SimpleVariant value;
};
PSIO1_REFLECT(WVWithVariant, definitionWillNotChange(), id, value)

struct WVNested
{
   std::string    label;
   StructVariant  data;
};
PSIO1_REFLECT(WVNested, label, data)

// ============================================================================
//  LAYOUT TESTS — size/align verification
// ============================================================================

TEST_CASE("wit variant: monostate layout", "[wit][variant]")
{
   REQUIRE(psio1::canonical_align_v<std::monostate> == 1);
   REQUIRE(psio1::canonical_size_v<std::monostate> == 0);
}

TEST_CASE("wit variant: simple variant layout (int32_t, double)", "[wit][variant]")
{
   // variant<int32_t, double>: 2 cases -> u8 discriminant
   // disc_size = 1, disc_align = 1
   // max_payload_align = max(align(i32)=4, align(f64)=8) = 8
   // max_payload_size = max(size(i32)=4, size(f64)=8) = 8
   // variant_align = max(1, 8) = 8
   // payload_offset = align_up(1, 8) = 8
   // variant_size = align_up(8 + 8, 8) = 16
   REQUIRE(psio1::canonical_align_v<SimpleVariant> == 8);
   REQUIRE(psio1::canonical_size_v<SimpleVariant> == 16);
}

TEST_CASE("wit variant: monostate variant layout", "[wit][variant]")
{
   // variant<monostate, int32_t, string>: 3 cases -> u8 discriminant
   // disc_size = 1, disc_align = 1
   // align(monostate) = 1, align(i32) = 4, align(string) = 4
   // max_payload_align = 4
   // max_payload_size = max(0, 4, 8) = 8
   // variant_align = max(1, 4) = 4
   // payload_offset = align_up(1, 4) = 4
   // variant_size = align_up(4 + 8, 4) = 12
   REQUIRE(psio1::canonical_align_v<MonoVariant> == 4);
   REQUIRE(psio1::canonical_size_v<MonoVariant> == 12);
}

TEST_CASE("wit variant: struct variant layout", "[wit][variant]")
{
   // variant<int32_t, WVPoint>: 2 cases -> u8 discriminant
   // disc_size = 1
   // align(i32) = 4, align(WVPoint) = align(i32, i32) = 4
   // max_payload_align = 4
   // max_payload_size = max(4, 8) = 8
   // variant_align = max(1, 4) = 4
   // payload_offset = align_up(1, 4) = 4
   // variant_size = align_up(4 + 8, 4) = 12
   REQUIRE(psio1::canonical_align_v<StructVariant> == 4);
   REQUIRE(psio1::canonical_size_v<StructVariant> == 12);
}

TEST_CASE("wit variant: flat count", "[wit][variant]")
{
   // variant flat count = 1 (disc) + max(flat count of alternatives)
   // SimpleVariant: 1 + max(1, 1) = 2
   REQUIRE(psio1::canonical_flat_count_v<SimpleVariant> == 2);

   // MonoVariant: 1 + max(0, 1, 2) = 3  (string = 2 slots: ptr + len)
   REQUIRE(psio1::canonical_flat_count_v<MonoVariant> == 3);
}

// ============================================================================
//  PACK/UNPACK ROUND-TRIP TESTS
// ============================================================================

TEST_CASE("wit variant: simple variant round-trip (int case)", "[wit][variant]")
{
   SimpleVariant orig(int32_t(42));
   auto buf = psio1::wit::pack(orig);
   auto back = psio1::wit::unpack<SimpleVariant>(buf);
   REQUIRE(back.index() == 0);
   REQUIRE(std::get<int32_t>(back) == 42);
}

TEST_CASE("wit variant: simple variant round-trip (double case)", "[wit][variant]")
{
   SimpleVariant orig(3.14);
   auto buf = psio1::wit::pack(orig);
   auto back = psio1::wit::unpack<SimpleVariant>(buf);
   REQUIRE(back.index() == 1);
   REQUIRE(std::get<double>(back) == Approx(3.14));
}

TEST_CASE("wit variant: monostate round-trip (all 3 cases)", "[wit][variant]")
{
   // Case 0: monostate
   {
      MonoVariant orig(std::monostate{});
      auto buf = psio1::wit::pack(orig);
      auto back = psio1::wit::unpack<MonoVariant>(buf);
      REQUIRE(back.index() == 0);
   }

   // Case 1: int32_t
   {
      MonoVariant orig(int32_t(99));
      auto buf = psio1::wit::pack(orig);
      auto back = psio1::wit::unpack<MonoVariant>(buf);
      REQUIRE(back.index() == 1);
      REQUIRE(std::get<int32_t>(back) == 99);
   }

   // Case 2: string
   {
      MonoVariant orig(std::string("hello"));
      auto buf = psio1::wit::pack(orig);
      auto back = psio1::wit::unpack<MonoVariant>(buf);
      REQUIRE(back.index() == 2);
      REQUIRE(std::get<std::string>(back) == "hello");
   }
}

TEST_CASE("wit variant: struct variant round-trip", "[wit][variant]")
{
   StructVariant orig(WVPoint{-5, 10});
   auto buf = psio1::wit::pack(orig);
   auto back = psio1::wit::unpack<StructVariant>(buf);
   REQUIRE(back.index() == 1);
   REQUIRE(std::get<WVPoint>(back) == WVPoint{-5, 10});
}

// ============================================================================
//  VARIANT IN A STRUCT (via PSIO1_REFLECT) — pack/unpack
// ============================================================================

TEST_CASE("wit variant: struct with variant field round-trip", "[wit][variant]")
{
   WVWithVariant orig{7, SimpleVariant(int32_t(100))};
   auto buf = psio1::wit::pack(orig);
   auto back = psio1::wit::unpack<WVWithVariant>(buf);
   REQUIRE(back.id == 7);
   REQUIRE(back.value.index() == 0);
   REQUIRE(std::get<int32_t>(back.value) == 100);
}

TEST_CASE("wit variant: nested struct with variant field", "[wit][variant]")
{
   WVNested orig{"test", StructVariant(WVPoint{3, 4})};
   auto buf = psio1::wit::pack(orig);
   auto back = psio1::wit::unpack<WVNested>(buf);
   REQUIRE(back.label == "test");
   REQUIRE(back.data.index() == 1);
   REQUIRE(std::get<WVPoint>(back.data) == WVPoint{3, 4});
}

// ============================================================================
//  WIRE FORMAT VERIFICATION — check raw bytes
// ============================================================================

TEST_CASE("wit variant: wire format matches layout rules", "[wit][variant]")
{
   // variant<int32_t, double> holding int32_t(0x12345678)
   // Layout: 16 bytes total, align 8
   //   offset 0: disc = 0 (u8)
   //   offset 1-7: padding
   //   offset 8-11: int32_t 0x12345678 LE
   //   offset 12-15: padding
   SimpleVariant v(int32_t(0x12345678));
   auto buf = psio1::wit::pack(v);
   REQUIRE(buf.size() == 16);
   REQUIRE(buf[0] == 0);  // discriminant = 0

   // Payload at offset 8
   int32_t payload;
   std::memcpy(&payload, buf.data() + 8, 4);
   REQUIRE(payload == 0x12345678);

   // Now check double case
   SimpleVariant v2(2.5);
   auto buf2 = psio1::wit::pack(v2);
   REQUIRE(buf2.size() == 16);
   REQUIRE(buf2[0] == 1);  // discriminant = 1

   // Payload at offset 8
   double dpayload;
   std::memcpy(&dpayload, buf2.data() + 8, 8);
   REQUIRE(dpayload == 2.5);
}

TEST_CASE("wit variant: monostate wire format", "[wit][variant]")
{
   // variant<monostate, int32_t, string> holding monostate
   // Layout: 12 bytes total, align 4
   //   offset 0: disc = 0 (u8)
   //   offset 1-3: padding
   //   offset 4-11: payload area (unused for monostate)
   MonoVariant v(std::monostate{});
   auto buf = psio1::wit::pack(v);
   REQUIRE(buf.size() == 12);
   REQUIRE(buf[0] == 0);  // discriminant = 0
}

// ============================================================================
//  VIEW TESTS — read variant fields through wit::field()
// ============================================================================

TEST_CASE("wit variant: view struct with variant field", "[wit][variant]")
{
   WVWithVariant orig{42, SimpleVariant(int32_t(123))};
   auto buf = psio1::wit::pack(orig);
   auto v = psio1::wit_view<WVWithVariant>::from_buffer(buf.data());

   REQUIRE(v.id() == 42);
   auto val = v.value();
   REQUIRE(val.index() == 0);
   REQUIRE(std::get<int32_t>(val) == 123);
}

TEST_CASE("wit variant: view struct with variant double", "[wit][variant]")
{
   WVWithVariant orig{1, SimpleVariant(9.99)};
   auto buf = psio1::wit::pack(orig);
   auto v = psio1::wit_view<WVWithVariant>::from_buffer(buf.data());

   REQUIRE(v.id() == 1);
   auto val = v.value();
   REQUIRE(val.index() == 1);
   REQUIRE(std::get<double>(val) == Approx(9.99));
}

// ============================================================================
//  VALIDATE TESTS
// ============================================================================

TEST_CASE("wit variant: validate good buffer", "[wit][variant]")
{
   SimpleVariant v(int32_t(42));
   auto buf = psio1::wit::pack(v);
   REQUIRE(psio1::wit::validate<SimpleVariant>(buf));
}

TEST_CASE("wit variant: validate struct with variant", "[wit][variant]")
{
   WVWithVariant orig{1, SimpleVariant(2.0)};
   auto buf = psio1::wit::pack(orig);
   REQUIRE(psio1::wit::validate<WVWithVariant>(buf));
}

TEST_CASE("wit variant: validate truncated buffer fails", "[wit][variant]")
{
   SimpleVariant v(int32_t(42));
   auto buf = psio1::wit::pack(v);
   // Truncate
   buf.resize(4);
   REQUIRE_FALSE(psio1::wit::validate<SimpleVariant>(buf));
}

TEST_CASE("wit variant: validate invalid discriminant fails", "[wit][variant]")
{
   SimpleVariant v(int32_t(42));
   auto buf = psio1::wit::pack(v);
   // Set discriminant to invalid index (2 for a 2-case variant)
   buf[0] = 2;
   REQUIRE_FALSE(psio1::wit::validate<SimpleVariant>(buf));
}

// ============================================================================
//  CANONICALIZE TEST
// ============================================================================

TEST_CASE("wit variant: canonicalize round-trip", "[wit][variant]")
{
   WVNested orig{"data", StructVariant(int32_t(77))};
   auto buf = psio1::wit::pack(orig);
   auto canonical = psio1::wit::canonicalize<WVNested>(buf);
   auto back = psio1::wit::unpack<WVNested>(canonical);
   REQUIRE(back.label == "data");
   REQUIRE(back.data.index() == 0);
   REQUIRE(std::get<int32_t>(back.data) == 77);
}

// ============================================================================
//  VARIANT WITH STRING ALTERNATIVE ROUND-TRIP
// ============================================================================

TEST_CASE("wit variant: variant with string alternative round-trip", "[wit][variant]")
{
   MonoVariant v(std::string("world"));
   auto buf = psio1::wit::pack(v);
   REQUIRE(psio1::wit::validate<MonoVariant>(buf));
   auto back = psio1::wit::unpack<MonoVariant>(buf);
   REQUIRE(back.index() == 2);
   REQUIRE(std::get<std::string>(back) == "world");
}
