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

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

// Opt-in phase profiler. Enable with PZAM_RUN_PROFILE=1 in the environment;
// prints a sorted breakdown of where startup time goes to stderr just before
// _start is invoked. Off by default to keep pzam-run output tidy.
class phase_profile {
   struct entry { const char* name; double ms; };
   std::vector<entry> _entries;
   std::chrono::steady_clock::time_point _anchor;
   bool _enabled;

public:
   phase_profile()
      : _anchor(std::chrono::steady_clock::now()),
        _enabled(std::getenv("PZAM_RUN_PROFILE") != nullptr) {
      if (_enabled) _entries.reserve(32);
   }

   bool enabled() const { return _enabled; }

   void mark(const char* name) {
      if (!_enabled) return;
      auto now = std::chrono::steady_clock::now();
      double ms = std::chrono::duration<double, std::milli>(now - _anchor).count();
      _entries.push_back({name, ms});
      _anchor = now;
   }

   void dump(std::FILE* out) const {
      if (!_enabled) return;
      double total = 0;
      for (auto& e : _entries) total += e.ms;
      std::fprintf(out, "[pzam-run] profile (PZAM_RUN_PROFILE=1):\n");
      for (auto& e : _entries) {
         std::fprintf(out, "  %8.2f ms  %5.1f%%  %s\n",
                      e.ms, total > 0 ? (e.ms / total * 100.0) : 0.0, e.name);
      }
      std::fprintf(out, "  --------\n");
      std::fprintf(out, "  %8.2f ms  100.0%%  TOTAL (pre-_start)\n", total);
   }
};

} // namespace

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
   phase_profile profile;

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

   profile.mark("arg parsing + preopen setup");

   // Map the .pzam file read-only. mmap + madvise(WILLNEED) lets the kernel
   // page it in lazily from the page cache; avoids the std::ifstream::read
   // copy into a heap vector (which cost ~425ms for a 168 MB file on warm
   // cache, ~400 MB/s effective) and, on cold cache, overlaps I/O with the
   // parse/relocate work below.
   int pzam_fd = ::open(pzam_file_path.c_str(), O_RDONLY | O_CLOEXEC);
   if (pzam_fd < 0) {
      std::cerr << "Error: cannot open pzam file: " << pzam_file_path << "\n";
      return 1;
   }
   struct stat st;
   if (::fstat(pzam_fd, &st) != 0) {
      perror("[pzam-run] fstat");
      ::close(pzam_fd);
      return 1;
   }
   size_t pzam_size = static_cast<size_t>(st.st_size);
   void* pzam_mapped = ::mmap(nullptr, pzam_size, PROT_READ, MAP_PRIVATE, pzam_fd, 0);
   if (pzam_mapped == MAP_FAILED) {
      perror("[pzam-run] mmap");
      ::close(pzam_fd);
      return 1;
   }
   ::close(pzam_fd);  // mapping survives close
   ::madvise(pzam_mapped, pzam_size, MADV_WILLNEED);
   std::span<const char> pzam_bytes(static_cast<const char*>(pzam_mapped), pzam_size);

   profile.mark("mmap .pzam file");

   // Parse the .pzam. from_frac already rejects malformed encodings by
   // returning an error (we get an exception via abort_error), so we skip
   // the separate pzam_validate walk that used to precede it — it was a
   // second full walk over ~168 MB for no extra safety.
   pzam_file pzam;
   try {
      pzam = pzam_load(pzam_bytes);
   } catch (const std::exception& e) {
      std::cerr << "Error: failed to parse .pzam file: " << e.what() << "\n";
      ::munmap(pzam_mapped, pzam_size);
      return 1;
   }
   if (pzam.magic != PZAM_MAGIC) {
      std::cerr << "Error: bad .pzam magic\n";
      ::munmap(pzam_mapped, pzam_size);
      return 1;
   }

   profile.mark("pzam_load (from_frac)");

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

   profile.mark("restore_module from metadata");

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

   profile.mark("wasi_host setup + register_wasi");

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

   profile.mark("build symbol table");

   // Build relocations
   std::vector<code_relocation> relocs(cs->relocations.size());
   for (size_t j = 0; j < cs->relocations.size(); j++) {
      relocs[j].code_offset = cs->relocations[j].code_offset;
      relocs[j].symbol = static_cast<reloc_symbol>(cs->relocations[j].symbol);
      relocs[j].type = static_cast<reloc_type>(cs->relocations[j].type);
      relocs[j].addend = cs->relocations[j].addend;
   }

   profile.mark("build reloc vector");

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

   profile.mark("plan veneers (aarch64)");

   // Allocate executable memory
   size_t page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
   size_t code_alloc_size = (total_code_size + page_size - 1) & ~(page_size - 1);
   auto& jit_alloc = jit_allocator::instance();
   void* exec_code = jit_alloc.alloc(code_alloc_size);

   if (mprotect(exec_code, code_alloc_size, PROT_READ | PROT_WRITE) != 0) {
      perror("[pzam-run] mprotect RW failed");
      return 1;
   }

   profile.mark("alloc + mprotect RW exec region");

   std::memcpy(exec_code, cs->code_blob.data(), cs->code_blob.size());

   profile.mark("memcpy code blob");

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

   profile.mark("write veneers + rewrite BL relocs");
