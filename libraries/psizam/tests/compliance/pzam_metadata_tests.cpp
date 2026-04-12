// Tests for pzam_metadata.hpp — extract/restore round-trip verification.

#include <psizam/pzam_metadata.hpp>
#include <psizam/pzam_format.hpp>
#include <psizam/types.hpp>
#include <catch2/catch.hpp>

using namespace psizam;

// ── Helper: build a minimal module with known data ──────────────────────────

static module make_test_module() {
   module mod;

   // Types
   func_type ft0;
   ft0.form = types::func;
   ft0.param_types = {types::i32, types::i64};
   ft0.return_types = {types::i32};
   ft0.finalize_returns();
   ft0.compute_sig_hash();
   mod.types.push_back(ft0);

   func_type ft1;
   ft1.form = types::func;
   ft1.param_types = {};
   ft1.return_types = {};
   ft1.finalize_returns();
   ft1.compute_sig_hash();
   mod.types.push_back(ft1);

   // Multi-value return type
   func_type ft2;
   ft2.form = types::func;
   ft2.param_types = {types::f32};
   ft2.return_types = {types::f32, types::f64};
   ft2.finalize_returns();
   ft2.compute_sig_hash();
   mod.types.push_back(ft2);

   // Imports
   import_entry imp_func;
   imp_func.module_str = {'e', 'n', 'v'};
   imp_func.field_str = {'l', 'o', 'g'};
   imp_func.kind = Function;
   imp_func.type.func_t = 0;
   mod.imports.push_back(imp_func);

   import_entry imp_mem;
   imp_mem.module_str = {'e', 'n', 'v'};
   imp_mem.field_str = {'m', 'e', 'm'};
   imp_mem.kind = Memory;
   imp_mem.type.mem_t.limits = {true, 1, 256};
   mod.imports.push_back(imp_mem);

   import_entry imp_table;
   imp_table.module_str = {'e', 'n', 'v'};
   imp_table.field_str = {'t', 'b', 'l'};
   imp_table.kind = Table;
   imp_table.type.table_t.element_type = types::funcref;
   imp_table.type.table_t.limits = {true, 10, 100};
   mod.imports.push_back(imp_table);

   import_entry imp_global;
   imp_global.module_str = {'e', 'n', 'v'};
   imp_global.field_str = {'g'};
   imp_global.kind = Global;
   imp_global.type.global_t.content_type = types::i32;
   imp_global.type.global_t.mutability = false;
   mod.imports.push_back(imp_global);

   // Functions (type indices for local functions)
   mod.functions = {0, 1, 2};

   // Tables
   table_type tt;
   tt.element_type = types::funcref;
   tt.limits = {true, 5, 50};
   mod.tables.push_back(tt);

   // Memories
   memory_type mt;
   mt.limits = {true, 1, 256};
   mod.memories.push_back(mt);

   // Globals
   global_variable gv_i32;
   gv_i32.type = {types::i32, true};
   gv_i32.init.opcode = opcodes::i32_const;
   gv_i32.init.value.i32 = 42;
   mod.globals.push_back(gv_i32);

   global_variable gv_i64;
   gv_i64.type = {types::i64, false};
   gv_i64.init.opcode = opcodes::i64_const;
   gv_i64.init.value.i64 = -123456789LL;
   mod.globals.push_back(gv_i64);

   global_variable gv_f32;
   gv_f32.type = {types::f32, false};
   gv_f32.init.opcode = opcodes::f32_const;
   gv_f32.init.value.f32 = 0x40490FDB; // pi as uint32
   mod.globals.push_back(gv_f32);

   global_variable gv_f64;
   gv_f64.type = {types::f64, false};
   gv_f64.init.opcode = opcodes::f64_const;
   gv_f64.init.value.f64 = 0x400921FB54442D18ULL; // pi as uint64
   mod.globals.push_back(gv_f64);

   // Exports
   export_entry exp0;
   exp0.field_str = {'_', 's', 't', 'a', 'r', 't'};
   exp0.kind = Function;
   exp0.index = 1;
   mod.exports.push_back(exp0);

   export_entry exp1;
   exp1.field_str = {'m', 'e', 'm', 'o', 'r', 'y'};
   exp1.kind = Memory;
   exp1.index = 0;
   mod.exports.push_back(exp1);

