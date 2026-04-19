// pzam-run: Load and execute a pre-compiled .pzam native code module.
//
// Usage: pzam-run [--dir=guest:host ...] <module.pzam> [-- args...]
//
// Loads module metadata and native code from a self-contained .pzam v3 file,
// applies relocations, and executes the _start function.
// No .wasm file is needed — all metadata is embedded in the .pzam.

#include <psizam/backend.hpp>
#include <psizam/detail/llvm_runtime_helpers.hpp>
#include <psizam/pzam_cache.hpp>
#include <psizam/pzam_format.hpp>
#include <psizam/pzam_metadata.hpp>
#include <psizam/detail/wasi_host.hpp>

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
using namespace psizam::detail;

int pzam_run_main(int argc, char** argv);

#ifdef PZAM_STANDALONE_RUN
int main(int argc, char** argv) { return pzam_run_main(argc, argv); }
#endif

int pzam_run_main(int argc, char** argv) {
   if (argc < 2) {
      std::cerr << "Usage: pzam-run [--dir=guest:host ...] <module.pzam> [-- args...]\n";
      return 1;
   }

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
      std::cerr << "Error: no pzam file specified\n";
      return 1;
   }
   pzam_file_path = argv[i++];

   // Collect args (argv[0] = pzam filename)
   wasm_args.push_back(pzam_file_path);
   for (; i < argc; i++)
      wasm_args.push_back(argv[i]);

   // Default preopens
   if (dirs.empty())
      dirs.push_back({".", "."});

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

   // Find a code section matching this platform's architecture
   auto expected_arch =
#if defined(__x86_64__)
      pzam_arch::x86_64;
#elif defined(__aarch64__)
      pzam_arch::aarch64;
#else
      pzam_arch{};
#endif

   const pzam_code_section* cs = nullptr;
   for (const auto& section : pzam.code_sections) {
      if (static_cast<pzam_arch>(section.arch) == expected_arch) {
         cs = &section;
         break;
      }
   }
   if (!cs) {
      std::cerr << "Error: no code section for architecture "
                << (expected_arch == pzam_arch::x86_64 ? "x86_64" : "aarch64") << "\n";
      return 1;
   }

   // Restore module from embedded metadata — no .wasm needed
   module mod = restore_module(pzam.metadata);
   mod.allocator.use_default_memory();

   if (cs->functions.size() != mod.code.size()) {
      std::cerr << "Error: code section function count (" << cs->functions.size()
                << ") doesn't match metadata (" << mod.code.size() << ")\n";
      return 1;
   }

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
   table.resolve(mod);

   // Build symbol table for relocation
   void* symbol_table[static_cast<size_t>(reloc_symbol::NUM_SYMBOLS)];
#if defined(__aarch64__)
   std::memset(symbol_table, 0, sizeof(symbol_table));
   using jit_cg = jit_codegen_a64;
   symbol_table[static_cast<uint32_t>(reloc_symbol::call_host_function)]     = reinterpret_cast<void*>(&jit_cg::call_host_function);
   symbol_table[static_cast<uint32_t>(reloc_symbol::current_memory)]         = reinterpret_cast<void*>(&jit_cg::current_memory);
   symbol_table[static_cast<uint32_t>(reloc_symbol::grow_memory)]            = reinterpret_cast<void*>(&jit_cg::grow_memory);
   symbol_table[static_cast<uint32_t>(reloc_symbol::memory_fill)]            = reinterpret_cast<void*>(&jit_cg::memory_fill_impl);
   symbol_table[static_cast<uint32_t>(reloc_symbol::memory_copy)]            = reinterpret_cast<void*>(&jit_cg::memory_copy_impl);
   symbol_table[static_cast<uint32_t>(reloc_symbol::memory_init)]            = reinterpret_cast<void*>(&jit_cg::memory_init_impl);
   symbol_table[static_cast<uint32_t>(reloc_symbol::data_drop)]              = reinterpret_cast<void*>(&jit_cg::data_drop_impl);
   symbol_table[static_cast<uint32_t>(reloc_symbol::table_init)]             = reinterpret_cast<void*>(&jit_cg::table_init_impl);
   symbol_table[static_cast<uint32_t>(reloc_symbol::elem_drop)]              = reinterpret_cast<void*>(&jit_cg::elem_drop_impl);
   symbol_table[static_cast<uint32_t>(reloc_symbol::table_copy)]             = reinterpret_cast<void*>(&jit_cg::table_copy_impl);
   symbol_table[static_cast<uint32_t>(reloc_symbol::on_unreachable)]         = reinterpret_cast<void*>(&jit_cg::on_unreachable);
   symbol_table[static_cast<uint32_t>(reloc_symbol::on_fp_error)]            = reinterpret_cast<void*>(&jit_cg::on_fp_error);
   symbol_table[static_cast<uint32_t>(reloc_symbol::on_memory_error)]        = reinterpret_cast<void*>(&jit_cg::on_memory_error);
   symbol_table[static_cast<uint32_t>(reloc_symbol::on_call_indirect_error)] = reinterpret_cast<void*>(&jit_cg::on_call_indirect_error);
   symbol_table[static_cast<uint32_t>(reloc_symbol::on_type_error)]          = reinterpret_cast<void*>(&jit_cg::on_type_error);
   symbol_table[static_cast<uint32_t>(reloc_symbol::on_stack_overflow)]      = reinterpret_cast<void*>(&jit_cg::on_stack_overflow);