#endif

   symbol_table[static_cast<uint32_t>(reloc_symbol::code_blob_self)] = exec_code;

#if defined(__aarch64__)
   // Diagnose ADRP range overflows: Small code model ADRP has ±4GB range.
   // If runtime symbols are >4GB from the code blob, the page delta overflows
   // and the ADRP computes a garbage address.
   {
      uint64_t code_base = reinterpret_cast<uint64_t>(exec_code);
      uint32_t adrp_total = 0, adrp_overflow = 0;
      for (auto& r : relocs) {
         if (r.type == reloc_type::aarch64_adr_prel_pg_hi21) {
            adrp_total++;
            void* addr = symbol_table[static_cast<uint32_t>(r.symbol)];
            uint64_t target = reinterpret_cast<uint64_t>(addr) + r.addend;
            uint64_t patch_site = code_base + r.code_offset;
            int64_t page_delta = static_cast<int64_t>((target & ~0xFFFULL) -
                                 (patch_site & ~0xFFFULL));
            int64_t pages = page_delta >> 12;
            if (pages < -(1LL << 20) || pages >= (1LL << 20)) {
               if (adrp_overflow < 5) {
                  std::cerr << "[pzam-run] ADRP OVERFLOW: reloc at code+" << std::hex
                            << r.code_offset << " sym=" << std::dec << static_cast<uint32_t>(r.symbol)
                            << " target=0x" << std::hex << target
                            << " site=0x" << patch_site
                            << " delta=" << std::dec << (page_delta >> 12) << " pages\n";
               }
               adrp_overflow++;
            }
         }
      }
      std::cerr << "[pzam-run] ADRP stats: " << adrp_total << " total, "
                << adrp_overflow << " overflows (±4GB range)\n";
   }

   profile.mark("ADRP overflow diagnostic scan");
#endif

   apply_relocations(static_cast<char*>(exec_code), relocs.data(),
                     static_cast<uint32_t>(relocs.size()), symbol_table);

   profile.mark("apply_relocations");

   if (mprotect(exec_code, code_alloc_size, PROT_READ | PROT_EXEC) != 0) {
      perror("[pzam-run] mprotect RX failed");
      return 1;
   }

   profile.mark("mprotect RX");

#if defined(__aarch64__)
   __builtin___clear_cache(static_cast<char*>(exec_code),
                           static_cast<char*>(exec_code) + total_code_size);

   profile.mark("__clear_cache (icache flush)");
