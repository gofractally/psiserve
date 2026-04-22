#include <psizam/backend.hpp>
#include <psizam/debug/debug.hpp>
#include <psizam/host_function.hpp>
#include <catch2/catch.hpp>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include "utils.hpp"

using namespace psizam;

namespace {
// Minimal WASM module with two exported functions.
// (module
//   (func (export "f0") (result i32) i32.const 42)
//   (func (export "f1") (result i32) i32.const 7))
const std::vector<uint8_t> kTwoFuncs = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,  // magic + version
    0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,         // type: () -> i32
    0x03, 0x03, 0x02, 0x00, 0x00,                     // funcs: 2 of type 0
    0x07, 0x0b, 0x02,                                 // exports: 2 entries
       0x02, 0x66, 0x30, 0x00, 0x00,                  //   "f0" -> func 0
       0x02, 0x66, 0x31, 0x00, 0x01,                  //   "f1" -> func 1
    0x0a, 0x0b, 0x02,                                 // code: 2 bodies
       0x04, 0x00, 0x41, 0x2a, 0x0b,                  //   body 0: len=4, 0 locals, i32.const 42, end
       0x04, 0x00, 0x41, 0x07, 0x0b,                  //   body 1: len=4, 0 locals, i32.const 7,  end
};
}

BACKEND_TEST_CASE("debug_instr_map populates fn_locs + instr_locs",
                  "[debug]") {
   auto code = kTwoFuncs;
   using backend_t = backend<std::nullptr_t, TestType, default_options,
                             psizam::debug::debug_instr_map>;
   backend_t bkend(code, &wa);

   auto& dbg = bkend.get_debug();

   REQUIRE(dbg.locs.fn_locs.size() == 2);

   // Per-function invariants populated during codegen.
   for (const auto& fn : dbg.locs.fn_locs) {
      CHECK(fn.code_prologue <= fn.code_end);
      CHECK(fn.wasm_begin <= fn.wasm_end);
   }

   REQUIRE(dbg.locs.instr_locs.size() >= 2);

   // jit2 and jit_llvm emit IR during parse and native code in a later
   // pass, so the parser-level on_code_end hook fires before native
   // addresses are known. code_size / translate aren't populated by the
   // parser flow for these backends — consumers should call
   // psizam::debug::rebuild_from_module() after codegen completes
   // (validated separately by the "[llvm]" test).
   constexpr bool is_two_pass =
       std::is_same_v<TestType, jit2>
#if defined(PSIZAM_ENABLE_LLVM_BACKEND)
       || std::is_same_v<TestType, jit_llvm>
#endif
       ;
   if constexpr (!is_two_pass) {
      REQUIRE(dbg.code_size > 0);
      REQUIRE(dbg.code_begin != nullptr);

#ifndef __aarch64__
      // On x86_64 JIT emits instructions in address order; the invariant in
      // debug_instr_map::set asserts this. aarch64 codegen may reorder.
      for (size_t i = 1; i < dbg.locs.instr_locs.size(); ++i) {
         CHECK(dbg.locs.instr_locs[i-1].code_offset
               <= dbg.locs.instr_locs[i].code_offset);
      }
#endif

      // translate(pc) should round-trip: a PC within the emitted code
      // region returns a valid wasm address.
      auto* code_base = reinterpret_cast<const char*>(dbg.code_begin);
      auto wasm_addr =
         dbg.translate(code_base + dbg.locs.fn_locs[0].code_prologue);
      CHECK(wasm_addr != 0xFFFFFFFFu);
   }
}

BACKEND_TEST_CASE("null_debug_info is the zero-cost default",
                  "[debug]") {
   auto code = kTwoFuncs;
   // Default DebugInfo template arg is null_debug_info — no debug map.
   backend<std::nullptr_t, TestType> bkend(code, &wa);
   // Compiles and runs; null_debug_info has no state to inspect.
   SUCCEED();
}

// ────────────────────────────────────────────────────────────────────────────
// Backtrace walker end-to-end: WASM calls a host import, the host captures
// a backtrace using the backend's execution_context, we verify the frame
// chain was walked and at least the host-trampoline return address appears.
// Only jit_profile has EnableBacktrace=true, so the method is instantiated
// for that context. x86_64 has always had this; the aarch64 walker is new.
// ────────────────────────────────────────────────────────────────────────────

