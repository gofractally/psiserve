// pzam-run: Load and execute a pre-compiled .pzam native code module.
//
// Usage: pzam-run [--dir=guest:host ...] <module.wasm> <module.pzam> [args...]
//
// Reads a .wasm file for module metadata (types, imports, data segments, etc.),
// loads the native code from a .pzam file, applies relocations, and executes
// the _start function. No interpretation or compilation happens at runtime.
//
// This completes the bootstrapping loop:
//   1. pzam-compile.wasm compiles input.wasm → output.pzam (inside WASM sandbox)
//   2. pzam-run loads output.pzam and runs native code directly (no WASM overhead)

#include <psizam/backend.hpp>
#include <psizam/llvm_runtime_helpers.hpp>
#include <psizam/pzam_cache.hpp>
#include <psizam/pzam_format.hpp>
#include <psizam/wasi_host.hpp>

#include <sys/mman.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <crt_externs.h>
#define environ (*_NSGetEnviron())
#endif

using namespace psizam;

int main(int argc, char** argv) {
   if (argc < 3) {
      std::cerr << "Usage: pzam-run [--dir=guest:host ...] <module.wasm> <module.pzam> [args...]\n";
      return 1;
   }

   std::string wasm_file;
   std::string pzam_file_path;
   std::vector<std::pair<std::string, std::string>> dirs;
   std::vector<std::string> wasm_args;

   // Parse options
   int i = 1;
   for (; i < argc; i++) {
      std::string arg = argv[i];
      if (arg.starts_with("--dir=")) {
         auto val = arg.substr(6);
         auto colon = val.find(':');
         if (colon != std::string::npos)
            dirs.push_back({val.substr(0, colon), val.substr(colon + 1)});
         else
            dirs.push_back({val, val});
      } else if (arg == "--") {
         i++;
         break;
      } else if (!arg.starts_with("-")) {
         break;
      } else {
         std::cerr << "Unknown option: " << arg << "\n";
         return 1;
      }
   }

   if (i >= argc) {
      std::cerr << "Error: no wasm file specified\n";
      return 1;
   }
   wasm_file = argv[i++];

   if (i >= argc) {
      std::cerr << "Error: no pzam file specified\n";
      return 1;
   }
   pzam_file_path = argv[i++];

   // Collect wasm args (argv[0] = wasm filename)
   wasm_args.push_back(wasm_file);
   for (; i < argc; i++)
      wasm_args.push_back(argv[i]);

   // Default preopens
   if (dirs.empty())
      dirs.push_back({".", "."});

   // Read the WASM module (for metadata: types, imports, data segments, etc.)
   auto wasm_bytes = read_wasm(wasm_file);

   // Read the .pzam file
   std::ifstream pzam_in(pzam_file_path, std::ios::binary | std::ios::ate);
   if (!pzam_in.is_open()) {
      std::cerr << "Error: cannot open pzam file: " << pzam_file_path << "\n";
      return 1;
   }
   auto pzam_size = pzam_in.tellg();
   pzam_in.seekg(0);
   std::vector<char> pzam_data(pzam_size);
   pzam_in.read(pzam_data.data(), pzam_size);

   // Set up WASI host
   wasi_host wasi;
   wasi.args = std::move(wasm_args);
   if (environ) {
      for (char** e = environ; *e; e++)
         wasi.env.push_back(*e);
   }
   for (auto& [guest, host] : dirs)
      wasi.add_preopen(guest, host);

   // Set up host function table with WASI functions
   host_function_table table;
   register_wasi(table);

   // Parse the WASM module for metadata (no code generation)
   module mod;
   mod.allocator.use_default_memory();
   null_debug_info debug;
   using parser_t = binary_parser<null_writer, default_options, null_debug_info>;
   parser_t parser(mod.allocator, default_options{}, false, false);

   try {
      parser.parse_module(wasm_bytes, mod, debug);
   } catch (const psizam::exception& e) {
      std::cerr << "WASM parse error: " << e.what() << " : " << e.detail() << "\n";
      return 1;
   }
   mod.finalize();

   // Resolve WASI imports
   table.resolve(mod);

   // Build LLVM symbol table for relocation
   // code_blob_self is set after we know the code base address (below)
   void* symbol_table[static_cast<size_t>(reloc_symbol::NUM_SYMBOLS)];
   build_llvm_symbol_table(symbol_table);

   // Load and validate .pzam file
   if (!pzam_validate(pzam_data)) {
      std::cerr << "Error: invalid .pzam file\n";
      return 1;
   }
   pzam_file pzam;
   try {
      pzam = pzam_load(pzam_data);
   } catch (const std::exception& e) {
      std::cerr << "Error: failed to parse .pzam file: " << e.what() << "\n";
      return 1;
   }
   if (pzam.magic != PZAM_MAGIC) {
      std::cerr << "Error: bad .pzam magic\n";
      return 1;
   }

   // Validate architecture matches this platform
   auto expected_arch =
#if defined(__x86_64__)
      pzam_arch::x86_64;
#elif defined(__aarch64__)
      pzam_arch::aarch64;
#else
      pzam_arch{};
#endif
   if (static_cast<pzam_arch>(pzam.arch) != expected_arch) {
      std::cerr << "Error: .pzam architecture mismatch (expected "
                << (expected_arch == pzam_arch::x86_64 ? "x86_64" : "aarch64") << ")\n";
      return 1;
   }

   if (pzam.functions.size() != mod.code.size()) {
      std::cerr << "Error: .pzam function count (" << pzam.functions.size()
                << ") doesn't match .wasm (" << mod.code.size() << ")\n";
      return 1;
   }

   // Build relocations
   std::vector<code_relocation> relocs(pzam.relocations.size());
   for (size_t j = 0; j < pzam.relocations.size(); j++) {
      relocs[j].code_offset = pzam.relocations[j].code_offset;
      relocs[j].symbol = static_cast<reloc_symbol>(pzam.relocations[j].symbol);
      relocs[j].type = static_cast<reloc_type>(pzam.relocations[j].type);
      relocs[j].addend = pzam.relocations[j].addend;
   }

#if defined(__aarch64__)
   // On aarch64, BL instructions have ±128MB range. If the code blob is loaded
   // far from runtime helpers, BL won't reach. Generate veneers (trampolines)
   // at the end of the code blob that load the full 64-bit address and branch.
   // Each veneer: MOVZ x16, #g0 / MOVK x16, #g1, LSL#16 / MOVK x16, #g2, LSL#32
   //              / MOVK x16, #g3, LSL#48 / BR x16  = 20 bytes
   // We collect unique external symbols used in call26 relocations.
   std::unordered_map<uint16_t, uint32_t> veneer_offsets; // symbol → veneer offset in blob
   size_t veneer_start = (pzam.code_blob.size() + 3) & ~size_t(3); // 4-byte align for ARM64

   // Count unique external symbols needing veneers
   for (auto& r : relocs) {
      if (r.type == reloc_type::aarch64_call26 &&
          r.symbol != reloc_symbol::code_blob_self) {
         auto sym_idx = static_cast<uint16_t>(r.symbol);
         if (veneer_offsets.find(sym_idx) == veneer_offsets.end()) {
            // Align veneer start to 4 bytes
            size_t off = veneer_start + veneer_offsets.size() * 20;
            veneer_offsets[sym_idx] = static_cast<uint32_t>(off);
         }
      }
   }
   size_t total_code_size = veneer_start + veneer_offsets.size() * 20;
#else
   size_t total_code_size = pzam.code_blob.size();
#endif

   // Allocate executable memory, copy code, generate veneers, apply relocations
   size_t page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
   size_t code_alloc_size = (total_code_size + page_size - 1) & ~(page_size - 1);
   auto& jit_alloc = jit_allocator::instance();
   void* exec_code = jit_alloc.alloc(code_alloc_size);

   // Make writable for copying + relocation patching
   if (mprotect(exec_code, code_alloc_size, PROT_READ | PROT_WRITE) != 0) {
      perror("[pzam-run] mprotect RW failed");
      return 1;
   }
   std::memcpy(exec_code, pzam.code_blob.data(), pzam.code_blob.size());

#if defined(__aarch64__)
   // Write veneer instructions
   for (auto& [sym_idx, veneer_off] : veneer_offsets) {
      uint64_t target = reinterpret_cast<uint64_t>(symbol_table[sym_idx]);
      uint32_t* v = reinterpret_cast<uint32_t*>(static_cast<char*>(exec_code) + veneer_off);
      // MOVZ X16, #imm16         (bits 0-15)
      v[0] = 0xD2800010u | ((static_cast<uint32_t>(target >>  0) & 0xFFFF) << 5);
      // MOVK X16, #imm16, LSL#16 (bits 16-31)
      v[1] = 0xF2A00010u | ((static_cast<uint32_t>(target >> 16) & 0xFFFF) << 5);
      // MOVK X16, #imm16, LSL#32 (bits 32-47)
      v[2] = 0xF2C00010u | ((static_cast<uint32_t>(target >> 32) & 0xFFFF) << 5);
      // MOVK X16, #imm16, LSL#48 (bits 48-63)
      v[3] = 0xF2E00010u | ((static_cast<uint32_t>(target >> 48) & 0xFFFF) << 5);
      // BR X16
      v[4] = 0xD61F0200u;
   }

   // Redirect call26 relocations to veneers (they're always in range since
   // veneers are at the end of the same code blob)
   for (auto& r : relocs) {
      if (r.type == reloc_type::aarch64_call26 &&
          r.symbol != reloc_symbol::code_blob_self) {
         auto sym_idx = static_cast<uint16_t>(r.symbol);
         auto it = veneer_offsets.find(sym_idx);
         if (it != veneer_offsets.end()) {
            // Rewrite as a code_blob_self call26 targeting the veneer
            r.symbol = reloc_symbol::code_blob_self;
            r.addend = static_cast<int32_t>(it->second);
         }
      }
   }
#endif

   // Set code_blob_self symbol to the code base address for internal data references
   symbol_table[static_cast<uint32_t>(reloc_symbol::code_blob_self)] = exec_code;

   // Apply relocations
   apply_relocations(static_cast<char*>(exec_code), relocs.data(),
                     static_cast<uint32_t>(relocs.size()), symbol_table);

   // Make executable and flush instruction cache
   if (mprotect(exec_code, code_alloc_size, PROT_READ | PROT_EXEC) != 0) {
      perror("[pzam-run] mprotect RX failed");
      return 1;
   }
#if defined(__aarch64__)
   // ARM64 has non-coherent I/D caches — must invalidate after writing code
   __builtin___clear_cache(static_cast<char*>(exec_code),
                           static_cast<char*>(exec_code) + total_code_size);
#endif

   // Update module function entries with absolute addresses
   // LLVM path: _code_base stays null, jit_code_offset = absolute address
   auto code_base_addr = reinterpret_cast<uintptr_t>(exec_code);
   for (size_t j = 0; j < pzam.functions.size(); j++) {
      mod.code[j].jit_code_offset = code_base_addr + pzam.functions[j].code_offset;
      mod.code[j].jit_code_size = pzam.functions[j].code_size;
      mod.code[j].stack_size = pzam.functions[j].stack_size;
   }
   mod.maximum_stack = pzam.max_stack;
   mod.stack_limit_is_bytes = pzam.opts.stack_limit_is_bytes;


   // Set up execution context
   wasm_allocator wa;
   jit_execution_context<> ctx(mod, 8192);
   ctx.set_wasm_allocator(&wa);
   ctx.set_host_table(&table);
   ctx.reset();

   // Run _start (WASI entry point) — also handles module start function
   try {
      // Module start function (if any)
      if (mod.start != std::numeric_limits<uint32_t>::max()) {
         ctx.execute(&wasi, jit_visitor{nullptr}, mod.start);
      }

      uint32_t start_idx = mod.get_exported_function("_start");
      ctx.execute(&wasi, jit_visitor{nullptr}, start_idx);
   } catch (const wasi_host::wasi_exit_exception& e) {
      return e.code;
   } catch (const psizam::exception& e) {
      std::cerr << "psizam error: " << e.what() << " : " << e.detail() << "\n";
      return 1;
   } catch (const std::exception& e) {
      std::cerr << "Error: " << e.what() << "\n";
      return 1;
   } catch (...) {
      std::cerr << "Unknown exception\n";
      return 1;
   }

   return wasi.exit_code;
}