#else
   build_symbol_table<jit_codegen>(symbol_table);
#endif

   // Overlay LLVM runtime symbols
   build_llvm_symbol_table(symbol_table);

   // Build relocations
   std::vector<code_relocation> relocs(cs->relocations.size());
   for (size_t j = 0; j < cs->relocations.size(); j++) {
      relocs[j].code_offset = cs->relocations[j].code_offset;
      relocs[j].symbol = static_cast<reloc_symbol>(cs->relocations[j].symbol);
      relocs[j].type = static_cast<reloc_type>(cs->relocations[j].type);
      relocs[j].addend = cs->relocations[j].addend;
   }

#if defined(__aarch64__)
   // On aarch64, BL instructions have +-128MB range. Generate veneers for
   // external symbols that may be out of range.
   std::unordered_map<uint16_t, uint32_t> veneer_offsets;
   size_t veneer_start = (cs->code_blob.size() + 3) & ~size_t(3);

   for (auto& r : relocs) {
      if (r.type == reloc_type::aarch64_call26 &&
          r.symbol != reloc_symbol::code_blob_self) {
         auto sym_idx = static_cast<uint16_t>(r.symbol);
         if (veneer_offsets.find(sym_idx) == veneer_offsets.end()) {
            size_t off = veneer_start + veneer_offsets.size() * 20;
            veneer_offsets[sym_idx] = static_cast<uint32_t>(off);
         }
      }
   }
   size_t total_code_size = veneer_start + veneer_offsets.size() * 20;
#else
   size_t total_code_size = cs->code_blob.size();
#endif

   // Allocate executable memory
   size_t page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
   size_t code_alloc_size = (total_code_size + page_size - 1) & ~(page_size - 1);
   auto& jit_alloc = jit_allocator::instance();
   void* exec_code = jit_alloc.alloc(code_alloc_size);

   if (mprotect(exec_code, code_alloc_size, PROT_READ | PROT_WRITE) != 0) {
      perror("[pzam-run] mprotect RW failed");
      return 1;
   }
   std::memcpy(exec_code, cs->code_blob.data(), cs->code_blob.size());

#if defined(__aarch64__)
   // Write veneers
   for (auto& [sym_idx, veneer_off] : veneer_offsets) {
      uint64_t target = reinterpret_cast<uint64_t>(symbol_table[sym_idx]);
      uint32_t* v = reinterpret_cast<uint32_t*>(static_cast<char*>(exec_code) + veneer_off);
      v[0] = 0xD2800010u | ((static_cast<uint32_t>(target >>  0) & 0xFFFF) << 5);
      v[1] = 0xF2A00010u | ((static_cast<uint32_t>(target >> 16) & 0xFFFF) << 5);
      v[2] = 0xF2C00010u | ((static_cast<uint32_t>(target >> 32) & 0xFFFF) << 5);
      v[3] = 0xF2E00010u | ((static_cast<uint32_t>(target >> 48) & 0xFFFF) << 5);
      v[4] = 0xD61F0200u;
   }

   for (auto& r : relocs) {
      if (r.type == reloc_type::aarch64_call26 &&
          r.symbol != reloc_symbol::code_blob_self) {
         auto sym_idx = static_cast<uint16_t>(r.symbol);
         auto it = veneer_offsets.find(sym_idx);
         if (it != veneer_offsets.end()) {
            r.symbol = reloc_symbol::code_blob_self;
            r.addend = static_cast<int32_t>(it->second);
         }
      }
   }
#endif

   symbol_table[static_cast<uint32_t>(reloc_symbol::code_blob_self)] = exec_code;

   apply_relocations(static_cast<char*>(exec_code), relocs.data(),
                     static_cast<uint32_t>(relocs.size()), symbol_table);

   if (mprotect(exec_code, code_alloc_size, PROT_READ | PROT_EXEC) != 0) {
      perror("[pzam-run] mprotect RX failed");
      return 1;
   }
#if defined(__aarch64__)
   __builtin___clear_cache(static_cast<char*>(exec_code),
                           static_cast<char*>(exec_code) + total_code_size);
