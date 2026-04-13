// Tests for WIT type structs, parser, C++ → WIT generation, and canonical ABI.

#include <psizam/wit_types.hpp>
#include <psizam/wit_parser.hpp>
#include <psizam/wit_gen.hpp>
#include <psizam/canonical_abi.hpp>
#include <psizam/pzam_format.hpp>
#include <catch2/catch.hpp>

using namespace psizam;

// ── Phase 1: wit_types.hpp — fracpack round-trip ─────────────────────────────

TEST_CASE("wit_prim_idx round-trip", "[wit]") {
   for (uint8_t i = 0; i <= static_cast<uint8_t>(wit_prim::string_); i++) {
      auto p = static_cast<wit_prim>(i);
      int32_t idx = wit_prim_idx(p);
      CHECK(idx < 0);
      CHECK(is_prim_idx(idx));
      CHECK(idx_to_prim(idx) == p);
   }
   CHECK(!is_prim_idx(0));
   CHECK(!is_prim_idx(5));
}

TEST_CASE("pzam_wit_world fracpack round-trip", "[wit]") {
   pzam_wit_world world;
   world.name = "test:pkg@1.0.0";
   world.wit_source = "world test { export greet: func(name: string) -> string; }";

   // Add a record type
   wit_type_def rec;
   rec.name = "person";
   rec.kind = static_cast<uint8_t>(wit_type_kind::record_);
   rec.fields = {
      {"name", wit_prim_idx(wit_prim::string_)},
      {"age",  wit_prim_idx(wit_prim::u32)},
   };
   world.types.push_back(rec);

   // Add a variant type
   wit_type_def var;
   var.name = "status";
   var.kind = static_cast<uint8_t>(wit_type_kind::variant_);
   var.fields = {
      {"ok",    0},  // refers to person record
      {"error", wit_prim_idx(wit_prim::string_)},
   };
   world.types.push_back(var);

   // Add an enum
   wit_type_def en;
   en.name = "color";
   en.kind = static_cast<uint8_t>(wit_type_kind::enum_);
   en.fields = {{"red", 0}, {"green", 0}, {"blue", 0}};
   world.types.push_back(en);

   // Add a flags type
   wit_type_def fl;
   fl.name = "perms";
   fl.kind = static_cast<uint8_t>(wit_type_kind::flags_);
   fl.fields = {{"read", 0}, {"write", 0}, {"exec", 0}};
   world.types.push_back(fl);

   // Add a list type
   wit_type_def lst;
   lst.name = "";
   lst.kind = static_cast<uint8_t>(wit_type_kind::list_);
   lst.element_type_idx = wit_prim_idx(wit_prim::u8);
   world.types.push_back(lst);

   // Add a result type
   wit_type_def res;
   res.name = "";
   res.kind = static_cast<uint8_t>(wit_type_kind::result_);
   res.element_type_idx = 0;  // ok: person
   res.error_type_idx = wit_prim_idx(wit_prim::string_);
   world.types.push_back(res);

   // Add a function
   wit_func func;
   func.name = "greet";
   func.params = {{"name", wit_prim_idx(wit_prim::string_)}};
   func.results = {{"", wit_prim_idx(wit_prim::string_)}};
   func.core_func_idx = 42;
   world.funcs.push_back(func);

   // Add interfaces
   wit_interface exp_iface;
   exp_iface.name = "greeter";
   exp_iface.type_idxs = {0, 1};
   exp_iface.func_idxs = {0};
   world.exports.push_back(exp_iface);

   wit_interface imp_iface;
   imp_iface.name = "logger";
   imp_iface.func_idxs = {};
   world.imports.push_back(imp_iface);

   // Round-trip via fracpack
   auto packed = psio::convert_to_frac(world);
   auto unpacked = psio::from_frac<pzam_wit_world>(packed);

   CHECK(unpacked.name == world.name);
   CHECK(unpacked.wit_source == world.wit_source);
   REQUIRE(unpacked.types.size() == world.types.size());
   REQUIRE(unpacked.funcs.size() == world.funcs.size());
   REQUIRE(unpacked.exports.size() == world.exports.size());
   REQUIRE(unpacked.imports.size() == world.imports.size());

   // Verify record
   CHECK(unpacked.types[0].name == "person");
   CHECK(unpacked.types[0].kind == static_cast<uint8_t>(wit_type_kind::record_));
   REQUIRE(unpacked.types[0].fields.size() == 2);
   CHECK(unpacked.types[0].fields[0].name == "name");
   CHECK(unpacked.types[0].fields[0].type_idx == wit_prim_idx(wit_prim::string_));
   CHECK(unpacked.types[0].fields[1].name == "age");
   CHECK(unpacked.types[0].fields[1].type_idx == wit_prim_idx(wit_prim::u32));

   // Verify variant
   CHECK(unpacked.types[1].name == "status");
   CHECK(unpacked.types[1].kind == static_cast<uint8_t>(wit_type_kind::variant_));
   REQUIRE(unpacked.types[1].fields.size() == 2);
   CHECK(unpacked.types[1].fields[0].type_idx == 0);

   // Verify enum
   CHECK(unpacked.types[2].kind == static_cast<uint8_t>(wit_type_kind::enum_));
   CHECK(unpacked.types[2].fields.size() == 3);

   // Verify flags
   CHECK(unpacked.types[3].kind == static_cast<uint8_t>(wit_type_kind::flags_));

   // Verify list
   CHECK(unpacked.types[4].kind == static_cast<uint8_t>(wit_type_kind::list_));
   CHECK(unpacked.types[4].element_type_idx == wit_prim_idx(wit_prim::u8));

   // Verify result
   CHECK(unpacked.types[5].kind == static_cast<uint8_t>(wit_type_kind::result_));
   CHECK(unpacked.types[5].element_type_idx == 0);
   CHECK(unpacked.types[5].error_type_idx == wit_prim_idx(wit_prim::string_));

   // Verify function
   CHECK(unpacked.funcs[0].name == "greet");
   CHECK(unpacked.funcs[0].params[0].name == "name");
   CHECK(unpacked.funcs[0].core_func_idx == 42);

   // Verify interfaces
   CHECK(unpacked.exports[0].name == "greeter");
   CHECK(unpacked.exports[0].type_idxs == std::vector<uint32_t>{0, 1});
   CHECK(unpacked.exports[0].func_idxs == std::vector<uint32_t>{0});
   CHECK(unpacked.imports[0].name == "logger");
}

