#include <catch2/catch.hpp>
#include <psio/wit_encode.hpp>
#include <psio/wit_types.hpp>

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

// ── Helpers ─────────────────────────────────────────────────────────────────

using namespace psio;

static constexpr int32_t P_BOOL   = wit_prim_idx(wit_prim::bool_);
static constexpr int32_t P_U8     = wit_prim_idx(wit_prim::u8);
static constexpr int32_t P_S8     = wit_prim_idx(wit_prim::s8);
static constexpr int32_t P_U16    = wit_prim_idx(wit_prim::u16);
static constexpr int32_t P_S16    = wit_prim_idx(wit_prim::s16);
static constexpr int32_t P_U32    = wit_prim_idx(wit_prim::u32);
static constexpr int32_t P_S32    = wit_prim_idx(wit_prim::s32);
static constexpr int32_t P_U64    = wit_prim_idx(wit_prim::u64);
static constexpr int32_t P_S64    = wit_prim_idx(wit_prim::s64);
static constexpr int32_t P_F32    = wit_prim_idx(wit_prim::f32);
static constexpr int32_t P_F64    = wit_prim_idx(wit_prim::f64);
static constexpr int32_t P_CHAR   = wit_prim_idx(wit_prim::char_);
static constexpr int32_t P_STRING = wit_prim_idx(wit_prim::string_);

static const uint8_t K_RECORD  = static_cast<uint8_t>(wit_type_kind::record_);
static const uint8_t K_VARIANT = static_cast<uint8_t>(wit_type_kind::variant_);
static const uint8_t K_ENUM    = static_cast<uint8_t>(wit_type_kind::enum_);
static const uint8_t K_FLAGS   = static_cast<uint8_t>(wit_type_kind::flags_);
static const uint8_t K_LIST    = static_cast<uint8_t>(wit_type_kind::list_);
static const uint8_t K_OPTION  = static_cast<uint8_t>(wit_type_kind::option_);
static const uint8_t K_RESULT  = static_cast<uint8_t>(wit_type_kind::result_);
static const uint8_t K_TUPLE   = static_cast<uint8_t>(wit_type_kind::tuple_);

static void write_golden(const std::string& name, const std::vector<uint8_t>& binary) {
   auto path = std::string(GOLDEN_DIR) + "/" + name + ".wasm";
   std::ofstream out(path, std::ios::binary);
   REQUIRE(out.good());
   out.write(reinterpret_cast<const char*>(binary.data()),
             static_cast<std::streamsize>(binary.size()));
   INFO("Written " << binary.size() << " bytes to " << path);
}

static void verify_component_header(const std::vector<uint8_t>& binary) {
   REQUIRE(binary.size() >= 8);
   CHECK(binary[0] == 0x00);
   CHECK(binary[1] == 0x61);
   CHECK(binary[2] == 0x73);
   CHECK(binary[3] == 0x6d);
   CHECK(binary[4] == 0x0d);
   CHECK(binary[5] == 0x00);
   CHECK(binary[6] == 0x01);
   CHECK(binary[7] == 0x00);
}

static bool contains(const std::vector<uint8_t>& binary, const std::string& needle) {
   std::string s(binary.begin(), binary.end());
   return s.find(needle) != std::string::npos;
}

// ── 1. All primitives ───────────────────────────────────────────────────────

TEST_CASE("golden — all primitives", "[golden]") {
   wit_world w;
   w.package = "test:primitives@1.0.0";
   w.name    = "primitives-api-world";

   // Type 0: record all-prims with every primitive type
   w.types.push_back({
      "all-prims", K_RECORD,
      {
         {"b",       P_BOOL},
         {"u8-val",  P_U8},
         {"s8-val",  P_S8},
         {"u16-val", P_U16},
         {"s16-val", P_S16},
         {"u32-val", P_U32},
         {"s32-val", P_S32},
         {"u64-val", P_U64},
         {"s64-val", P_S64},
         {"f32-val", P_F32},
         {"f64-val", P_F64},
         {"ch",      P_CHAR},
         {"text",    P_STRING},
      },
      0, 0
   });

   // Functions exercising primitives
   w.funcs.push_back({"identity-bool",   {{"v", P_BOOL}},   {{"", P_BOOL}}});
   w.funcs.push_back({"identity-string", {{"v", P_STRING}}, {{"", P_STRING}}});
   w.funcs.push_back({"sum-ints",   {{"a", P_U32}, {"b", P_S32}, {"c", P_U64}}, {{"", P_S64}}});
   w.funcs.push_back({"sum-floats", {{"a", P_F32}, {"b", P_F64}}, {{"", P_F64}}});
   w.funcs.push_back({"get-all",    {},                    {{"", 0}}});  // returns record

   wit_interface iface;
   iface.name = "primitives-api";
   iface.type_idxs = {0};
   iface.func_idxs = {0, 1, 2, 3, 4};
   w.exports.push_back(std::move(iface));

   auto binary = encode_wit_binary(w);
   verify_component_header(binary);
   CHECK(contains(binary, "all-prims"));
   CHECK(contains(binary, "identity-bool"));
   CHECK(contains(binary, "sum-ints"));
   write_golden("all_primitives", binary);
}