#endif

   // Update module function entries based on backend type
   bool is_jit = static_cast<pzam_opt_tier>(cs->opt_tier) == pzam_opt_tier::jit1 ||
                 static_cast<pzam_opt_tier>(cs->opt_tier) == pzam_opt_tier::jit2;
   if (is_jit) {
      mod.allocator._code_base = static_cast<char*>(exec_code);
      // _code_size feeds growable_allocator::get_code_span(), which the
      // signal handler reads to decide whether a fault is JIT code (→ wasm
      // trap) or a corrupted PC (→ "control-flow corruption"). Without this,
      // any SIGSEGV inside pzam-run's JIT code looks unrecognized.
      mod.allocator._code_size = total_code_size;
      for (size_t j = 0; j < cs->functions.size(); j++) {
         mod.code[j].jit_code_offset = cs->functions[j].code_offset;
         mod.code[j].jit_code_size = cs->functions[j].code_size;
         mod.code[j].stack_size = cs->functions[j].stack_size;
      }
   } else {
      auto code_base_addr = reinterpret_cast<uintptr_t>(exec_code);
      for (size_t j = 0; j < cs->functions.size(); j++) {
         mod.code[j].jit_code_offset = code_base_addr + cs->functions[j].code_offset;
         mod.code[j].jit_code_size = cs->functions[j].code_size;
         mod.code[j].stack_size = cs->functions[j].stack_size;
      }
   }
   mod.maximum_stack = cs->max_stack;
   mod.stack_limit_is_bytes = cs->stack_limit_mode != 0;

   // Fix up element segment code_ptr fields for JIT dispatch
   if (is_jit) {
      uint32_t num_imports = mod.get_imported_functions_size();
      for (auto& elem_seg : mod.elements) {
         for (auto& entry : elem_seg.elems) {
            if (entry.index < num_imports + cs->functions.size()) {
               uint32_t code_idx = entry.index - num_imports;
               if (entry.index >= num_imports && code_idx < cs->functions.size()) {
                  entry.code_ptr = mod.allocator._code_base + cs->functions[code_idx].code_offset;
               }
            }
         }
      }
   }

   // Set up execution context
   wasm_allocator wa;
   jit_execution_context<> ctx(mod, 8192);
   ctx.set_wasm_allocator(&wa);
   ctx.set_host_table(&table);
   ctx.reset();

   // Populate _host_trampoline_ptrs with reverse-order trampolines. jit/jit2
   // pack args on the WASM operand stack with args[0] = last param, and
   // call_host_function forwards that buffer verbatim to the trampoline —
   // so the trampoline must be the rev variant. backend<>::construct() does
   // this when reverse_host_args=true; pzam-run skips that path, so do it
   // here by hand. (Falling through to host_function_table::call() would
   // pick up the forward trampoline and read every arg swapped.)
   std::vector<host_trampoline_t> trampoline_ptrs(mod.import_functions.size());
   for (uint32_t i = 0; i < mod.import_functions.size(); i++) {
      uint32_t mapped = mod.import_functions[i];
      if (mapped < table.size()) {
         const auto& e = table.get_entry(mapped);
         trampoline_ptrs[i] = e.rev_trampoline ? e.rev_trampoline : e.trampoline;
      }
   }
   ctx._host_trampoline_ptrs = trampoline_ptrs.data();

   // Handle page_size mismatch between compile-time and runtime
   if (is_jit && cs->page_size != 0) {
      uint32_t compile_ps = cs->page_size;
      uint32_t runtime_ps = static_cast<uint32_t>(wasm_allocator::table_size());
      if (compile_ps != runtime_ps) {
         char* linear_memory = wa.get_base_ptr<char>();

         char* guard_page = linear_memory - runtime_ps;
         mprotect(guard_page, runtime_ps, PROT_READ | PROT_WRITE);

         int32_t compile_globals_off = -static_cast<int32_t>(compile_ps) - static_cast<int32_t>(sizeof(void*));
         int32_t runtime_globals_off = wasm_allocator::globals_end() - static_cast<int32_t>(sizeof(void*));
         void* globals_ptr;
         std::memcpy(&globals_ptr, linear_memory + runtime_globals_off, sizeof(void*));
         std::memcpy(linear_memory + compile_globals_off, &globals_ptr, sizeof(void*));

         int32_t compile_table_off = -static_cast<int32_t>(2 * compile_ps);
         int32_t runtime_table_off = wasm_allocator::table_offset();
         if (compile_table_off != runtime_table_off && !mod.tables.empty()) {
            uint32_t tsize = mod.tables[0].limits.initial;
            size_t table_bytes = tsize * sizeof(table_entry);
            char* runtime_table_loc = linear_memory + runtime_table_off;
            char* src;
            if (mod.indirect_table(0)) {
               std::memcpy(&src, runtime_table_loc, sizeof(src));
            } else {
               src = runtime_table_loc;
            }
            std::memcpy(linear_memory + compile_table_off, src, table_bytes);
         }

         mprotect(guard_page, runtime_ps, PROT_READ);
      }
   }

   // Find and run _start
   uint32_t start_idx = mod.get_exported_function("_start");

   try {
      if (mod.start != std::numeric_limits<uint32_t>::max()) {
         ctx.execute(&wasi, jit_visitor{nullptr}, mod.start);
      }
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