#if defined(__x86_64__) || defined(__aarch64__)
namespace {
struct bt_host {
   std::function<int(void**, int, void*)> capture_fn;
   void*                                  frames[32] = {};
   int                                    nframes    = 0;
   void                                   capture() {
      if (capture_fn) nframes = capture_fn(frames, 32, nullptr);
   }
};

// Minimal WASM: (import "env" "capture" (func $cap))
//               (func (export "go") (call $cap))
const std::vector<uint8_t> kHostCall = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,     // magic + version
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00,                  // type: [] -> []
    0x02, 0x0f, 0x01,                                    // imports: 1
       0x03, 0x65, 0x6e, 0x76,                           //   "env"
       0x07, 0x63, 0x61, 0x70, 0x74, 0x75, 0x72, 0x65,   //   "capture"
       0x00, 0x00,                                       //   func, type 0
    0x03, 0x02, 0x01, 0x00,                              // funcs: 1 of type 0
    0x07, 0x06, 0x01,                                    // exports: 1
       0x02, 0x67, 0x6f, 0x00, 0x01,                     //   "go" -> func 1
    0x0a, 0x06, 0x01,                                    // code: 1 body
       0x04, 0x00, 0x10, 0x00, 0x0b,                     //   len=4, 0 locals, call 0, end
};
} // namespace

TEST_CASE("backtrace() walks the JIT FP chain from a host call (jit_profile)",
          "[debug][backtrace]") {
   auto code = kHostCall;
   using rhf_t = registered_host_functions<bt_host, detail::execution_interface>;
   rhf_t::add<&bt_host::capture>("env", "capture");

   using backend_t = backend<rhf_t, jit_profile>;
   bt_host hs;
   backend_t bkend(code, hs, &wa);
   hs.capture_fn = [&](void** out, int n, void* uc) {
      return bkend.get_context().backtrace(out, n, uc);
   };

   bkend.call(hs, "env", "go");

   // The walker should produce at least one frame from within the WASM call
   // stack (the host call trampoline itself plus the WASM "go" function).
   // Zero frames would indicate top_frame was null or the FP chain was
   // broken. We don't pin specific PC values — those are JIT-emission-
   // address-dependent — just that the walker chased the chain.
   INFO("captured nframes=" << hs.nframes);
   CHECK(hs.nframes >= 1);

   // Every returned frame should be a non-null code pointer. A cleanly-null
   // entry in the middle would indicate the walker kept writing past the
   // end of the chain.
   for (int i = 0; i < hs.nframes; ++i) {
      INFO("frame[" << i << "] = " << hs.frames[i]);
      CHECK(hs.frames[i] != nullptr);
   }
}
#endif  // host-call backtrace

// ────────────────────────────────────────────────────────────────────────────
// Golden DWARF symbolization: a WASM module built with wasi-sdk -g imports
// env::capture() and calls it from a known source line in divide(). The
// host captures a backtrace, translates each PC to a wasm offset via
// debug_instr_map, then maps the wasm offset to a source (file, line) via
// dwarf::info. We assert the expected source line appears.
// ────────────────────────────────────────────────────────────────────────────

#if defined(__x86_64__) || defined(__aarch64__)
namespace {
std::vector<uint8_t> load_fixture(const char* name) {
   std::string path = std::string(PSIZAM_DEBUG_FIXTURES_DIR) + "/" + name;
   std::ifstream f(path, std::ios::binary);
   REQUIRE(f.is_open());
   std::ostringstream ss;
   ss << f.rdbuf();
   std::string blob = ss.str();
   return std::vector<uint8_t>(blob.begin(), blob.end());
}

struct dwarf_host {
   psizam::debug::info*                   dwarf_info  = nullptr;
   std::function<int(void**, int, void*)> capture_fn;
   void*                                  frames[32]  = {};
   int                                    nframes     = 0;
   std::function<uint32_t(const void*)>   translate_fn;

   struct resolved { std::string file; uint32_t line = 0; };
   std::vector<resolved>                  resolved_frames;

   struct diag { uint32_t wasm_addr = 0xFFFFFFFFu; bool had_loc = false; };
   std::vector<diag>                      diags;