#endif

   // Update module function entries based on backend type
   bool is_jit = static_cast<pzam_opt_tier>(cs->opt_tier) == pzam_opt_tier::jit1 ||
                 static_cast<pzam_opt_tier>(cs->opt_tier) == pzam_opt_tier::jit2;
   if (is_jit) {
      mod.allocator._code_base = static_cast<char*>(exec_code);
      mod.allocator._code_size = total_code_size;
      for (size_t j = 0; j < cs->functions.size(); j++) {
         mod.code[j].jit_code_offset = cs->functions[j].code_offset;
         mod.code[j].jit_code_size = cs->functions[j].code_size;
         mod.code[j].stack_size = cs->functions[j].stack_size;
      }
   } else {
      // LLVM tier: _code_base must stay null (it's the LLVM dispatch flag in
      // execution_context), but the signal handler needs the code range via
      // get_code_span() to classify faults as WASM traps vs corruption.
      mod.allocator._exec_code_base = static_cast<char*>(exec_code);
      mod.allocator._exec_code_size = total_code_size;
      auto code_base_addr = reinterpret_cast<uintptr_t>(exec_code);
      for (size_t j = 0; j < cs->functions.size(); j++) {
         mod.code[j].jit_code_offset = code_base_addr + cs->functions[j].code_offset;
         mod.code[j].jit_code_size = cs->functions[j].code_size;
         mod.code[j].stack_size = cs->functions[j].stack_size;
      }
   }
   mod.maximum_stack = cs->max_stack;
   mod.stack_limit_is_bytes = cs->stack_limit_mode != 0;

   profile.mark("populate function entries");

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

   profile.mark("fixup element-segment code_ptr");

   // Set up execution context
   wasm_allocator wa;
   jit_execution_context<> ctx(mod, 8192);
   ctx.set_wasm_allocator(&wa);
   ctx.set_host_table(&table);
   ctx.reset();

   profile.mark("wasm_allocator + execution_context setup");

   // Populate _host_trampoline_ptrs. The trampoline direction depends on how
   // the compiled .pzam packs host-call args:
   //   - jit / jit2 push WASM stack in-place so args[0] = LAST WASM param
   //     → use the `rev_trampoline` (reverse-order reader).
   //   - LLVM emits a normal C-ABI call to __psizam_call_host with args[0]
   //     = FIRST WASM param → use the forward `trampoline`.
   // Previously this code unconditionally picked rev_trampoline, which caused
   // every host-call argument to be read in reversed order on LLVM .pzam
   // files — e.g. `environ_sizes_get(count_ptr, size_ptr)` received
   // (size_ptr, count_ptr), so WASI wrote count where size was expected
   // (and vice versa). That corrupted the allocator's view of env size and
   // produced overlapping malloc regions, which ultimately OOB-crashed
   // deep in startup (issue #0016).
   const bool is_llvm_pzam =
      cs->opt_tier == static_cast<uint8_t>(pzam_opt_tier::llvm_O1) ||
      cs->opt_tier == static_cast<uint8_t>(pzam_opt_tier::llvm_O2) ||
      cs->opt_tier == static_cast<uint8_t>(pzam_opt_tier::llvm_O3);
   std::vector<host_trampoline_t> trampoline_ptrs(mod.import_functions.size());
   for (uint32_t i = 0; i < mod.import_functions.size(); i++) {
      uint32_t mapped = mod.import_functions[i];
      if (mapped < table.size()) {
         const auto& e = table.get_entry(mapped);
         trampoline_ptrs[i] = is_llvm_pzam
            ? (e.trampoline ? e.trampoline : e.rev_trampoline)
            : (e.rev_trampoline ? e.rev_trampoline : e.trampoline);
      }
   }
   ctx._host_trampoline_ptrs = trampoline_ptrs.data();

   profile.mark("build host trampoline ptrs");