   // Element segment (active)
   elem_segment es;
   es.index = 0;
   es.offset.opcode = opcodes::i32_const;
   es.offset.value.i32 = 0;
   es.mode = elem_mode::active;
   es.type = types::funcref;
   table_entry te0{types::funcref, 1, nullptr};
   table_entry te1{types::funcref, 2, nullptr};
   es.elems = {te0, te1};
   mod.elements.push_back(es);

   // Data segment (active)
   data_segment ds;
   ds.index = 0;
   ds.offset.opcode = opcodes::i32_const;
   ds.offset.value.i32 = 1024;
   ds.passive = false;
   ds.data = {0x48, 0x65, 0x6C, 0x6C, 0x6F}; // "Hello"
   mod.data.push_back(ds);

   // Data segment (passive)
   data_segment ds2;
   ds2.index = 0;
   ds2.passive = true;
   ds2.data = {0x01, 0x02, 0x03};
   mod.data.push_back(ds2);

   // Tags
   tag_type tg;
   tg.attribute = 0;
   tg.type_index = 0;
   mod.tags.push_back(tg);

   // Start function
   mod.start = 1;

   // Derived counts
   mod.num_imported_tables = 1;
   mod.num_imported_memories = 1;
   mod.num_imported_globals = 1;

   // Code entries (empty bodies — just for sizing)
   mod.code.resize(3);

   return mod;
}

// ── Tests ───────────────────────────────────────────────────────────────────

TEST_CASE("pzam_metadata: extract round-trip preserves types", "[pzam_metadata]") {
   auto mod = make_test_module();
   auto meta = extract_metadata(mod);

   REQUIRE(meta.types.size() == 3);

   CHECK(meta.types[0].param_types.size() == 2);
   CHECK(meta.types[0].param_types[0] == types::i32);
   CHECK(meta.types[0].param_types[1] == types::i64);
   CHECK(meta.types[0].return_types.size() == 1);
   CHECK(meta.types[0].return_types[0] == types::i32);

   CHECK(meta.types[1].param_types.empty());
   CHECK(meta.types[1].return_types.empty());

   CHECK(meta.types[2].param_types.size() == 1);
   CHECK(meta.types[2].return_types.size() == 2);
}

TEST_CASE("pzam_metadata: extract round-trip preserves imports", "[pzam_metadata]") {
   auto mod = make_test_module();
   auto meta = extract_metadata(mod);

   REQUIRE(meta.imports.size() == 4);

   CHECK(meta.imports[0].module_name == "env");
   CHECK(meta.imports[0].field_name == "log");
   CHECK(meta.imports[0].kind == Function);
   CHECK(meta.imports[0].func_type_idx == 0);

   CHECK(meta.imports[1].kind == Memory);
   CHECK(meta.imports[1].memory_type.limits.has_maximum == 1);
   CHECK(meta.imports[1].memory_type.limits.initial == 1);
   CHECK(meta.imports[1].memory_type.limits.maximum == 256);

   CHECK(meta.imports[2].kind == Table);
   CHECK(meta.imports[2].table_type.element_type == types::funcref);
   CHECK(meta.imports[2].table_type.limits.initial == 10);

   CHECK(meta.imports[3].kind == Global);
   CHECK(meta.imports[3].global_type.content_type == types::i32);
   CHECK(meta.imports[3].global_type.mutability == 0);
}

TEST_CASE("pzam_metadata: extract round-trip preserves exports", "[pzam_metadata]") {
   auto mod = make_test_module();
   auto meta = extract_metadata(mod);

   REQUIRE(meta.exports.size() == 2);
   CHECK(meta.exports[0].field_name == "_start");
   CHECK(meta.exports[0].kind == Function);
   CHECK(meta.exports[0].index == 1);
   CHECK(meta.exports[1].field_name == "memory");
   CHECK(meta.exports[1].kind == Memory);
}