TEST_CASE("pzam_file with wit metadata fracpack round-trip", "[wit]") {
   pzam_file file;
   file.metadata.wit = pzam_wit_world{};
   file.metadata.wit->name = "test-world";
   file.metadata.wit->wit_source = "world test {}";

   auto packed = pzam_save(file);
   auto loaded = pzam_load(packed);

   REQUIRE(loaded.metadata.wit.has_value());
   CHECK(loaded.metadata.wit->name == "test-world");
   CHECK(loaded.metadata.wit->wit_source == "world test {}");
}

TEST_CASE("pzam_file without wit metadata fracpack round-trip", "[wit]") {
   pzam_file file;
   // No WIT set — wit should be std::nullopt

   auto packed = pzam_save(file);
   auto loaded = pzam_load(packed);

   CHECK(!loaded.metadata.wit.has_value());
}

// ── Phase 2: wit_parser.hpp — parsing ────────────────────────────────────────

TEST_CASE("wit_parse: empty world", "[wit][parser]") {
   auto world = wit_parse("world empty {}");
   CHECK(world.name == "empty");
   CHECK(world.types.empty());
   CHECK(world.funcs.empty());
   CHECK(world.exports.empty());
   CHECK(world.imports.empty());
}

TEST_CASE("wit_parse: package declaration", "[wit][parser]") {
   auto world = wit_parse(
      "package my:pkg@1.0.0;\n"
      "world test {}\n"
   );
   CHECK(world.name == "my:pkg@1.0.0");
}

TEST_CASE("wit_parse: export functions", "[wit][parser]") {
   auto world = wit_parse(
      "world greeter {\n"
      "  export greet: func(name: string) -> string;\n"
      "  export add: func(a: u32, b: u32) -> u32;\n"
      "}\n"
   );

   CHECK(world.name == "greeter");
   REQUIRE(world.funcs.size() == 2);

   CHECK(world.funcs[0].name == "greet");
   REQUIRE(world.funcs[0].params.size() == 1);
   CHECK(world.funcs[0].params[0].name == "name");
   CHECK(world.funcs[0].params[0].type_idx == wit_prim_idx(wit_prim::string_));
   REQUIRE(world.funcs[0].results.size() == 1);
   CHECK(world.funcs[0].results[0].type_idx == wit_prim_idx(wit_prim::string_));

   CHECK(world.funcs[1].name == "add");
   REQUIRE(world.funcs[1].params.size() == 2);
   CHECK(world.funcs[1].params[0].type_idx == wit_prim_idx(wit_prim::u32));
   CHECK(world.funcs[1].params[1].type_idx == wit_prim_idx(wit_prim::u32));
   CHECK(world.funcs[1].results[0].type_idx == wit_prim_idx(wit_prim::u32));
}

TEST_CASE("wit_parse: import and export functions", "[wit][parser]") {
   auto world = wit_parse(
      "world app {\n"
      "  import log: func(msg: string);\n"
      "  export run: func() -> u32;\n"
      "}\n"
   );

   REQUIRE(world.funcs.size() == 2);
   CHECK(world.funcs[0].name == "log");
   CHECK(world.funcs[0].results.empty());

   CHECK(world.funcs[1].name == "run");
   CHECK(world.funcs[1].params.empty());

   REQUIRE(world.imports.size() == 1);
   REQUIRE(world.exports.size() == 1);
}