// ── 2. Variants ─────────────────────────────────────────────────────────────

TEST_CASE("golden — variants", "[golden]") {
   wit_world w;
   w.package = "test:variants@1.0.0";
   w.name    = "variants-api-world";

   // Type 0: record dims
   w.types.push_back({
      "dims", K_RECORD,
      {{"width", P_F64}, {"height", P_F64}},
      0, 0
   });

   // Type 1: variant shape { circle(f64), rectangle(dims), triangle }
   // Variant cases: name + type_idx (0 for no payload? Actually let's use a sentinel)
   // In WIT, a variant case with no payload has no type. In our encoding,
   // we need to handle this. Looking at the C++ code, variant fields have type_idx.
   // For no-payload cases, we'll use a special marker. Let's check how wasm-tools handles it.
   // Actually, in the Component Model binary, variant cases are: string name + optional valtype
   // Our current encoder emits name + valtype for every case. We need to handle the
   // "no payload" case. For now, let's use cases that all have payloads.
   w.types.push_back({
      "shape", K_VARIANT,
      {
         {"circle",    P_F64},
         {"rectangle", 0},      // dims record
         {"point",     P_U32},
      },
      0, 0
   });

   // Type 2: variant result-like { ok(u64), err(string) }
   w.types.push_back({
      "outcome", K_VARIANT,
      {
         {"success", P_U64},
         {"failure", P_STRING},
      },
      0, 0
   });

   w.funcs.push_back({"area",     {{"s", 1}}, {{"", P_F64}}});
   w.funcs.push_back({"describe", {{"s", 1}}, {{"", P_STRING}}});
   w.funcs.push_back({"try-op",   {},         {{"", 2}}});

   wit_interface iface;
   iface.name = "variants-api";
   iface.type_idxs = {0, 1, 2};
   iface.func_idxs = {0, 1, 2};
   w.exports.push_back(std::move(iface));

   auto binary = encode_wit_binary(w);
   verify_component_header(binary);
   CHECK(contains(binary, "shape"));
   CHECK(contains(binary, "circle"));
   CHECK(contains(binary, "outcome"));
   write_golden("variants", binary);
}

// ── 3. Enums and flags ──────────────────────────────────────────────────────

TEST_CASE("golden — enums and flags", "[golden]") {
   wit_world w;
   w.package = "test:enums-flags@1.0.0";
   w.name    = "enums-flags-api-world";

   // Type 0: enum color { red, green, blue, alpha }
   w.types.push_back({
      "color", K_ENUM,
      {{"red", 0}, {"green", 0}, {"blue", 0}, {"alpha", 0}},
      0, 0
   });

   // Type 1: flags permissions { read, write, execute, admin }
   w.types.push_back({
      "permissions", K_FLAGS,
      {{"read", 0}, {"write", 0}, {"execute", 0}, {"admin", 0}},
      0, 0
   });

   // Type 2: enum size { small, medium, large }
   w.types.push_back({
      "size", K_ENUM,
      {{"small", 0}, {"medium", 0}, {"large", 0}},
      0, 0
   });

   w.funcs.push_back({"color-name",   {{"c", 0}},              {{"", P_STRING}}});
   w.funcs.push_back({"check-access", {{"p", 1}},              {{"", P_BOOL}}});
   w.funcs.push_back({"combine",      {{"a", 1}, {"b", 1}},    {{"", 1}}});
   w.funcs.push_back({"size-value",   {{"s", 2}},              {{"", P_U32}}});

   wit_interface iface;
   iface.name = "enums-flags-api";
   iface.type_idxs = {0, 1, 2};
   iface.func_idxs = {0, 1, 2, 3};
   w.exports.push_back(std::move(iface));

   auto binary = encode_wit_binary(w);
   verify_component_header(binary);
   CHECK(contains(binary, "color"));
   CHECK(contains(binary, "permissions"));
   CHECK(contains(binary, "read"));
   CHECK(contains(binary, "admin"));
   write_golden("enums_flags", binary);
}