TEST_CASE("pzam_metadata: extract round-trip preserves globals", "[pzam_metadata]") {
   auto mod = make_test_module();
   auto meta = extract_metadata(mod);

   REQUIRE(meta.globals.size() == 4);

   CHECK(meta.globals[0].type.content_type == types::i32);
   CHECK(meta.globals[0].type.mutability == 1);

   CHECK(meta.globals[1].type.content_type == types::i64);
   CHECK(meta.globals[1].type.mutability == 0);

   CHECK(meta.globals[2].type.content_type == types::f32);
   CHECK(meta.globals[3].type.content_type == types::f64);
}

TEST_CASE("pzam_metadata: extract round-trip preserves data segments", "[pzam_metadata]") {
   auto mod = make_test_module();
   auto meta = extract_metadata(mod);

   REQUIRE(meta.data.size() == 2);

   CHECK(meta.data[0].memory_index == 0);
   CHECK(meta.data[0].passive == 0);
   REQUIRE(meta.data[0].data.size() == 5);
   CHECK(meta.data[0].data[0] == 0x48);

   CHECK(meta.data[1].passive == 1);
   REQUIRE(meta.data[1].data.size() == 3);
}

TEST_CASE("pzam_metadata: extract round-trip preserves elements", "[pzam_metadata]") {
   auto mod = make_test_module();
   auto meta = extract_metadata(mod);

   REQUIRE(meta.elements.size() == 1);
   CHECK(meta.elements[0].table_index == 0);
   CHECK(meta.elements[0].mode == 0); // active
   CHECK(meta.elements[0].elem_type == types::funcref);
   REQUIRE(meta.elements[0].elems.size() == 2);
   CHECK(meta.elements[0].elems[0].index == 1);
   CHECK(meta.elements[0].elems[1].index == 2);
}

TEST_CASE("pzam_metadata: extract round-trip preserves derived counts", "[pzam_metadata]") {
   auto mod = make_test_module();
   auto meta = extract_metadata(mod);

   CHECK(meta.num_imported_functions == 1);
   CHECK(meta.num_imported_tables == 1);
   CHECK(meta.num_imported_memories == 1);
   CHECK(meta.num_imported_globals == 1);
   CHECK(meta.start_function == 1);
}

TEST_CASE("pzam_metadata: full round-trip extract→fracpack→restore", "[pzam_metadata]") {
   auto mod = make_test_module();
   auto meta = extract_metadata(mod);

   // Serialize to fracpack and back
   auto bytes = psio::convert_to_frac(meta);
   auto restored_meta = psio::from_frac<pzam_module_metadata>(bytes);

   // Restore to module
   auto restored = restore_module(restored_meta);

   // Verify types
   REQUIRE(restored.types.size() == mod.types.size());
   for (size_t i = 0; i < mod.types.size(); i++) {
      CHECK(restored.types[i].form == types::func);
      CHECK(restored.types[i].param_types == mod.types[i].param_types);
      CHECK(restored.types[i].return_types == mod.types[i].return_types);
      CHECK(restored.types[i].sig_hash == mod.types[i].sig_hash);
      CHECK(restored.types[i].return_count == mod.types[i].return_count);
      CHECK(restored.types[i].return_type == mod.types[i].return_type);
   }

   // Verify imports
   REQUIRE(restored.imports.size() == mod.imports.size());
   for (size_t i = 0; i < mod.imports.size(); i++) {
      CHECK(restored.imports[i].module_str == mod.imports[i].module_str);
      CHECK(restored.imports[i].field_str == mod.imports[i].field_str);
      CHECK(restored.imports[i].kind == mod.imports[i].kind);
   }

   // Verify functions
   CHECK(restored.functions == mod.functions);

   // Verify tables
   REQUIRE(restored.tables.size() == mod.tables.size());
   CHECK(restored.tables[0].element_type == mod.tables[0].element_type);
   CHECK(restored.tables[0].limits.initial == mod.tables[0].limits.initial);
   CHECK(restored.tables[0].limits.maximum == mod.tables[0].limits.maximum);

   // Verify memories
   REQUIRE(restored.memories.size() == mod.memories.size());
   CHECK(restored.memories[0].limits.initial == mod.memories[0].limits.initial);
   CHECK(restored.memories[0].limits.maximum == mod.memories[0].limits.maximum);

   // Verify globals
   REQUIRE(restored.globals.size() == mod.globals.size());
   CHECK(restored.globals[0].type.content_type == mod.globals[0].type.content_type);
   CHECK(restored.globals[0].type.mutability == mod.globals[0].type.mutability);
   CHECK(restored.globals[0].init.opcode == opcodes::i32_const);
   CHECK(restored.globals[0].init.value.i32 == 42);
   CHECK(restored.globals[1].init.opcode == opcodes::i64_const);
   CHECK(restored.globals[1].init.value.i64 == -123456789LL);

   // Verify exports
   REQUIRE(restored.exports.size() == mod.exports.size());
   CHECK(restored.exports[0].field_str == mod.exports[0].field_str);
   CHECK(restored.exports[0].kind == mod.exports[0].kind);
   CHECK(restored.exports[0].index == mod.exports[0].index);

   // Verify elements
   REQUIRE(restored.elements.size() == mod.elements.size());
   CHECK(restored.elements[0].index == mod.elements[0].index);
   CHECK(restored.elements[0].mode == mod.elements[0].mode);
   CHECK(restored.elements[0].elems.size() == mod.elements[0].elems.size());
   CHECK(restored.elements[0].elems[0].index == mod.elements[0].elems[0].index);

   // Verify data
   REQUIRE(restored.data.size() == mod.data.size());
   CHECK(restored.data[0].data == mod.data[0].data);
   CHECK(restored.data[0].passive == mod.data[0].passive);
   CHECK(restored.data[1].passive == mod.data[1].passive);
   CHECK(restored.data[1].data == mod.data[1].data);

   // Verify tags
   REQUIRE(restored.tags.size() == mod.tags.size());
   CHECK(restored.tags[0].attribute == mod.tags[0].attribute);
   CHECK(restored.tags[0].type_index == mod.tags[0].type_index);

   // Verify derived
   CHECK(restored.start == mod.start);
   CHECK(restored.num_imported_tables == mod.num_imported_tables);
   CHECK(restored.num_imported_memories == mod.num_imported_memories);
   CHECK(restored.num_imported_globals == mod.num_imported_globals);
   CHECK(restored.import_functions.size() == 1);
   CHECK(restored.code.size() == mod.functions.size());
}