TEST_CASE("wit_parse: all primitive types", "[wit][parser]") {
   auto world = wit_parse(
      "world prims {\n"
      "  export test: func(\n"
      "    a: bool, b: u8, c: s8, d: u16, e: s16,\n"
      "    f: u32, g: s32, h: u64, i: s64,\n"
      "    j: f32, k: f64, l: char, m: string\n"
      "  );\n"
      "}\n"
   );

   REQUIRE(world.funcs.size() == 1);
   auto& params = world.funcs[0].params;
   REQUIRE(params.size() == 13);
   CHECK(params[0].type_idx  == wit_prim_idx(wit_prim::bool_));
   CHECK(params[1].type_idx  == wit_prim_idx(wit_prim::u8));
   CHECK(params[2].type_idx  == wit_prim_idx(wit_prim::s8));
   CHECK(params[3].type_idx  == wit_prim_idx(wit_prim::u16));
   CHECK(params[4].type_idx  == wit_prim_idx(wit_prim::s16));
   CHECK(params[5].type_idx  == wit_prim_idx(wit_prim::u32));
   CHECK(params[6].type_idx  == wit_prim_idx(wit_prim::s32));
   CHECK(params[7].type_idx  == wit_prim_idx(wit_prim::u64));
   CHECK(params[8].type_idx  == wit_prim_idx(wit_prim::s64));
   CHECK(params[9].type_idx  == wit_prim_idx(wit_prim::f32));
   CHECK(params[10].type_idx == wit_prim_idx(wit_prim::f64));
   CHECK(params[11].type_idx == wit_prim_idx(wit_prim::char_));
   CHECK(params[12].type_idx == wit_prim_idx(wit_prim::string_));
}

TEST_CASE("wit_parse: record type", "[wit][parser]") {
   auto world = wit_parse(
      "world app {\n"
      "  record person {\n"
      "    name: string,\n"
      "    age: u32,\n"
      "  }\n"
      "  export get-person: func() -> person;\n"
      "}\n"
   );

   REQUIRE(world.types.size() >= 1);
   auto& rec = world.types[0];
   CHECK(rec.name == "person");
   CHECK(rec.kind == static_cast<uint8_t>(wit_type_kind::record_));
   REQUIRE(rec.fields.size() == 2);
   CHECK(rec.fields[0].name == "name");
   CHECK(rec.fields[0].type_idx == wit_prim_idx(wit_prim::string_));
   CHECK(rec.fields[1].name == "age");
   CHECK(rec.fields[1].type_idx == wit_prim_idx(wit_prim::u32));
}

TEST_CASE("wit_parse: variant type", "[wit][parser]") {
   auto world = wit_parse(
      "world app {\n"
      "  variant result-val {\n"
      "    ok(string),\n"
      "    error(u32),\n"
      "  }\n"
      "}\n"
   );

   REQUIRE(world.types.size() >= 1);
   auto& var = world.types[0];
   CHECK(var.name == "result-val");
   CHECK(var.kind == static_cast<uint8_t>(wit_type_kind::variant_));
   REQUIRE(var.fields.size() == 2);
   CHECK(var.fields[0].name == "ok");
   CHECK(var.fields[0].type_idx == wit_prim_idx(wit_prim::string_));
   CHECK(var.fields[1].name == "error");
   CHECK(var.fields[1].type_idx == wit_prim_idx(wit_prim::u32));
}

TEST_CASE("wit_parse: enum type", "[wit][parser]") {
   auto world = wit_parse(
      "world app {\n"
      "  enum color {\n"
      "    red,\n"
      "    green,\n"
      "    blue,\n"
      "  }\n"
      "}\n"
   );

   REQUIRE(world.types.size() >= 1);
   auto& en = world.types[0];
   CHECK(en.name == "color");
   CHECK(en.kind == static_cast<uint8_t>(wit_type_kind::enum_));
   REQUIRE(en.fields.size() == 3);
   CHECK(en.fields[0].name == "red");
   CHECK(en.fields[1].name == "green");
   CHECK(en.fields[2].name == "blue");
}

TEST_CASE("wit_parse: flags type", "[wit][parser]") {
   auto world = wit_parse(
      "world app {\n"
      "  flags permissions {\n"
      "    read,\n"
      "    write,\n"
      "    exec,\n"
      "  }\n"
      "}\n"
   );

   REQUIRE(world.types.size() >= 1);
   auto& fl = world.types[0];
   CHECK(fl.name == "permissions");
   CHECK(fl.kind == static_cast<uint8_t>(wit_type_kind::flags_));
   REQUIRE(fl.fields.size() == 3);
}

TEST_CASE("wit_parse: compound types (list, option, result, tuple)", "[wit][parser]") {
   auto world = wit_parse(
      "world app {\n"
      "  export test: func(\n"
      "    a: list<u8>,\n"
      "    b: option<string>,\n"
      "    c: result<u32, string>,\n"
      "    d: tuple<u32, string, bool>\n"
      "  );\n"
      "}\n"
   );

   REQUIRE(world.funcs.size() == 1);
   auto& params = world.funcs[0].params;
   REQUIRE(params.size() == 4);

   // list<u8>
   REQUIRE(params[0].type_idx >= 0);
   auto& list_type = world.types[params[0].type_idx];
   CHECK(list_type.kind == static_cast<uint8_t>(wit_type_kind::list_));
   CHECK(list_type.element_type_idx == wit_prim_idx(wit_prim::u8));

   // option<string>
   REQUIRE(params[1].type_idx >= 0);
   auto& opt_type = world.types[params[1].type_idx];
   CHECK(opt_type.kind == static_cast<uint8_t>(wit_type_kind::option_));
   CHECK(opt_type.element_type_idx == wit_prim_idx(wit_prim::string_));

   // result<u32, string>
   REQUIRE(params[2].type_idx >= 0);
   auto& res_type = world.types[params[2].type_idx];
   CHECK(res_type.kind == static_cast<uint8_t>(wit_type_kind::result_));
   CHECK(res_type.element_type_idx == wit_prim_idx(wit_prim::u32));
   CHECK(res_type.error_type_idx == wit_prim_idx(wit_prim::string_));

   // tuple<u32, string, bool>
   REQUIRE(params[3].type_idx >= 0);
   auto& tup_type = world.types[params[3].type_idx];
   CHECK(tup_type.kind == static_cast<uint8_t>(wit_type_kind::tuple_));
   REQUIRE(tup_type.fields.size() == 3);
}