   void host_capture() {
      nframes = capture_fn(frames, 32, nullptr);
      for (int i = 0; i < nframes; ++i) {
         diag d;
         d.wasm_addr = translate_fn(frames[i]);
         resolved r;
         if (d.wasm_addr != 0xFFFFFFFFu) {
            // translate() returns wasm offsets relative to the start of the
            // WASM file; DWARF locations are relative to the code-section
            // content start — subtract the code-section offset.
            uint32_t code_relative = d.wasm_addr - dwarf_info->wasm_code_offset;
            auto* loc = dwarf_info->get_location(code_relative);
            if (loc) {
               d.had_loc = true;
               r.line = loc->line;
               if (loc->file_index < dwarf_info->files.size())
                  r.file = dwarf_info->files[loc->file_index];
            }
         }
         diags.push_back(d);
         resolved_frames.push_back(std::move(r));
      }
   }
};
} // namespace

TEST_CASE("DWARF symbolization: captured PCs resolve to trap_guest.cpp source lines",
          "[debug][dwarf]") {
   auto code = load_fixture("trap_guest.wasm");
   REQUIRE(code.size() > 100);

   // Parse DWARF custom sections out of the raw WASM bytes first — the
   // dwarf::info must outlive the backend so backend::call sees the same
   // wasm addresses that DWARF encoded at build time.
   psio::input_stream wasm_stream(reinterpret_cast<const char*>(code.data()),
                                  code.size());
   auto dwarf_info = psizam::debug::get_info_from_wasm(wasm_stream);

   using rhf_t = registered_host_functions<dwarf_host, detail::execution_interface>;
   rhf_t::add<&dwarf_host::host_capture>("env", "capture");

   using backend_t = backend<rhf_t, jit_profile, default_options,
                             psizam::debug::debug_instr_map>;
   dwarf_host hs;
   hs.dwarf_info = &dwarf_info;
   backend_t bkend(code, hs, &wa);
   hs.capture_fn = [&](void** out, int n, void* uc) {
      return bkend.get_context().backtrace(out, n, uc);
   };
   hs.translate_fn = [&](const void* pc) {
      return bkend.get_debug().translate(pc);
   };

   // Call outer(10) → divide(10, 0) → host_capture()
   bkend.call(hs, "env", "outer", uint32_t(10));

   INFO("captured nframes=" << hs.nframes);
   REQUIRE(hs.nframes >= 1);

   // Build a dumpable summary so CHECK failures include the full resolution
   // chain: PC → wasm_addr → (file, line).
   std::ostringstream trace;
   for (size_t i = 0; i < hs.resolved_frames.size(); ++i) {
      const auto& r = hs.resolved_frames[i];
      const auto& d = hs.diags[i];
      trace << "frame[" << i << "] pc=" << hs.frames[i]
            << " wasm_addr=" << (d.wasm_addr == 0xFFFFFFFFu
                                     ? std::string("OOR")
                                     : std::to_string(d.wasm_addr))
            << " -> " << (r.file.empty() ? "<unresolved>" : r.file)
            << ":" << r.line << "\n";
   }
   INFO("resolved trace:\n" << trace.str());

   // At least one frame must resolve to trap_guest.cpp. We don't pin the
   // exact line number — clang's line-number emission can vary — but a
   // resolved file + non-zero line demonstrates the full pipeline works.
   bool matched = false;
   for (const auto& r : hs.resolved_frames) {
      if (r.file.find("trap_guest.cpp") != std::string::npos && r.line > 0) {
         matched = true;
      }
   }
   CHECK(matched);
}

// ────────────────────────────────────────────────────────────────────────────
// GDB/LLDB JIT-interface registration: verify that register_with_debugger()
// builds a valid ELF image for the emitted JIT code and links it into the
// global `__jit_debug_descriptor` chain that debuggers poll.
// Doesn't spawn gdb — a separate scripted attach test exercises live
// breakpoint hits. This test only validates the ELF synthesis + chain
// linkage, which is what every debugger-attach invocation depends on.
// ────────────────────────────────────────────────────────────────────────────

extern "C" {
   struct jit_desc_probe {
      uint32_t action_flag;
      void*    relevant_entry;
      void*    first_entry;
   };
   extern jit_desc_probe __jit_debug_descriptor;
}

