// Tests for pack_bin's extensibility features:
//   1. Non-DWNC reflected structs get a varuint size prefix.
//   2. Trailing absent std::optional fields are trimmed on the wire.
//   3. Readers tolerate buffer ending when remaining fields are all optional
//      (= trimmed by older schema or trimmed by sender).
//   4. Readers skip unknown trailing bytes within the declared struct size
//      (= extension fields from a newer schema).
//   5. DWNC structs have NO size prefix (compact, fixed-schema).

#include <catch2/catch.hpp>

#include <psio1/from_bin.hpp>
#include <psio1/to_bin.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// ── Schemas ─────────────────────────────────────────────────────────────

struct BFixed
{
   std::uint32_t x;
   std::uint32_t y;
};
PSIO1_REFLECT(BFixed, definitionWillNotChange(), x, y)

struct BExt
{
   std::uint32_t id;
   std::string   name;
};
PSIO1_REFLECT(BExt, id, name)  // extensible (no DWNC)

struct BWithTrailingOpt
{
   std::uint32_t                id;
   std::string                  name;
   std::optional<std::uint64_t> extra;
};
PSIO1_REFLECT(BWithTrailingOpt, id, name, extra)  // extensible + trailing optional

struct BAllOpt
{
   std::optional<std::uint32_t> a;
   std::optional<std::string>   b;
   std::optional<std::uint64_t> c;
};
PSIO1_REFLECT(BAllOpt, a, b, c)

// V1 / V2 of the same logical schema — V2 adds one optional field at the end
struct SchemaV1
{
   std::uint32_t x;
   std::string   y;
};
PSIO1_REFLECT(SchemaV1, x, y)

struct SchemaV2
{
   std::uint32_t                x;
   std::string                  y;
   std::optional<std::uint64_t> z;  // added in V2
};
PSIO1_REFLECT(SchemaV2, x, y, z)

// ── DWNC has no size prefix ────────────────────────────────────────────

TEST_CASE("pack_bin: DWNC struct has no size prefix (compact)", "[pack_bin][dwnc]")
{
   BFixed p{42, 100};
   auto   bytes = psio1::convert_to_bin(p);
   REQUIRE(bytes.size() == 8);  // just two u32s, no header
   BFixed out = psio1::convert_from_bin<BFixed>(bytes);
   REQUIRE(out.x == 42);
   REQUIRE(out.y == 100);
}

// ── Extensible adds varuint size prefix ────────────────────────────────

TEST_CASE("pack_bin: non-DWNC struct carries varuint size prefix",
          "[pack_bin][extensible]")
{
   BExt v{7, "hi"};
   auto bytes = psio1::convert_to_bin(v);
   // Content: u32 id (4) + varuint(2) (1) + "hi" (2) = 7 B
   // Prefix: varuint(7) (1)
   // Total:  8 B
   REQUIRE(bytes.size() == 1 + 4 + 1 + 2);
   BExt out = psio1::convert_from_bin<BExt>(bytes);
   REQUIRE(out.id == 7);
   REQUIRE(out.name == "hi");
}

// ── Trailing absent optional is trimmed ────────────────────────────────

TEST_CASE("pack_bin: trailing nullopt is omitted from wire",
          "[pack_bin][trim]")
{
   BWithTrailingOpt absent{5, "ab", std::nullopt};
   BWithTrailingOpt present{5, "ab", std::uint64_t{9000}};

   auto bytes_absent  = psio1::convert_to_bin(absent);
   auto bytes_present = psio1::convert_to_bin(present);

   // bytes_absent content: u32(4) + varuint(1)+"ab"(2) = 7 B, prefix 1 B → 8 B
   // bytes_present content: + u8 has_value(1) + u64(8) = 16 B, prefix 1 B → 17 B
   REQUIRE(bytes_absent.size() < bytes_present.size());
   REQUIRE(bytes_absent.size() == 1 + 4 + 1 + 2);

   auto out_absent = psio1::convert_from_bin<BWithTrailingOpt>(bytes_absent);
   REQUIRE(out_absent.id == 5);
   REQUIRE(out_absent.name == "ab");
   REQUIRE(!out_absent.extra.has_value());

   auto out_present = psio1::convert_from_bin<BWithTrailingOpt>(bytes_present);
   REQUIRE(out_present.id == 5);
   REQUIRE(out_present.name == "ab");
   REQUIRE(out_present.extra.value() == 9000);
}

// ── All-optional struct; all absent trims to empty content ─────────────

TEST_CASE("pack_bin: all-nullopt struct packs as empty content",
          "[pack_bin][trim]")
{
   BAllOpt v{};  // all std::nullopt
   auto    bytes = psio1::convert_to_bin(v);
   // Prefix = varuint(0) = 1 byte; content = 0 bytes
   REQUIRE(bytes.size() == 1);
   auto out = psio1::convert_from_bin<BAllOpt>(bytes);
   REQUIRE(!out.a.has_value());
   REQUIRE(!out.b.has_value());
   REQUIRE(!out.c.has_value());
}

// ── Forward-compat: old reader tolerates newer schema's extra fields ───

TEST_CASE("pack_bin: reader skips unknown trailing extension bytes",
          "[pack_bin][extensible]")
{
   SchemaV2 v2{7, "hi", std::uint64_t{42}};
   auto     bytes_v2 = psio1::convert_to_bin(v2);

   // Decode with V1 schema (missing `z`): V1 reader should stop after `y`
   // and skip the u8 has_value + u64 value bytes as unknown extension.
   SchemaV1 out_v1 = psio1::convert_from_bin<SchemaV1>(bytes_v2);
   REQUIRE(out_v1.x == 7);
   REQUIRE(out_v1.y == "hi");
}

// ── Back-compat: new reader tolerates older schema's missing trailing opt

TEST_CASE("pack_bin: new schema reading old-schema wire sees nullopt",
          "[pack_bin][extensible]")
{
   SchemaV1 v1{7, "hi"};
   auto     bytes_v1 = psio1::convert_to_bin(v1);

   // Decode with V2 schema (has `z` as trailing optional): V2 reader
   // should see end-of-content before `z` and treat `z` as nullopt.
   SchemaV2 out_v2 = psio1::convert_from_bin<SchemaV2>(bytes_v1);
   REQUIRE(out_v2.x == 7);
   REQUIRE(out_v2.y == "hi");
   REQUIRE(!out_v2.z.has_value());
}