TEST_CASE("wit_parse: interface block", "[wit][parser]") {
   auto world = wit_parse(
      "interface greeter {\n"
      "  record greeting {\n"
      "    message: string,\n"
      "  }\n"
      "  greet: func(name: string) -> greeting;\n"
      "}\n"
   );

   REQUIRE(world.exports.size() == 1);
   CHECK(world.exports[0].name == "greeter");
   CHECK(world.exports[0].type_idxs.size() == 1);
   CHECK(world.exports[0].func_idxs.size() == 1);
}

TEST_CASE("wit_parse: comments", "[wit][parser]") {
   auto world = wit_parse(
      "// This is a line comment\n"
      "/* This is a block comment */\n"
      "world test {\n"
      "  /// Doc comment on export\n"
      "  export hello: func() -> string;\n"
      "}\n"
   );

   CHECK(world.name == "test");
   REQUIRE(world.funcs.size() == 1);
   CHECK(world.funcs[0].name == "hello");
}

TEST_CASE("wit_parse: no-return function", "[wit][parser]") {
   auto world = wit_parse(
      "world app {\n"
      "  export fire-and-forget: func(data: list<u8>);\n"
      "}\n"
   );

   REQUIRE(world.funcs.size() == 1);
   CHECK(world.funcs[0].results.empty());
}

TEST_CASE("wit_parse error: unexpected token", "[wit][parser]") {
   CHECK_THROWS_AS(wit_parse("world test { 42 }"), wit_parse_error);
}

TEST_CASE("wit_parse error: unclosed brace", "[wit][parser]") {
   CHECK_THROWS_AS(wit_parse("world test {"), wit_parse_error);
}

TEST_CASE("wit_parse: record with type reference", "[wit][parser]") {
   auto world = wit_parse(
      "world app {\n"
      "  record address {\n"
      "    street: string,\n"
      "    city: string,\n"
      "  }\n"
      "  record person {\n"
      "    name: string,\n"
      "    home: address,\n"
      "  }\n"
      "}\n"
   );

   REQUIRE(world.types.size() >= 2);
   // address is type 0
   CHECK(world.types[0].name == "address");
   // person is type 1, with field "home" referencing type 0
   CHECK(world.types[1].name == "person");
   REQUIRE(world.types[1].fields.size() == 2);
   CHECK(world.types[1].fields[1].name == "home");
   CHECK(world.types[1].fields[1].type_idx == 0); // index of address
}

// ── Phase 3: wit_gen.hpp — C++ → WIT generation ─────────────────────────────

// Test types for WIT generation — must be at namespace scope for PSIO_REFLECT

namespace wit_test {
   struct test_exports {
      std::string greet(std::string name) { return "hello " + name; }
      uint32_t add(uint32_t a, uint32_t b) { return a + b; }
      void fire_and_forget() {}
   };

   struct test_imports {
      void log_message(std::string msg) {}
      uint64_t get_time() { return 0; }
   };

   struct test_address {
      std::string street;
      std::string city;
   };

   struct test_person {
      std::string name;
      uint32_t age;
      test_address home;
   };

   struct test_record_exports {
      test_person get_person() { return {}; }
      void set_person(test_person p) {}
   };

   struct test_container_exports {
      std::vector<uint8_t> get_bytes() { return {}; }
      std::optional<std::string> find(std::string key) { return {}; }
   };

   struct test_prim_exports {
      void test(bool a, uint8_t b, int8_t c, uint16_t d, int16_t e,
                uint32_t f, int32_t g, uint64_t h, int64_t i,
                float j, double k) {}
   };

   struct test_dedup_exports {
      test_person echo(test_person p) { return p; }
   };

   PSIO_REFLECT(test_exports, method(greet, name), method(add, a, b), method(fire_and_forget))
   PSIO_REFLECT(test_imports, method(log_message, msg), method(get_time))
   PSIO_REFLECT(test_address, street, city)
   PSIO_REFLECT(test_person, name, age, home)
   PSIO_REFLECT(test_record_exports, method(get_person), method(set_person, p))
   PSIO_REFLECT(test_container_exports, method(get_bytes), method(find, key))
   PSIO_REFLECT(test_prim_exports, method(test, a, b, c, d, e, f, g, h, i, j, k))
   PSIO_REFLECT(test_dedup_exports, method(echo, p))
} // namespace wit_test