// ── 4. Result types ─────────────────────────────────────────────────────────

TEST_CASE("golden — result types", "[golden]") {
   wit_world w;
   w.package = "test:results@1.0.0";
   w.name    = "results-api-world";

   // Type 0: record user
   w.types.push_back({
      "user", K_RECORD,
      {{"id", P_U64}, {"name", P_STRING}},
      0, 0
   });

   // Type 1: result<user, string> — ok=type0, err=string
   w.types.push_back({
      "", K_RESULT, {},
      0,        // element_type_idx = ok type = user(0)
      P_STRING  // error_type_idx = string
   });

   // Type 2: result<u64, string>
   w.types.push_back({
      "", K_RESULT, {},
      P_U64,    // ok type
      P_STRING  // err type
   });

   // Type 3: result<u32, u32>
   w.types.push_back({
      "", K_RESULT, {},
      P_U32, P_U32
   });

   w.funcs.push_back({"find-user",    {{"id", P_U64}},     {{"", 1}}});
   w.funcs.push_back({"parse-number", {{"text", P_STRING}}, {{"", 2}}});
   w.funcs.push_back({"divide",       {{"a", P_U32}, {"b", P_U32}}, {{"", 3}}});

   wit_interface iface;
   iface.name = "results-api";
   iface.type_idxs = {0};
   iface.func_idxs = {0, 1, 2};
   w.exports.push_back(std::move(iface));

   auto binary = encode_wit_binary(w);
   verify_component_header(binary);
   CHECK(contains(binary, "user"));
   CHECK(contains(binary, "find-user"));
   write_golden("result_types", binary);
}

// ── 5. Tuples ───────────────────────────────────────────────────────────────

TEST_CASE("golden — tuples", "[golden]") {
   wit_world w;
   w.package = "test:tuples@1.0.0";
   w.name    = "tuples-api-world";

   // Type 0: tuple<u32, string>
   w.types.push_back({
      "", K_TUPLE,
      {{"", P_U32}, {"", P_STRING}},
      0, 0
   });

   // Type 1: tuple<bool, u32, string>
   w.types.push_back({
      "", K_TUPLE,
      {{"", P_BOOL}, {"", P_U32}, {"", P_STRING}},
      0, 0
   });

   // Type 2: tuple<string, u32> (reverse of type 0)
   w.types.push_back({
      "", K_TUPLE,
      {{"", P_STRING}, {"", P_U32}},
      0, 0
   });

   // Type 3: tuple<f32, f64>
   w.types.push_back({
      "", K_TUPLE,
      {{"", P_F32}, {"", P_F64}},
      0, 0
   });

   w.funcs.push_back({"pair",   {{"a", P_U32}, {"b", P_STRING}}, {{"", 0}}});
   w.funcs.push_back({"triple", {},                                {{"", 1}}});
   w.funcs.push_back({"swap",   {{"t", 0}},                       {{"", 2}}});
   w.funcs.push_back({"coords", {},                                {{"", 3}}});

   wit_interface iface;
   iface.name = "tuples-api";
   iface.type_idxs = {};  // no named types
   iface.func_idxs = {0, 1, 2, 3};
   w.exports.push_back(std::move(iface));

   auto binary = encode_wit_binary(w);
   verify_component_header(binary);
   CHECK(contains(binary, "pair"));
   CHECK(contains(binary, "swap"));
   write_golden("tuples", binary);
}

// ── 6. Nested generics ─────────────────────────────────────────────────────