TEST_CASE("pzam_metadata: init_expr round-trip for all opcode types", "[pzam_metadata]") {
   // i32_const
   {
      init_expr ie{};
      ie.opcode = opcodes::i32_const;
      ie.value.i32 = -42;
      auto raw = serialize_init_expr(ie);
      auto restored = deserialize_init_expr(raw);
      CHECK(restored.opcode == opcodes::i32_const);
      CHECK(restored.value.i32 == -42);
   }

   // i64_const
   {
      init_expr ie{};
      ie.opcode = opcodes::i64_const;
      ie.value.i64 = 0x7FFFFFFFFFFFFFFFLL;
      auto raw = serialize_init_expr(ie);
      auto restored = deserialize_init_expr(raw);
      CHECK(restored.opcode == opcodes::i64_const);
      CHECK(restored.value.i64 == 0x7FFFFFFFFFFFFFFFLL);
   }

   // f32_const
   {
      init_expr ie{};
      ie.opcode = opcodes::f32_const;
      ie.value.f32 = 0x40490FDB;
      auto raw = serialize_init_expr(ie);
      auto restored = deserialize_init_expr(raw);
      CHECK(restored.opcode == opcodes::f32_const);
      CHECK(restored.value.f32 == 0x40490FDB);
   }

   // f64_const
   {
      init_expr ie{};
      ie.opcode = opcodes::f64_const;
      ie.value.f64 = 0x400921FB54442D18ULL;
      auto raw = serialize_init_expr(ie);
      auto restored = deserialize_init_expr(raw);
      CHECK(restored.opcode == opcodes::f64_const);
      CHECK(restored.value.f64 == 0x400921FB54442D18ULL);
   }

   // global.get
   {
      init_expr ie{};
      ie.opcode = opcodes::get_global;
      ie.value.i32 = 3;
      auto raw = serialize_init_expr(ie);
      auto restored = deserialize_init_expr(raw);
      CHECK(restored.opcode == opcodes::get_global);
      CHECK(restored.value.i32 == 3);
   }

   // ref.null
   {
      init_expr ie{};
      ie.opcode = opcodes::ref_null;
      ie.value.i64 = 0;
      auto raw = serialize_init_expr(ie, types::funcref);
      auto restored = deserialize_init_expr(raw);
      CHECK(restored.opcode == opcodes::ref_null);
      CHECK(restored.value.i64 == 0);
   }

   // ref.func
   {
      init_expr ie{};
      ie.opcode = opcodes::ref_func;
      ie.value.i32 = 7;
      auto raw = serialize_init_expr(ie);
      auto restored = deserialize_init_expr(raw);
      CHECK(restored.opcode == opcodes::ref_func);
      CHECK(restored.value.i32 == 7);
   }
}