TEST_CASE("generate_wit: basic exports", "[wit][gen]") {
   auto world = generate_wit<wit_test::test_exports>();

   CHECK(world.name == "test_exports");
   REQUIRE(world.exports.size() == 1);
   REQUIRE(world.funcs.size() == 3);

   // greet(name: string) -> string
   CHECK(world.funcs[0].name == "greet");
   REQUIRE(world.funcs[0].params.size() == 1);
   CHECK(world.funcs[0].params[0].name == "name");
   CHECK(world.funcs[0].params[0].type_idx == wit_prim_idx(wit_prim::string_));
   REQUIRE(world.funcs[0].results.size() == 1);
   CHECK(world.funcs[0].results[0].type_idx == wit_prim_idx(wit_prim::string_));

   // add(a: u32, b: u32) -> u32
   CHECK(world.funcs[1].name == "add");
   REQUIRE(world.funcs[1].params.size() == 2);
   CHECK(world.funcs[1].params[0].name == "a");
   CHECK(world.funcs[1].params[0].type_idx == wit_prim_idx(wit_prim::u32));
   CHECK(world.funcs[1].params[1].name == "b");
   CHECK(world.funcs[1].results[0].type_idx == wit_prim_idx(wit_prim::u32));

   // fire_and_forget() — no results, kebab-cased
   CHECK(world.funcs[2].name == "fire-and-forget");
   CHECK(world.funcs[2].params.empty());
   CHECK(world.funcs[2].results.empty());
}

TEST_CASE("generate_wit: exports and imports", "[wit][gen]") {
   auto world = generate_wit<wit_test::test_exports, wit_test::test_imports>();

   REQUIRE(world.exports.size() == 1);
   REQUIRE(world.imports.size() == 1);

   // Import functions
   auto& imp = world.imports[0];
   REQUIRE(imp.func_idxs.size() == 2);
   // Import funcs start after export funcs
   auto& log_func = world.funcs[imp.func_idxs[0]];
   CHECK(log_func.name == "log-message");
   CHECK(log_func.params[0].type_idx == wit_prim_idx(wit_prim::string_));
   CHECK(log_func.results.empty());

   auto& time_func = world.funcs[imp.func_idxs[1]];
   CHECK(time_func.name == "get-time");
   CHECK(time_func.params.empty());
   CHECK(time_func.results[0].type_idx == wit_prim_idx(wit_prim::u64));
}

TEST_CASE("generate_wit: record types from reflected structs", "[wit][gen]") {
   auto world = generate_wit<wit_test::test_record_exports>();

   // Should have generated record types for test_person and test_address
   REQUIRE(world.funcs.size() == 2);

   // get_person() -> person
   auto& get_func = world.funcs[0];
   CHECK(get_func.name == "get-person");
   REQUIRE(get_func.results.size() == 1);
   int32_t person_idx = get_func.results[0].type_idx;
   REQUIRE(person_idx >= 0);

   auto& person_type = world.types[person_idx];
   CHECK(person_type.name == "test_person");
   CHECK(person_type.kind == static_cast<uint8_t>(wit_type_kind::record_));
   REQUIRE(person_type.fields.size() == 3);
   CHECK(person_type.fields[0].name == "name");
   CHECK(person_type.fields[0].type_idx == wit_prim_idx(wit_prim::string_));
   CHECK(person_type.fields[1].name == "age");
   CHECK(person_type.fields[1].type_idx == wit_prim_idx(wit_prim::u32));

   // "home" field should reference test_address record
   CHECK(person_type.fields[2].name == "home");
   int32_t addr_idx = person_type.fields[2].type_idx;
   REQUIRE(addr_idx >= 0);
   auto& addr_type = world.types[addr_idx];
   CHECK(addr_type.name == "test_address");
   CHECK(addr_type.kind == static_cast<uint8_t>(wit_type_kind::record_));
   REQUIRE(addr_type.fields.size() == 2);
   CHECK(addr_type.fields[0].name == "street");
   CHECK(addr_type.fields[1].name == "city");
}

TEST_CASE("generate_wit: container types (vector, optional)", "[wit][gen]") {
   auto world = generate_wit<wit_test::test_container_exports>();

   REQUIRE(world.funcs.size() == 2);

   // get_bytes() -> list<u8>
   auto& bytes_func = world.funcs[0];
   CHECK(bytes_func.name == "get-bytes");
   REQUIRE(bytes_func.results.size() == 1);
   int32_t list_idx = bytes_func.results[0].type_idx;
   REQUIRE(list_idx >= 0);
   auto& list_type = world.types[list_idx];
   CHECK(list_type.kind == static_cast<uint8_t>(wit_type_kind::list_));
   CHECK(list_type.element_type_idx == wit_prim_idx(wit_prim::u8));

   // find(key: string) -> option<string>
   auto& find_func = world.funcs[1];
   CHECK(find_func.name == "find");
   REQUIRE(find_func.results.size() == 1);
   int32_t opt_idx = find_func.results[0].type_idx;
   REQUIRE(opt_idx >= 0);
   auto& opt_type = world.types[opt_idx];
   CHECK(opt_type.kind == static_cast<uint8_t>(wit_type_kind::option_));
   CHECK(opt_type.element_type_idx == wit_prim_idx(wit_prim::string_));
}