TEST_CASE("golden — nested generics", "[golden]") {
   wit_world w;
   w.package = "test:nested@1.0.0";
   w.name    = "nested-api-world";

   // Type 0: record point { x: f64, y: f64 }
   w.types.push_back({
      "point", K_RECORD,
      {{"x", P_F64}, {"y", P_F64}},
      0, 0
   });

   // Type 1: list<point> (anonymous)
   w.types.push_back({"", K_LIST, {}, 0, 0});  // element = point(0)

   // Type 2: option<list<point>> (anonymous) — option over type 1
   w.types.push_back({"", K_OPTION, {}, 1, 0});

   // Type 3: list<list<point>> (anonymous) — list of type 1
   w.types.push_back({"", K_LIST, {}, 1, 0});

   // Type 4: option<u32> (anonymous)
   w.types.push_back({"", K_OPTION, {}, P_U32, 0});

   // Type 5: list<option<u32>> (anonymous) — list of type 4
   w.types.push_back({"", K_LIST, {}, 4, 0});

   // Type 6: option<string> (anonymous)
   w.types.push_back({"", K_OPTION, {}, P_STRING, 0});

   // Type 7: list<option<string>> (anonymous) — list of type 6
   w.types.push_back({"", K_LIST, {}, 6, 0});

   // Type 8: option<list<option<string>>> (anonymous) — option over type 7
   w.types.push_back({"", K_OPTION, {}, 7, 0});

   // Type 9: result<list<point>, string>
   w.types.push_back({"", K_RESULT, {}, 1, P_STRING});

   // Type 10: option<option<u32>> — option over type 4
   w.types.push_back({"", K_OPTION, {}, 4, 0});

   w.funcs.push_back({"maybe-points",  {},  {{"", 2}}});   // -> option<list<point>>
   w.funcs.push_back({"point-grid",    {},  {{"", 3}}});   // -> list<list<point>>
   w.funcs.push_back({"tagged-values", {},  {{"", 5}}});   // -> list<option<u32>>
   w.funcs.push_back({"deep",          {},  {{"", 8}}});   // -> option<list<option<string>>>
   w.funcs.push_back({"result-list",   {},  {{"", 9}}});   // -> result<list<point>, string>
   w.funcs.push_back({"maybe-maybe",   {},  {{"", 10}}});  // -> option<option<u32>>

   wit_interface iface;
   iface.name = "nested-api";
   iface.type_idxs = {0};
   iface.func_idxs = {0, 1, 2, 3, 4, 5};
   w.exports.push_back(std::move(iface));

   auto binary = encode_wit_binary(w);
   verify_component_header(binary);
   CHECK(contains(binary, "point"));
   CHECK(contains(binary, "maybe-points"));
   CHECK(contains(binary, "deep"));
   write_golden("nested_generics", binary);
}

// ── 7. Imports and exports ──────────────────────────────────────────────────

TEST_CASE("golden — imports and exports", "[golden]") {
   wit_world w;
   w.package = "test:mixed@1.0.0";
   w.name    = "mixed-world";

   // Type 0: record log-entry { level: u32, message: string }
   w.types.push_back({
      "log-entry", K_RECORD,
      {{"level", P_U32}, {"message", P_STRING}},
      0, 0
   });

   // Type 1: record calc-result { value: s64, overflow: bool }
   w.types.push_back({
      "calc-result", K_RECORD,
      {{"value", P_S64}, {"overflow", P_BOOL}},
      0, 0
   });

   // Import interface: logger
   w.funcs.push_back({"log",       {{"msg", P_STRING}},               {}});  // void
   w.funcs.push_back({"log-level", {{"level", P_U32}, {"msg", P_STRING}}, {}});  // void
   w.funcs.push_back({"last-entry", {},                                {{"", 0}}});  // -> log-entry

   wit_interface imp;
   imp.name = "logger";
   imp.type_idxs = {0};
   imp.func_idxs = {0, 1, 2};
   w.imports.push_back(std::move(imp));

   // Export interface: calculator
   w.funcs.push_back({"add",      {{"a", P_S32}, {"b", P_S32}}, {{"", P_S32}}});
   w.funcs.push_back({"mul",      {{"a", P_S32}, {"b", P_S32}}, {{"", P_S32}}});
   w.funcs.push_back({"safe-add", {{"a", P_S64}, {"b", P_S64}}, {{"", 1}}});

   wit_interface exp;
   exp.name = "calculator";
   exp.type_idxs = {1};
   exp.func_idxs = {3, 4, 5};
   w.exports.push_back(std::move(exp));

   auto binary = encode_wit_binary(w);
   verify_component_header(binary);
   CHECK(contains(binary, "logger"));
   CHECK(contains(binary, "calculator"));
   CHECK(contains(binary, "log-entry"));
   write_golden("imports_exports", binary);
}

