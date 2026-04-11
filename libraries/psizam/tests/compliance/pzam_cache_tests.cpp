// Tests for .pzam file format — save/load/relocation infrastructure.

#include <psizam/pzam_cache.hpp>
#include <psizam/pzam_format.hpp>
#include <psizam/jit_reloc.hpp>
#include <catch2/catch.hpp>

using namespace psizam;

TEST_CASE("pzam: fracpack serialization roundtrip", "[pzam]") {
   pzam_file file;
   file.max_stack = 16;

   file.functions = {
      {0, 20, 4},
      {20, 24, 8},
      {44, 20, 4},
   };

   file.relocations = {
      {8, static_cast<uint16_t>(reloc_symbol::call_host_function)},
      {32, static_cast<uint16_t>(reloc_symbol::current_memory)},
   };

   file.code_blob.assign(64, 0x90); // NOP fill

   auto bytes = pzam_save(file);

   // Parse it back
   REQUIRE(pzam_validate(bytes));
   auto loaded = pzam_load(bytes);

   CHECK(loaded.magic == PZAM_MAGIC);
   CHECK(loaded.format_version == PZAM_VERSION);
   CHECK(loaded.max_stack == 16);

   REQUIRE(loaded.functions.size() == 3);
   CHECK(loaded.functions[0].code_offset == 0);
   CHECK(loaded.functions[0].code_size == 20);
   CHECK(loaded.functions[1].code_offset == 20);
   CHECK(loaded.functions[2].code_offset == 44);

   REQUIRE(loaded.relocations.size() == 2);
   CHECK(loaded.relocations[0].code_offset == 8);
   CHECK(loaded.relocations[0].symbol == static_cast<uint16_t>(reloc_symbol::call_host_function));
   CHECK(loaded.relocations[1].code_offset == 32);
   CHECK(loaded.relocations[1].symbol == static_cast<uint16_t>(reloc_symbol::current_memory));

   REQUIRE(loaded.code_blob.size() == 64);
   CHECK(loaded.code_blob[0] == 0x90);
}

TEST_CASE("pzam: hash validation", "[pzam]") {
   auto hash1 = pzam_cache::hash_wasm(std::vector<uint8_t>{1, 2, 3});
   auto hash2 = pzam_cache::hash_wasm(std::vector<uint8_t>{1, 2, 4});

   // Same input produces same hash
   auto hash1b = pzam_cache::hash_wasm(std::vector<uint8_t>{1, 2, 3});
   CHECK(hash1 == hash1b);

   // Different input produces different hash
   CHECK(hash1 != hash2);

   // compiler_identity with same arch produces same result
   auto c1 = pzam_cache::compiler_identity(pzam_arch::x86_64);
   auto c2 = pzam_cache::compiler_identity(pzam_arch::x86_64);
   CHECK(c1 == c2);

   // Different arch produces different compiler identity
   auto c3 = pzam_cache::compiler_identity(pzam_arch::aarch64);
   CHECK(c1 != c3);
}

TEST_CASE("pzam: relocation recording", "[pzam]") {
   relocation_recorder rec;

   rec.record(0, reloc_symbol::call_host_function);
   rec.record(16, reloc_symbol::current_memory);
   rec.record(32, reloc_symbol::grow_memory);

   CHECK(rec.size() == 3);
   CHECK(rec.entries()[0].code_offset == 0);
   CHECK(rec.entries()[0].symbol == reloc_symbol::call_host_function);
   CHECK(rec.entries()[1].code_offset == 16);
   CHECK(rec.entries()[1].symbol == reloc_symbol::current_memory);
   CHECK(rec.entries()[2].code_offset == 32);
   CHECK(rec.entries()[2].symbol == reloc_symbol::grow_memory);
}

TEST_CASE("pzam: apply relocations", "[pzam]") {
   // Simulate a code blob with two 8-byte slots for absolute addresses
   char code[32] = {};

   // Dummy function addresses
   void* addr1 = reinterpret_cast<void*>(0x1234567890ABCDEFULL);
   void* addr2 = reinterpret_cast<void*>(0xFEDCBA0987654321ULL);

   void* symbol_table[static_cast<uint32_t>(reloc_symbol::NUM_SYMBOLS)] = {};
   symbol_table[static_cast<uint32_t>(reloc_symbol::call_host_function)] = addr1;
   symbol_table[static_cast<uint32_t>(reloc_symbol::current_memory)] = addr2;

   code_relocation relocs[] = {
      {0, reloc_symbol::call_host_function, reloc_type::abs64, 0, 0},
      {16, reloc_symbol::current_memory, reloc_type::abs64, 0, 0},
   };

   apply_relocations(code, relocs, 2, symbol_table);

   void* patched1;
   void* patched2;
   std::memcpy(&patched1, code + 0, 8);
   std::memcpy(&patched2, code + 16, 8);

   CHECK(patched1 == addr1);
   CHECK(patched2 == addr2);
}

TEST_CASE("pzam: invalid data rejected", "[pzam]") {
   std::vector<char> too_small(10, 0);
   CHECK_FALSE(pzam_validate(too_small));
}