TEST_CASE("generate_wit: all primitive param types", "[wit][gen]") {
   auto world = generate_wit<wit_test::test_prim_exports>();
   REQUIRE(world.funcs.size() == 1);
   auto& params = world.funcs[0].params;
   REQUIRE(params.size() == 11);
   CHECK(params[0].type_idx  == wit_prim_idx(wit_prim::bool_));
   CHECK(params[1].type_idx  == wit_prim_idx(wit_prim::u8));
   CHECK(params[2].type_idx  == wit_prim_idx(wit_prim::s8));
   CHECK(params[3].type_idx  == wit_prim_idx(wit_prim::u16));
   CHECK(params[4].type_idx  == wit_prim_idx(wit_prim::s16));
   CHECK(params[5].type_idx  == wit_prim_idx(wit_prim::u32));
   CHECK(params[6].type_idx  == wit_prim_idx(wit_prim::s32));
   CHECK(params[7].type_idx  == wit_prim_idx(wit_prim::u64));
   CHECK(params[8].type_idx  == wit_prim_idx(wit_prim::s64));
   CHECK(params[9].type_idx  == wit_prim_idx(wit_prim::f32));
   CHECK(params[10].type_idx == wit_prim_idx(wit_prim::f64));
}

TEST_CASE("generate_wit_text: produces valid WIT", "[wit][gen]") {
   auto text = generate_wit_text<wit_test::test_exports>("my-app");
   CHECK(text.find("world my-app") != std::string::npos);
   CHECK(text.find("greet: func(name: string) -> string") != std::string::npos);
   CHECK(text.find("add: func(a: u32, b: u32) -> u32") != std::string::npos);
   CHECK(text.find("fire-and-forget: func()") != std::string::npos);
}

TEST_CASE("generate_wit: custom world name", "[wit][gen]") {
   auto world = generate_wit<wit_test::test_exports>("custom-name");
   CHECK(world.name == "custom-name");
}

TEST_CASE("wit_to_text: record types rendered", "[wit][gen]") {
   auto world = generate_wit<wit_test::test_record_exports>();
   auto text = wit_to_text(world);
   CHECK(text.find("record test_person") != std::string::npos);
   CHECK(text.find("record test_address") != std::string::npos);
   CHECK(text.find("name: string") != std::string::npos);
   CHECK(text.find("age: u32") != std::string::npos);
}

TEST_CASE("generate_wit: deduplicates types", "[wit][gen]") {
   // Using the same type in both param and return should not create duplicates
   auto world = generate_wit<wit_test::test_dedup_exports>();

   // Count how many types are named "test_person"
   int person_count = 0;
   for (auto& t : world.types)
      if (t.name == "test_person") person_count++;
   CHECK(person_count == 1);
}

// ── Phase 4: canonical_abi.hpp — layout, flatten, store/load, lower/lift ─────

TEST_CASE("canonical_abi: primitive layouts", "[wit][abi]") {
   pzam_wit_world world;

   CHECK(layout_of(world, wit_prim_idx(wit_prim::bool_)).size == 1);
   CHECK(layout_of(world, wit_prim_idx(wit_prim::u8)).size == 1);
   CHECK(layout_of(world, wit_prim_idx(wit_prim::u16)).size == 2);
   CHECK(layout_of(world, wit_prim_idx(wit_prim::u32)).size == 4);
   CHECK(layout_of(world, wit_prim_idx(wit_prim::u64)).size == 8);
   CHECK(layout_of(world, wit_prim_idx(wit_prim::f32)).size == 4);
   CHECK(layout_of(world, wit_prim_idx(wit_prim::f64)).size == 8);
   CHECK(layout_of(world, wit_prim_idx(wit_prim::string_)).size == 8); // ptr+len

   CHECK(layout_of(world, wit_prim_idx(wit_prim::u32)).align == 4);
   CHECK(layout_of(world, wit_prim_idx(wit_prim::u64)).align == 8);
}

TEST_CASE("canonical_abi: record layout", "[wit][abi]") {
   pzam_wit_world world;

   // record { a: u32, b: u8, c: u32 }
   // Expected: u32(4) + u8(1) + pad(3) + u32(4) = 12, align=4
   wit_type_def rec;
   rec.kind = static_cast<uint8_t>(wit_type_kind::record_);
   rec.fields = {
      {"a", wit_prim_idx(wit_prim::u32)},
      {"b", wit_prim_idx(wit_prim::u8)},
      {"c", wit_prim_idx(wit_prim::u32)},
   };
   world.types.push_back(rec);

   auto layout = layout_of(world, 0);
   CHECK(layout.size == 12);
   CHECK(layout.align == 4);
}

TEST_CASE("canonical_abi: flatten primitives", "[wit][abi]") {
   pzam_wit_world world;

   CHECK(flatten(world, wit_prim_idx(wit_prim::u32)) == std::vector{flat_type::i32});
   CHECK(flatten(world, wit_prim_idx(wit_prim::u64)) == std::vector{flat_type::i64});
   CHECK(flatten(world, wit_prim_idx(wit_prim::f32)) == std::vector{flat_type::f32});
   CHECK(flatten(world, wit_prim_idx(wit_prim::f64)) == std::vector{flat_type::f64});

   auto str_flat = flatten(world, wit_prim_idx(wit_prim::string_));
   REQUIRE(str_flat.size() == 2);
   CHECK(str_flat[0] == flat_type::i32);
   CHECK(str_flat[1] == flat_type::i32);
}

