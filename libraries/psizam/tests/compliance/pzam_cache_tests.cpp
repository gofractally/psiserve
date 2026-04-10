// Tests for .pzam file format — save/load/relocation infrastructure.

#include <psizam/pzam_cache.hpp>
#include <psizam/pzam_format.hpp>
#include <psizam/jit_reloc.hpp>
#include <catch2/catch.hpp>

using namespace psizam;

TEST_CASE("pzam: header serialization roundtrip", "[pzam]") {
   pzam_header hdr;
   hdr.num_functions = 3;
   hdr.num_relocations = 2;
   hdr.code_size = 64;
   hdr.max_stack = 16;

   std::vector<pzam_func_entry> funcs = {
      {0, 20, 4, 0},
      {20, 24, 8, 0},
      {44, 20, 4, 0},
   };

   std::vector<code_relocation> relocs = {
      {8, reloc_symbol::call_host_function, 0},
      {32, reloc_symbol::current_memory, 0},
   };

   std::vector<uint8_t> code_blob(64, 0x90); // NOP fill

   auto bytes = pzam_save(hdr, funcs, relocs, code_blob);

   // Parse it back
   pzam_parsed parsed;
   REQUIRE(pzam_parse(bytes, parsed));

   CHECK(parsed.header->magic == PZAM_MAGIC);
   CHECK(parsed.header->format_version == PZAM_VERSION);
   CHECK(parsed.header->num_functions == 3);
   CHECK(parsed.header->num_relocations == 2);
   CHECK(parsed.header->code_size == 64);
   CHECK(parsed.header->max_stack == 16);

   REQUIRE(parsed.funcs.size() == 3);
   CHECK(parsed.funcs[0].code_offset == 0);
   CHECK(parsed.funcs[0].code_size == 20);
   CHECK(parsed.funcs[1].code_offset == 20);
   CHECK(parsed.funcs[2].code_offset == 44);

   REQUIRE(parsed.relocs.size() == 2);
   CHECK(parsed.relocs[0].code_offset == 8);
   CHECK(parsed.relocs[0].symbol == reloc_symbol::call_host_function);
   CHECK(parsed.relocs[1].code_offset == 32);
   CHECK(parsed.relocs[1].symbol == reloc_symbol::current_memory);

   REQUIRE(parsed.code_blob.size() == 64);
   CHECK(parsed.code_blob[0] == 0x90);
}

TEST_CASE("pzam: header validation", "[pzam]") {
   auto hash1 = pzam_cache::hash_wasm(std::vector<uint8_t>{1, 2, 3});
   auto hash2 = pzam_cache::hash_wasm(std::vector<uint8_t>{1, 2, 4});
   auto compiler = pzam_cache::compiler_identity();

   // Same input produces same hash
   auto hash1b = pzam_cache::hash_wasm(std::vector<uint8_t>{1, 2, 3});
   CHECK(hash1 == hash1b);

   // Different input produces different hash
   CHECK(hash1 != hash2);

   // Validation passes with correct hashes
   pzam_header hdr;
#if defined(__x86_64__)
   hdr.arch = pzam_arch::x86_64;
#elif defined(__aarch64__)
   hdr.arch = pzam_arch::aarch64;
#endif
   hdr.input_hash = hash1;
   hdr.compiler_hash = compiler;
   CHECK(pzam_validate_header(hdr, hash1, compiler));

   // Validation fails with wrong input hash
   CHECK_FALSE(pzam_validate_header(hdr, hash2, compiler));

   // Validation fails with wrong magic
   pzam_header bad_hdr = hdr;
   bad_hdr.magic = 0xDEADBEEF;
   CHECK_FALSE(pzam_validate_header(bad_hdr, hash1, compiler));
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
      {0, reloc_symbol::call_host_function, 0},
      {16, reloc_symbol::current_memory, 0},
   };

   apply_relocations(code, relocs, 2, symbol_table);

   void* patched1;
   void* patched2;
   std::memcpy(&patched1, code + 0, 8);
   std::memcpy(&patched2, code + 16, 8);

   CHECK(patched1 == addr1);
   CHECK(patched2 == addr2);
}

TEST_CASE("pzam: truncated buffer rejected", "[pzam]") {
   std::vector<uint8_t> too_small(10, 0);
   pzam_parsed parsed;
   CHECK_FALSE(pzam_parse(too_small, parsed));
}