// ── 8. Kitchen sink — everything combined ───────────────────────────────────

TEST_CASE("golden — kitchen sink", "[golden]") {
   wit_world w;
   w.package = "test:kitchen-sink@1.0.0";
   w.name    = "kitchen-sink-world";

   // Type 0: enum status { pending, active, closed }
   w.types.push_back({
      "status", K_ENUM,
      {{"pending", 0}, {"active", 0}, {"closed", 0}},
      0, 0
   });

   // Type 1: flags features { auth, logging, metrics, caching }
   w.types.push_back({
      "features", K_FLAGS,
      {{"auth", 0}, {"logging", 0}, {"metrics", 0}, {"caching", 0}},
      0, 0
   });

   // Type 2: record metadata { key: string, value: string }
   w.types.push_back({
      "metadata", K_RECORD,
      {{"key", P_STRING}, {"value", P_STRING}},
      0, 0
   });

   // Type 3: list<metadata> (anonymous)
   w.types.push_back({"", K_LIST, {}, 2, 0});

   // Type 4: list<string> (anonymous)
   w.types.push_back({"", K_LIST, {}, P_STRING, 0});

   // Type 5: option<u32> (anonymous)
   w.types.push_back({"", K_OPTION, {}, P_U32, 0});

   // Type 6: record task
   w.types.push_back({
      "task", K_RECORD,
      {
         {"id",       P_U64},
         {"title",    P_STRING},
         {"status",   0},  // enum status
         {"tags",     4},  // list<string>
         {"meta",     3},  // list<metadata>
         {"priority", 5},  // option<u32>
         {"features", 1},  // flags features
      },
      0, 0
   });

   // Type 7: tuple<u64, status>
   w.types.push_back({
      "", K_TUPLE,
      {{"", P_U64}, {"", 0}},  // u64, status
      0, 0
   });

   // Type 8: variant action
   w.types.push_back({
      "action", K_VARIANT,
      {
         {"create",     6},  // task
         {"update",     6},  // task
         {"delete",     P_U64},
         {"set-status", 7},  // tuple<u64, status>
      },
      0, 0
   });

   // Type 9: result<task, string>
   w.types.push_back({"", K_RESULT, {}, 6, P_STRING});

   // Type 10: list<task> (anonymous)
   w.types.push_back({"", K_LIST, {}, 6, 0});

   // Type 11: option<task> (anonymous)
   w.types.push_back({"", K_OPTION, {}, 6, 0});

   // Type 12: list<action> (anonymous)
   w.types.push_back({"", K_LIST, {}, 8, 0});

   // Type 13: list<result<task, string>> (anonymous)
   w.types.push_back({"", K_LIST, {}, 9, 0});

   // Functions
   w.funcs.push_back({"process",      {{"a", 8}},           {{"", 9}}});   // action -> result<task, string>
   w.funcs.push_back({"list-tasks",   {{"s", 0}},           {{"", 10}}});  // status -> list<task>
   w.funcs.push_back({"get-task",     {{"id", P_U64}},      {{"", 11}}});  // u64 -> option<task>
   w.funcs.push_back({"get-features", {},                    {{"", 1}}});   // -> features
   w.funcs.push_back({"batch",        {{"actions", 12}},     {{"", 13}}}); // list<action> -> list<result<task,string>>

   wit_interface iface;
   iface.name = "kitchen-sink-api";
   iface.type_idxs = {0, 1, 2, 6, 8};  // named types: status, features, metadata, task, action
   iface.func_idxs = {0, 1, 2, 3, 4};
   w.exports.push_back(std::move(iface));

   auto binary = encode_wit_binary(w);
   verify_component_header(binary);
   CHECK(contains(binary, "kitchen-sink"));
   CHECK(contains(binary, "status"));
   CHECK(contains(binary, "features"));
   CHECK(contains(binary, "action"));
   CHECK(contains(binary, "task"));
   CHECK(contains(binary, "process"));
   CHECK(contains(binary, "batch"));
   write_golden("kitchen_sink", binary);
}