TEST_CASE("canonical_abi: flatten record", "[wit][abi]") {
   pzam_wit_world world;

   // record { x: u32, y: f64 }
   wit_type_def rec;
   rec.kind = static_cast<uint8_t>(wit_type_kind::record_);
   rec.fields = {
      {"x", wit_prim_idx(wit_prim::u32)},
      {"y", wit_prim_idx(wit_prim::f64)},
   };
   world.types.push_back(rec);

   auto flat = flatten(world, 0);
   REQUIRE(flat.size() == 2);
   CHECK(flat[0] == flat_type::i32);
   CHECK(flat[1] == flat_type::f64);
}

TEST_CASE("canonical_abi: store/load scalar round-trip", "[wit][abi]") {
   pzam_wit_world world;
   std::vector<uint8_t> memory(256, 0);
   canonical_memory mem;
   mem.base = memory.data();
   mem.length = memory.size();

   // u32
   auto val_u32 = dynamic_value::make_u32(42);
   canonical_store(world, wit_prim_idx(wit_prim::u32), val_u32, mem, 0);
   auto loaded_u32 = canonical_load(world, wit_prim_idx(wit_prim::u32), mem, 0);
   CHECK(loaded_u32.as_u32() == 42);

   // f64
   auto val_f64 = dynamic_value::make_f64(3.14);
   canonical_store(world, wit_prim_idx(wit_prim::f64), val_f64, mem, 8);
   auto loaded_f64 = canonical_load(world, wit_prim_idx(wit_prim::f64), mem, 8);
   CHECK(loaded_f64.as_f64() == Approx(3.14));

   // bool
   auto val_bool = dynamic_value::make_bool(true);
   canonical_store(world, wit_prim_idx(wit_prim::bool_), val_bool, mem, 16);
   auto loaded_bool = canonical_load(world, wit_prim_idx(wit_prim::bool_), mem, 16);
   CHECK(loaded_bool.as_bool() == true);
}

TEST_CASE("canonical_abi: store/load string round-trip", "[wit][abi]") {
   std::vector<uint8_t> memory(4096, 0);
   uint32_t alloc_ptr = 256; // start allocating at offset 256

   canonical_memory mem;
   mem.base = memory.data();
   mem.length = memory.size();
   mem.host = &alloc_ptr;
   mem.realloc = [](void* host, uint32_t, uint32_t, uint32_t align, uint32_t size) -> uint32_t {
      auto* ptr = static_cast<uint32_t*>(host);
      uint32_t result = (*ptr + align - 1) & ~(align - 1);
      *ptr = result + size;
      return result;
   };

   pzam_wit_world world;

   auto val = dynamic_value::make_string("hello world");
   canonical_store(world, wit_prim_idx(wit_prim::string_), val, mem, 0);

   auto loaded = canonical_load(world, wit_prim_idx(wit_prim::string_), mem, 0);
   CHECK(loaded.as_string() == "hello world");
}

TEST_CASE("canonical_abi: store/load record round-trip", "[wit][abi]") {
   std::vector<uint8_t> memory(4096, 0);
   uint32_t alloc_ptr = 256;

   canonical_memory mem;
   mem.base = memory.data();
   mem.length = memory.size();
   mem.host = &alloc_ptr;
   mem.realloc = [](void* host, uint32_t, uint32_t, uint32_t align, uint32_t size) -> uint32_t {
      auto* ptr = static_cast<uint32_t*>(host);
      uint32_t result = (*ptr + align - 1) & ~(align - 1);
      *ptr = result + size;
      return result;
   };

   pzam_wit_world world;

   // record { name: string, age: u32 }
   wit_type_def rec;
   rec.kind = static_cast<uint8_t>(wit_type_kind::record_);
   rec.fields = {
      {"name", wit_prim_idx(wit_prim::string_)},
      {"age",  wit_prim_idx(wit_prim::u32)},
   };
   world.types.push_back(rec);

   auto val = dynamic_value::make_record({
      dynamic_value::make_string("Alice"),
      dynamic_value::make_u32(30),
   });

   canonical_store(world, 0, val, mem, 0);
   auto loaded = canonical_load(world, 0, mem, 0);

   REQUIRE(loaded.fields().size() == 2);
   CHECK(loaded.fields()[0].as_string() == "Alice");
   CHECK(loaded.fields()[1].as_u32() == 30);
}

TEST_CASE("canonical_abi: store/load enum round-trip", "[wit][abi]") {
   std::vector<uint8_t> memory(256, 0);
   canonical_memory mem;
   mem.base = memory.data();
   mem.length = memory.size();

   pzam_wit_world world;
   wit_type_def en;
   en.kind = static_cast<uint8_t>(wit_type_kind::enum_);
   en.fields = {{"red", 0}, {"green", 0}, {"blue", 0}};
   world.types.push_back(en);

   auto val = dynamic_value::make_enum(2); // blue
   canonical_store(world, 0, val, mem, 0);
   auto loaded = canonical_load(world, 0, mem, 0);
   CHECK(loaded.as_enum() == 2);
}