TEST_CASE("pzam_metadata: init_expr edge cases", "[pzam_metadata]") {
   // i32 zero
   {
      init_expr ie{};
      ie.opcode = opcodes::i32_const;
      ie.value.i32 = 0;
      auto raw = serialize_init_expr(ie);
      auto restored = deserialize_init_expr(raw);
      CHECK(restored.value.i32 == 0);
   }

   // i32 max negative
   {
      init_expr ie{};
      ie.opcode = opcodes::i32_const;
      ie.value.i32 = INT32_MIN;
      auto raw = serialize_init_expr(ie);
      auto restored = deserialize_init_expr(raw);
      CHECK(restored.value.i32 == INT32_MIN);
   }

   // i64 max negative
   {
      init_expr ie{};
      ie.opcode = opcodes::i64_const;
      ie.value.i64 = INT64_MIN;
      auto raw = serialize_init_expr(ie);
      auto restored = deserialize_init_expr(raw);
      CHECK(restored.value.i64 == INT64_MIN);
   }

   // Large global index
   {
      init_expr ie{};
      ie.opcode = opcodes::get_global;
      ie.value.i32 = 12345;
      auto raw = serialize_init_expr(ie);
      auto restored = deserialize_init_expr(raw);
      CHECK(restored.value.i32 == 12345);
   }
}

TEST_CASE("pzam_metadata: full pzam_file with metadata round-trip", "[pzam_metadata]") {
   auto mod = make_test_module();
   auto meta = extract_metadata(mod);

   // Build a complete pzam_file
   pzam_file file;
   file.metadata = meta;

   pzam_code_section cs;
   cs.arch = static_cast<uint8_t>(pzam_arch::x86_64);
   cs.opt_tier = static_cast<uint8_t>(pzam_opt_tier::jit2);
   cs.max_stack = 8192;
   cs.functions = {{0, 100, 4}, {100, 200, 8}, {300, 150, 4}};
   cs.code_blob.assign(450, 0xCC);
   file.code_sections.push_back(std::move(cs));

   // Serialize and restore
   auto bytes = pzam_save(file);
   REQUIRE(pzam_validate(bytes));
   auto loaded = pzam_load(bytes);

   CHECK(loaded.magic == PZAM_MAGIC);
   CHECK(loaded.format_version == PZAM_VERSION);

   // Verify metadata survived fracpack
   REQUIRE(loaded.metadata.types.size() == 3);
   CHECK(loaded.metadata.imports.size() == 4);
   CHECK(loaded.metadata.exports.size() == 2);
   CHECK(loaded.metadata.globals.size() == 4);
   CHECK(loaded.metadata.data.size() == 2);
   CHECK(loaded.metadata.elements.size() == 1);
   CHECK(loaded.metadata.tags.size() == 1);
   CHECK(loaded.metadata.start_function == 1);
   CHECK(loaded.metadata.num_imported_functions == 1);

   // Verify code section survived
   REQUIRE(loaded.code_sections.size() == 1);
   CHECK(loaded.code_sections[0].arch == static_cast<uint8_t>(pzam_arch::x86_64));
   CHECK(loaded.code_sections[0].max_stack == 8192);
   CHECK(loaded.code_sections[0].functions.size() == 3);
   CHECK(loaded.code_sections[0].code_blob.size() == 450);

   // Full restore from metadata
   auto restored = restore_module(loaded.metadata);
   CHECK(restored.types.size() == 3);
   CHECK(restored.imports.size() == 4);
   CHECK(restored.exports.size() == 2);
   CHECK(restored.start == 1);
}