TEST_CASE("register_with_debugger builds a valid ELF and links the descriptor",
          "[debug][gdb]") {
   auto code = load_fixture("trap_guest.wasm");

   psio::input_stream wasm_stream(reinterpret_cast<const char*>(code.data()),
                                  code.size());
   auto dwarf_info = psizam::debug::get_info_from_wasm(wasm_stream);

   using rhf_t = registered_host_functions<dwarf_host, detail::execution_interface>;
   rhf_t::add<&dwarf_host::host_capture>("env", "capture");

   using backend_t = backend<rhf_t, jit_profile, default_options,
                             psizam::debug::debug_instr_map>;
   dwarf_host hs;
   hs.dwarf_info = &dwarf_info;
   backend_t bkend(code, hs, &wa);

   // Snapshot the descriptor before registering; lets us detect that a new
   // entry was spliced in. Other tests in this process may have registered
   // earlier, so we compare relative to the pre-call state.
   auto* pre_entry = __jit_debug_descriptor.first_entry;

   auto reg = psizam::debug::register_with_debugger(
       dwarf_info, bkend.get_debug().locs, bkend.get_module(), wasm_stream);

   REQUIRE(reg != nullptr);

   // The new entry should now be at the head of the global `__jit_debug_descriptor`
   // chain that gdb/lldb polls. This is the hook that makes the JIT code visible
   // to the system debugger.
   CHECK(__jit_debug_descriptor.first_entry != nullptr);
   CHECK(__jit_debug_descriptor.first_entry != pre_entry);

   // `reg`'s destructor unlinks the entry — no leak; next test starts clean.
}

// ────────────────────────────────────────────────────────────────────────────
// LLVM backend: the parser-level imap hooks fire before native codegen, so
// debug_instr_map's fn_locs are bogus for LLVM. rebuild_from_module_llvm
// walks mod.code after codegen and installs correct native-address
// fn_locs. Verifies translate(pc) round-trips for each exported function.
// ────────────────────────────────────────────────────────────────────────────

TEST_CASE("rebuild_from_module produces correct fn_locs for jit2",
          "[debug][jit2]") {
   auto code = load_fixture("trap_guest.wasm");
   using rhf_t = registered_host_functions<dwarf_host, detail::execution_interface>;
   rhf_t::add<&dwarf_host::host_capture>("env", "capture");

   using backend_t = backend<rhf_t, jit2, default_options,
                             psizam::debug::debug_instr_map>;
   dwarf_host hs;
   backend_t  bkend(code, hs, &wa);

   // jit2 stores jit_code_offset as a relative offset into the
   // allocator's code buffer, so pass the allocator's base pointer as
   // native_code_base. LLVM's absolute path uses nullptr.
   auto& dbg = bkend.get_debug();
   auto& mod = bkend.get_module();
   psizam::debug::rebuild_from_module(dbg, mod, code.data(),
                                      mod.allocator.get_code_start());

   REQUIRE(dbg.locs.fn_locs.size() == 2);
   for (const auto& fn : dbg.locs.fn_locs) {
      auto* pc = reinterpret_cast<const char*>(dbg.code_begin) + fn.code_prologue;
      CHECK(dbg.translate(pc) == fn.wasm_begin);
   }
}

#if defined(PSIZAM_ENABLE_LLVM_BACKEND)
TEST_CASE("rebuild_from_module produces correct fn_locs for jit_llvm",
          "[debug][llvm]") {
   auto code = load_fixture("trap_guest.wasm");
   using rhf_t = registered_host_functions<dwarf_host, detail::execution_interface>;
   rhf_t::add<&dwarf_host::host_capture>("env", "capture");

   using backend_t = backend<rhf_t, jit_llvm, default_options,
                             psizam::debug::debug_instr_map>;
   dwarf_host hs;
   backend_t  bkend(code, hs, &wa);

   // Rebuild the map from the finalized LLVM module.
   auto& dbg = bkend.get_debug();
   // jit_llvm: jit_code_offset is an absolute native pointer.
   psizam::debug::rebuild_from_module(dbg, bkend.get_module(), code.data(),
                                       /*native_code_base=*/nullptr);

   // There are two non-import functions in the fixture (divide, outer) —
   // the map should reflect exactly those.
   REQUIRE(dbg.locs.fn_locs.size() == 2);

   // Each function's native base address should translate back to the
   // wasm offset we recorded (wasm_begin).
   for (const auto& fn : dbg.locs.fn_locs) {
      auto* pc = reinterpret_cast<const char*>(dbg.code_begin) + fn.code_prologue;
      uint32_t wasm_addr = dbg.translate(pc);
      INFO("fn wasm range [" << fn.wasm_begin << ", " << fn.wasm_end
           << ") translated pc → " << wasm_addr);
      CHECK(wasm_addr == fn.wasm_begin);
   }
}
#endif
#endif