TEST_CASE("canonical_abi: store/load list round-trip", "[wit][abi]") {
   std::vector<uint8_t> memory(4096, 0);
   uint32_t alloc_ptr = 256;

   canonical_memory mem;
   mem.base = memory.data();
   mem.length = memory.size();
   mem.host = &alloc_ptr;
   mem.realloc = [](void* host, uint32_t, uint32_t, uint32_t align, uint32_t size) -> uint32_t {
      auto* ptr = static_cast<uint32_t*>(host);
      uint32_t result = (*ptr + align - 1) & ~(align - 1);
      *ptr = result + size;
      return result;
   };

   pzam_wit_world world;
   wit_type_def lst;
   lst.kind = static_cast<uint8_t>(wit_type_kind::list_);
   lst.element_type_idx = wit_prim_idx(wit_prim::u32);
   world.types.push_back(lst);

   auto val = dynamic_value::make_list({
      dynamic_value::make_u32(10),
      dynamic_value::make_u32(20),
      dynamic_value::make_u32(30),
   });

   canonical_store(world, 0, val, mem, 0);
   auto loaded = canonical_load(world, 0, mem, 0);

   REQUIRE(loaded.as_list().size() == 3);
   CHECK(loaded.as_list()[0].as_u32() == 10);
   CHECK(loaded.as_list()[1].as_u32() == 20);
   CHECK(loaded.as_list()[2].as_u32() == 30);
}

TEST_CASE("canonical_abi: lower/lift scalar function", "[wit][abi]") {
   std::vector<uint8_t> memory(4096, 0);
   uint32_t alloc_ptr = 256;

   canonical_memory mem;
   mem.base = memory.data();
   mem.length = memory.size();
   mem.host = &alloc_ptr;
   mem.realloc = [](void* host, uint32_t, uint32_t, uint32_t align, uint32_t size) -> uint32_t {
      auto* ptr = static_cast<uint32_t*>(host);
      uint32_t result = (*ptr + align - 1) & ~(align - 1);
      *ptr = result + size;
      return result;
   };

   pzam_wit_world world;

   // func add(a: u32, b: u32) -> u32
   wit_func func;
   func.name = "add";
   func.params = {
      {"a", wit_prim_idx(wit_prim::u32)},
      {"b", wit_prim_idx(wit_prim::u32)},
   };
   func.results = {{"", wit_prim_idx(wit_prim::u32)}};

   std::vector<dynamic_value> args = {
      dynamic_value::make_u32(3),
      dynamic_value::make_u32(4),
   };

   auto lowered = canonical_lower(world, func, args, mem);
   REQUIRE(lowered.size() == 2);
   CHECK(lowered[0].i32 == 3);
   CHECK(lowered[1].i32 == 4);

   // Simulate WASM returning 7
   native_value ret = {}; ret.i32 = 7;
   auto lifted = canonical_lift(world, func, std::span(&ret, 1), mem);
   CHECK(lifted.as_u32() == 7);
}

TEST_CASE("canonical_abi: lower string argument", "[wit][abi]") {
   std::vector<uint8_t> memory(4096, 0);
   uint32_t alloc_ptr = 256;

   canonical_memory mem;
   mem.base = memory.data();
   mem.length = memory.size();
   mem.host = &alloc_ptr;
   mem.realloc = [](void* host, uint32_t, uint32_t, uint32_t align, uint32_t size) -> uint32_t {
      auto* ptr = static_cast<uint32_t*>(host);
      uint32_t result = (*ptr + align - 1) & ~(align - 1);
      *ptr = result + size;
      return result;
   };

   pzam_wit_world world;

   // func greet(name: string)
   wit_func func;
   func.name = "greet";
   func.params = {{"name", wit_prim_idx(wit_prim::string_)}};

   std::vector<dynamic_value> args = {
      dynamic_value::make_string("world"),
   };

   auto lowered = canonical_lower(world, func, args, mem);
   REQUIRE(lowered.size() == 2); // ptr, len
   CHECK(lowered[1].i32 == 5);   // "world" length

   // Verify the string was actually written to memory
   uint32_t str_ptr = lowered[0].i32;
   std::string stored(reinterpret_cast<char*>(memory.data() + str_ptr), 5);
   CHECK(stored == "world");
}

TEST_CASE("dynamic_value: basic construction and access", "[wit][abi]") {
   CHECK(dynamic_value::make_bool(true).as_bool() == true);
   CHECK(dynamic_value::make_bool(false).as_bool() == false);
   CHECK(dynamic_value::make_u32(42).as_u32() == 42);
   CHECK(dynamic_value::make_s32(-1).as_s32() == -1);
   CHECK(dynamic_value::make_u64(1ULL << 40).as_u64() == (1ULL << 40));
   CHECK(dynamic_value::make_f32(1.5f).as_f32() == 1.5f);
   CHECK(dynamic_value::make_f64(2.5).as_f64() == 2.5);
   CHECK(dynamic_value::make_string("hello").as_string() == "hello");

   auto list = dynamic_value::make_list({
      dynamic_value::make_u32(1),
      dynamic_value::make_u32(2),
   });
   REQUIRE(list.as_list().size() == 2);
   CHECK(list.as_list()[0].as_u32() == 1);

   auto rec = dynamic_value::make_record({
      dynamic_value::make_string("test"),
      dynamic_value::make_u32(42),
   });
   REQUIRE(rec.fields().size() == 2);
   CHECK(rec.fields()[0].as_string() == "test");
   CHECK(rec.fields()[1].as_u32() == 42);
}