#if defined(PSIZAM_JIT_SIGNAL_DIAGNOSTICS)
   // Populate jit_func_ranges for crash diagnostics — lets the signal
   // handler report "Crash in func[N] at +offset" instead of just a raw PC.
   std::vector<jit_func_range> func_ranges(cs->functions.size());
   {
      uint32_t num_imported = mod.get_imported_functions_size();
      for (size_t j = 0; j < cs->functions.size(); j++) {
         func_ranges[j] = {
            static_cast<uint32_t>(cs->functions[j].code_offset),
            static_cast<uint32_t>(cs->functions[j].code_size),
            static_cast<uint32_t>(j) + num_imported
         };
      }
      jit_func_ranges = func_ranges.data();
      jit_func_range_count = static_cast<uint32_t>(func_ranges.size());

      // Build sorted offset→index table for crash lookup (handles zero-size funcs)
      std::vector<std::pair<uint32_t, uint32_t>> sorted_offsets(func_ranges.size());
      for (size_t j = 0; j < func_ranges.size(); j++)
         sorted_offsets[j] = {func_ranges[j].offset, func_ranges[j].func_index};
      std::sort(sorted_offsets.begin(), sorted_offsets.end());
      std::cerr << "[pzam-run] func_ranges: " << func_ranges.size() << " entries"
                << " code_size=" << cs->code_blob.size() << "\n";
      // Verify function-to-code mapping by checking param count in native code
      // On aarch64 LLVM body: x0=ctx, x1=mem, w2..w(2+N-1) = N wasm params
      // After prologue, the body moves w2/w3/w4/... to callee-saved regs
      // Count MOV Wd,Wn instructions in first 20 instructions to infer param count
      uint32_t num_imp = mod.get_imported_functions_size();
      auto count_native_params = [&](uint32_t code_idx) -> int {
         uint32_t off = cs->functions[code_idx].code_offset;
         if (off + 80 > cs->code_blob.size()) return -1;
         const uint32_t* insns = reinterpret_cast<const uint32_t*>(
            cs->code_blob.data() + off);
         int max_src_reg = -1;
         for (int i = 0; i < 20; i++) {
            uint32_t insn = insns[i];
            // MOV Wd, Wn is ORR Wd, WZR, Wn: 0x2A0003E0 | (Rm<<16) | Rd
            if ((insn & 0xFFE0FFE0) == 0x2A0003E0) {
               int rm = (insn >> 16) & 0x1F;
               if (rm >= 2 && rm <= 7) {
                  if (rm > max_src_reg) max_src_reg = rm;
               }
            }
         }
         return max_src_reg >= 2 ? (max_src_reg - 1) : 0;
      };

      // Check for BL (call26) range overflow
      {
         uint64_t code_base = reinterpret_cast<uint64_t>(exec_code);
         uint32_t call26_total = 0, call26_overflow = 0;
         for (auto& r : relocs) {
            if (r.type == reloc_type::aarch64_call26) {
               call26_total++;
               void* addr = symbol_table[static_cast<uint32_t>(r.symbol)];
               int64_t target = static_cast<int64_t>(reinterpret_cast<uint64_t>(addr)) + r.addend;
               int64_t pc = static_cast<int64_t>(code_base + r.code_offset);
               int64_t offset = target - pc;
               // BL range: ±128MB (26 bits * 4 = ±2^27)
               if (offset < -(1LL << 27) || offset >= (1LL << 27)) {
                  if (call26_overflow < 5) {
                     std::cerr << "[pzam-run] BL OVERFLOW: reloc at code+0x" << std::hex
                               << r.code_offset << " sym=" << std::dec << static_cast<uint32_t>(r.symbol)
                               << " target=0x" << std::hex << (uint64_t)addr
                               << " pc=0x" << (code_base + r.code_offset)
                               << " delta=" << std::dec << offset
                               << " (" << (offset / (1024*1024)) << " MB)\n";
                  }
                  call26_overflow++;
               }
            }
         }
         std::cerr << "[pzam-run] BL stats: " << call26_total << " total, "
                   << call26_overflow << " overflows (±128MB range)\n";
      }
   }

   profile.mark("jit_func_ranges + BL diagnostic scan");
#endif

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

   std::cerr << "[pzam-run] is_llvm=" << (mod.allocator._code_base == nullptr)
             << " mem_pages=" << ctx.current_linear_memory() << "\n";

   // Find and run _start
   uint32_t start_idx = mod.get_exported_function("_start");
   {
      uint32_t num_imp = mod.get_imported_functions_size();
      uint32_t code_idx = start_idx - num_imp;
      std::cerr << "[pzam-run] _start: wasm_idx=" << start_idx << " code_idx=" << code_idx
                << " jit_code_offset=0x" << std::hex << mod.code[code_idx].jit_code_offset
                << " size=" << std::dec << mod.code[code_idx].jit_code_size
                << " mem_base=" << std::hex << (uintptr_t)ctx.linear_memory()
                << " mem_pages=" << std::dec << ctx.current_linear_memory()
                << " is_llvm=" << (mod.allocator._code_base == nullptr) << "\n";
   }

   profile.mark("page-size mismatch fixups + final prep");

   if (profile.enabled()) profile.dump(stderr);

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
