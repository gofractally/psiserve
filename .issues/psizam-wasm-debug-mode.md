---
id: psizam-wasm-debug-mode
title: psizam WASM source-level debugging mode (DWARF + GDB/LLDB JIT interface)
status: in-progress
priority: medium
area: psizam
agent: psiserve-agent-psio
branch: main
created: 2026-04-20
depends_on: []
blocks: []
---

## Description

eos-vm + psibase provided source-level debugging of WASM modules: symbolized
stack traces on trap, and attachable GDB/LLDB sessions that step through the
original C++ source while actually executing JIT'd native code. Most of the
underlying VM machinery survived the eos-vm → psizam rebrand, but the
DWARF parser and ELF+GDB-JIT symbolization layer did not. The goal of this
issue is to re-enable debug as a first-class **option mode** across the
psizam backends.

Two deliverables, independent but complementary:

1. **Symbolized stack traces** on trap — map JIT PCs (or interpreter PCs) back
   to `file.cpp:line: demangled_fn` using DWARF embedded in the WASM's
   custom sections.
2. **Live debugger attach** — register a synthesized ELF with the system
   debugger via the standard `__jit_debug_register_code()` protocol so
   `gdb`/`lldb` can set breakpoints, step, and inspect locals in the JIT'd
   code as if it were a native binary.

## Current State in psizam (what survives)

The VM side of the debug plumbing is largely intact:

- **Frame-chain JIT codegen**: `emit_setup_backtrace` / `emit_restore_backtrace`
  still wrap every host call in `include/psizam/detail/x86_64.hpp` (~30 call
  sites) and `include/psizam/detail/aarch64.hpp`, maintaining RBP/FP so a
  signal-safe walker can cross wasm↔host boundaries.
- **Activation sentinels**: `_top_frame` / `_bottom_frame` at offsets 0/8 of
  the execution context (`include/psizam/detail/execution_context.hpp:637-638`).
- **Backtrace walkers**:
  - JIT: `execution_context.hpp:1030` — `backtrace(void** out, int count, void* uc)`,
    understands prologue/epilogue + Apple/FreeBSD/Linux ucontext.
  - Interpreter: `execution_context.hpp:1772` — walks `_as` activation stack.
- **Backend template hook**: every backend (`jit`, `jit2`, `llvm`, `interpreter`,
  `null`) accepts a `DebugInfo` template param and exposes `get_debug()`
  (`include/psizam/backend.hpp`).
- **Debug-info stubs**: `include/psizam/detail/debug_info.hpp` defines
  `null_debug_info` (no-op) and `profile_instr_map` (binary-search
  `translate(pc) → wasm_addr`), already consumed by `detail/profile.hpp`.
- **pzam format carries flags**: `pzam_format.hpp:205-206` reserves
  `debug_info` and `async_backtrace` bytes; wired through `pzam_compile.hpp`,
  `pzam_cache.hpp`, and the `--backtrace` CLI flag in `tools/pzam_compile.cpp`.
- **Compile-time selector**: `jit_execution_context<EnableBacktrace>` and the
  static `async_backtrace()` constexpr (`execution_context.hpp:1089`) let
  backtrace instrumentation compile out when disabled — zero overhead when
  debug is not selected.

## What's Missing (what needs porting)

The DWARF-aware symbolization layer lives only in psibase and was not brought
over in the rebrand:

- `/Users/dlarimer/psibase/libraries/debug_eos_vm/dwarf.cpp` (3034 lines) —
  parses `.debug_info` / `.debug_line` / `.debug_abbrev` / `.debug_str` from
  WASM custom sections, generates `.debug_frame` CFI, synthesizes a full
  ELF64 image whose `.text` points at the JIT'd code, registers it via
  `__jit_debug_descriptor` / `__jit_debug_register_code()` (lines 2720-2789).
- `/Users/dlarimer/psibase/libraries/debug_eos_vm/include/debug_eos_vm/dwarf.hpp`
  — public API: `info`, `jit_fn_loc`, `jit_instr_loc`, `get_info_from_wasm()`,
  `register_with_debugger()`.
- `/Users/dlarimer/psibase/libraries/debug_eos_vm/include/debug_eos_vm/debug_eos_vm.hpp`
  — `debug_instr_map` (richer than `profile_instr_map`: also records per-function
  prologue/end boundaries needed for CFI), and the one-shot `enable_debug(code,
  backend, dwarf_info, wasm_source)` entry point.
- `/Users/dlarimer/psibase/libraries/debug_eos_vm/locals.hpp` — DWARF local-var
  expression translation for live-locals inspection (`wasm_frame`).

Reference consumers to mirror:
- `/Users/dlarimer/psibase/programs/psitest/main.cpp:672-706` — symbolized
  stack-trace printing (PC → `translate(pc)` → `dwarf_info.get_location()`).
- `/Users/dlarimer/psibase/libraries/psibase/native/src/ExecutionContext.cpp:216-234`
  — production integration (conditionally selects `debug_instr_map` backend,
  calls `register_with_debugger`).

## Additional Verification Findings (2026-04-20)

Follow-up read confirmed most of the research and uncovered three items that
change the plan:

1. **aarch64 `backtrace()` walker does not exist.** JIT `backtrace()` in
   `execution_context.hpp:1030` is `#ifdef __x86_64__`. Only the interpreter
   walker (line 1772, arch-agnostic) works on arm64 today. Need a ~50-line
   aarch64 FP-chain walker parsing `ucontext` on Linux + macOS.
2. **aarch64 JIT codegen already emits frame-chain setup** when
   `_enable_backtrace=true` (`jit_codegen_a64.hpp:607/617/711/773`,
   `aarch64.hpp:279/310/4955/5005`). No codegen work needed — just the
   walker.
3. **`dwarf.cpp` uses `psio::` 362 times.** Not a blocker — `libraries/psio/`
   is already present in psiserve. The port keeps those uses as-is and links
   psio, rather than hand-rolling serialization.

## Scope

**In scope (all delivered by this issue)**
- x86_64 + aarch64, all JIT backends (jit, jit2, jit_llvm) + interpreter.
- `debug_instr_map` as a sibling of `null_debug_info` / `profile_instr_map`
  in `detail/debug_info.hpp`.
- DWARF parser + ELF synthesizer + GDB/LLDB JIT registrar ported into
  `libraries/psizam/src/debug/` + `include/psizam/debug/`.
- aarch64 backtrace walker; aarch64 CFI emitter; `e_machine` parameterized
  between `EM_X86_64` and `EM_AARCH64`.
- LLVM backend `on_function_start` / `on_instr_start` hooks wired.
- Wire the already-reserved `pzam_format.debug_info` / `async_backtrace`
  flags to actual behavior.
- Catch2 golden-trap tests (interpreter + each JIT × x86_64 + aarch64).
- Scripted gdb/lldb attach smoke test.
- Reference CLI: `pzam-run --debug` loads a pzam, registers the ELF, runs.

**Out of scope (split into separate follow-up issues)**
- Rich DAP/MI protocol integration for IDE debug adapters.
- Live variable inspection (`wasm_frame` + DWARF locals translation) —
  stretch goal; backtrace + source:line + breakpoints come first.
- Interpreter debugger attach (interpreter has no native PCs to register;
  interpreter gets symbolized traces only).

## Design Decisions

### Library structure: debug lives inside psizam, no CMake gate
- Code in `libraries/psizam/src/debug/` + `include/psizam/debug/` (sibling of
  `include/psizam/detail/debug_info.hpp`).
- **Always built** into psizam. No `PSIZAM_ENABLE_DEBUG_INFO` option.
- Rationale: host build config must not determine runtime capability. A
  release psinode should still be able to symbolize WASM traces. Few
  hundred KB of DWARF parser code; unused code stripped by the linker
  dead-code elimination when no consumer instantiates `debug_instr_map`.
- Matches how psibase ships `debug_eos_vm` (always-built, runtime switch
  is `enable_debug()` call).

### Three axes of selection, unentangled
1. **Host build config** (`-O2` vs `-O0`) — does not affect debug capability.
2. **psizam `DebugInfo` template param** — per-backend-instance choice at
   compile time. `null_debug_info` compiles all instrumentation to nothing
   and lets the linker strip the DWARF code. `debug_instr_map` pays for
   the pc-tracking map during codegen; DWARF parser only runs at load +
   on trap.
3. **Per-WASM-module runtime** — does the module carry DWARF in custom
   sections? If yes, full source-line symbolization; if no, debugger still
   gets PC-only backtrace (frame chain works regardless).

### Runtime flag vs. template param
- Keep both. Template param (`DebugInfo`) gates whether instrumentation
  code is *emitted*. The existing `pzam_format.debug_info` byte is a
  per-module runtime flag that gates whether the DWARF+ELF layer
  *activates* for that specific module.

### Arch gate strategy
- `register_with_debugger` dispatches on build arch: emits
  `EM_X86_64` or `EM_AARCH64` ELF with appropriate CFI. No `#ifdef
  __x86_64__` hard gate — both paths compiled in.
- Backtrace production works on both arches.

### Interpreter debugger attach
- Not implemented — interpreter has no native PCs to register. Interpreter
  gets symbolized traces only; live attach is JIT-only.

## Acceptance Criteria

### Infrastructure
- [x] `psizam::debug::debug_instr_map` type ported as a sibling of
      `profile_instr_map` (lives in `include/psizam/debug/debug.hpp` rather
      than `detail/debug_info.hpp` — same surface, separate header).
- [x] Ported DWARF parser + CFI emitter + ELF synthesizer + GDB JIT registrar
      in `libraries/psizam/src/debug/` + `include/psizam/debug/`.
- [x] `psizam::debug::enable_debug(code, backend, dwarf_info, wasm_bytes)`
      one-shot wrapper.
- [x] psizam CMakeLists.txt always builds the debug sources (no option gate);
      linked in via new `psizam-debug` STATIC library target.

### Zero-overhead guarantee
- [ ] `DebugInfo = null_debug_info` produces identical `emit_setup_backtrace`-free
      code (size/disasm of a small module shows no difference).
- [ ] Linker dead-code elimination drops the DWARF parser when no consumer
      instantiates `debug_instr_map` (verify with `nm` / `size`).

### x86_64
- [x] Symbolized stack traces: full pipeline validated on x86_64 via Rosetta.
      Test captures 3 frames from a WASM→host call, resolves 2 to
      `trap_guest.cpp:19` (host_capture site) and `trap_guest.cpp:25`
      (caller in outer()). Requires jit_profile backend for the FP-walker.
      Not yet verified for jit/jit2/jit_llvm (EnableBacktrace is false on
      those — a future refactor could make backtrace instrumentation an
      independent template axis).
- [x] `register_with_debugger` validated: Catch2 test verifies the
      synthesized ELF is linked into `__jit_debug_descriptor.first_entry`.
      Standalone `psizam_debug_attach_helper` demonstrates the API.
- [x] Live `lldb` source-line breakpoint attach working on macOS arm64
      and x86_64 (via Rosetta) against the checked-in `trap_guest.wasm`
      fixture. Verified backtrace includes `JIT(...)\`divide at
      trap_guest.cpp:19` + `JIT(...)\`outer at trap_guest.cpp:25`.
      Procedure + script: `tests/debug_fixtures/README.md` +
      `test_lldb_attach.sh`.
- [x] Three upstream bugs fixed during lldb-attach investigation:
      (a) `write_symtab` off-by-one (`idx > num_imported` → `>=`) that
      silently dropped the first exported WASM function's symbol;
      (b) `.debug_info` emitted in DWARF64 format that macOS lldb can't
      parse from JIT-registered modules (switched to DWARF32);
      (c) `.debug_line` emitted in DWARF64 format (same fix).
- [ ] Live `gdb` attach on Linux: code path compatible (DWARF32 is the
      standard GDB reads natively). Not yet verified on Linux CI.

### aarch64
- [x] Aarch64 `backtrace()` method on `jit_execution_context` — walks FP chain,
      parses ucontext on Linux + macOS. Verified with Catch2 test: JIT-emitted
      frames walked correctly from a host-call trampoline context.
- [x] Aarch64 codegen fix: `STR X19, [X19, ...]` → `STR X29, [X19, ...]` at
      both `_top_frame` and `_bottom_frame` stores — was previously storing
      the context pointer instead of the frame pointer.
- [x] `e_machine` parameterized: `EM_X86_64` vs `EM_AARCH64` selected by
      build arch. Added `EM_AARCH64` to local `elf.h`.
- [x] AArch64 CFI emission: `get_basic_frame` / `get_function_entry_frame` /
      `get_function_frame` now branch on arch. DWARF regs 29/30/31 used for
      FP/LR/SP; `return_address_register` CIE field switches to 30 on arm64.
- [x] Symbolized stack traces on macOS arm64: full pipeline validated. Same
      test passes natively; frames resolve to the expected source lines.
- [x] Live `lldb` attach on macOS arm64: **working** (see x86_64 entry
      above — same mechanism, same fixture). Earlier assumption that
      macOS lldb was upstream-blocked was wrong; the three real blockers
      (Apple's `jit-loader.gdb.enable=off` default, off-by-one in
      `write_symtab`, DWARF64-vs-DWARF32) are all either fixable in our
      code or documented as a one-line setting.

### LLVM backend + jit2 (two-pass backends)
- [x] Function-level symbolization working via
      `psizam::debug::rebuild_from_module(map, mod, wasm_base, native_code_base)`,
      a post-codegen helper that walks `mod.code[]` and installs correct
      `fn_locs` from `jit_code_offset` + `body_start`. Same helper
      supports both jit_llvm (pass `native_code_base=nullptr`, offsets
      are absolute) and jit2 (pass the allocator code_start, offsets are
      relative). Validated by `[llvm]` and `[jit2]` Catch2 tests — both
      assert `translate(pc)` round-trips for each exported function.
- [ ] Instruction-level symbolization (per-instruction pc→wasm_addr
      granularity) — follow-up; requires the LLVM codegen pass to drive
      `on_instr_start` hooks, not just function boundaries.
- [ ] Live backtrace under jit_llvm — blocked by jit_llvm having
      `enable_backtrace = false`; would need either a jit_llvm_profile
      variant or decoupling EnableBacktrace from backend choice.
- [x] GDB JIT symbol clash with LLVM ORC resolved: psizam's
      `__jit_debug_register_code` and `__jit_debug_descriptor` are now
      weak, so LLVM's definitions take precedence when both are linked.

### Format flag plumbing
- [ ] `pzam-compile --debug` flips `debug_info` + `async_backtrace` bits.
- [ ] Loader honors `debug_info` bit → activates DWARF layer if present.
- [ ] `pzam-run --debug wasm` produces symbolized traces.

### Tests
- [x] Catch2 smoke tests in `libraries/psizam/tests/debug_trap_tests.cpp`:
      `debug_instr_map` population across interpreter, jit, jit2; `translate`
      round-trip; `null_debug_info` zero-cost default; `backtrace()`
      end-to-end via jit_profile + host call on both arm64 and x86_64
      (Rosetta).
- [x] Catch2 golden symbolization test using a `-g` WASM fixture built with
      wasi-sdk (`tests/debug_fixtures/trap_guest.{cpp,wasm}`): captures host-
      call backtrace, translates PCs via debug_instr_map, resolves wasm
      offsets via DWARF, asserts expected source file/line appears.
      **arm64: 8 tests / 34 assertions, x86_64: 8 tests / 46 assertions.**
- [ ] Scripted `gdb` batch test (x86_64): launch under gdb, set breakpoint
      by source line, continue, assert breakpoint hit.
- [ ] Scripted `lldb` batch test (aarch64 macOS): same.

### Docs
- [ ] `libraries/psizam/docs/debug-mode.md` — how to build WASM with DWARF,
      how to select `debug_instr_map` at `backend<>` instantiation, how to
      attach gdb/lldb.

## Implementation Plan

### Phase 1 — Port the DWARF layer (x86_64 path intact)
1. Copy `/Users/dlarimer/psibase/libraries/debug_eos_vm/` into:
   - `libraries/psizam/src/debug/dwarf.cpp`
   - `libraries/psizam/src/debug/locals.hpp`
   - `libraries/psizam/include/psizam/debug/dwarf.hpp`
   - `libraries/psizam/include/psizam/debug/debug_eos_vm.hpp` (rename to
     `debug.hpp` for cleanliness)
   - `libraries/psizam/include/psizam/debug/elf.h`
2. Rename: `eosio::vm::` → `psizam::`, `debug_eos_vm::` → `psizam::debug::`.
3. Swap: `EOS_VM_ASSERT(...)` → `PSIZAM_ASSERT(...)`.
4. Retarget: `eosio::vm::module` → `psizam::module`,
   `profile_instr_map` fields → new `debug_instr_map` once phase 2 lands.
5. Keep `psio::` references as-is; link `libraries/psio`.
6. Wire `libraries/psizam/CMakeLists.txt` to always build the debug sources
   (new `psizam_debug_srcs` target, linked into `psizam` static lib).
7. Verify: builds clean.

### Phase 2 — debug_instr_map + x86_64 wiring
8. Add `psizam::debug_instr_map` to `detail/debug_info.hpp`:
   - `std::vector<jit_fn_loc>` (code_prologue, code_body, code_end)
   - `std::vector<jit_instr_loc>` (pc, wasm_addr)
   - `translate(pc)`, `get_function(pc)`, async-signal-safe lookups
   - `builder` that satisfies the same hook surface as `profile_instr_map`
9. Honor `pzam_format.debug_info` + `async_backtrace` through
   `pzam_compile.hpp` / `pzam_cache.hpp` / `pzam_loader.hpp`.
10. x86_64 trap path: `backend.get_context().backtrace(...)` → `translate(pc)`
    → `dwarf_info.get_location(addr)` → formatted `file.cpp:line: fn`.
11. **Milestone test**: Catch2 golden-trap test — small WASM built with
    `-gdwarf`, deterministic trap under x86_64 jit, assert source line in
    trace.

### Phase 3 — x86_64 GDB attach
12. Extend `pzam-run` / tooling with `--debug` that calls
    `psizam::debug::register_with_debugger`.
13. Scripted `gdb` batch test: launch → `break foo.cpp:42` → `run` → assert
    hit.
14. Verify jit2 path works (same codegen emits, same `debug_instr_map`).

### Phase 4 — aarch64 parity
15. Implement aarch64 `backtrace(void**, int, void*)` on
    `jit_execution_context` — walks FP chain from X29, parses `ucontext`
    for Linux (`uc_mcontext.regs`) and macOS (`uc_mcontext->__ss.__fp/__lr/__sp`).
16. Parameterize `Elf64_Ehdr.e_machine`: `EM_X86_64` vs `EM_AARCH64`.
17. Write AArch64 CFI: rewrite `get_basic_frame` / `get_function_frame` /
    `get_function_entry_frame` for DWARF regs FP=29, LR=30, SP=31 (x86
    uses RBP=6, RSP=7, RA=16).
18. Catch2 golden-trap tests on aarch64 (jit + interp).
19. Scripted `lldb` batch test on macOS arm64.

### Phase 5 — LLVM backend hooks
20. Add `on_function_start` / `on_instr_start` calls in
    `detail/ir_writer_llvm.hpp` at the same points machine_code_writer
    invokes them (track IR builder insert point → emitted machine code
    address; LLVM AOT makes this harder since code addr isn't known until
    link time — may need a two-pass approach or relocation-deferred map).
21. Trap symbolization test for jit_llvm.

### Phase 6 — Docs
22. `libraries/psizam/docs/debug-mode.md` — how to build WASM with
    `-gdwarf`, how to instantiate `backend<..., debug_instr_map>`, how to
    attach gdb/lldb.

### Follow-up issues (not in scope for this issue)
- `wasm_frame` + DWARF locals translation for live variable inspection.
- DAP/MI protocol integration for IDE debug adapters.

## Relevant Files (psizam side)

- `libraries/psizam/include/psizam/backend.hpp` — `DebugInfo` template param
  wiring on every backend.
- `libraries/psizam/include/psizam/detail/debug_info.hpp` — `null_debug_info`,
  `profile_instr_map`; add `debug_instr_map` here.
- `libraries/psizam/include/psizam/detail/execution_context.hpp` — surviving
  `backtrace()` methods (JIT line 1030, interp line 1772), frame sentinels
  lines 637-638, `async_backtrace()` line 1089.
- `libraries/psizam/include/psizam/detail/x86_64.hpp` — `emit_setup_backtrace`
  / `emit_restore_backtrace` call sites.
- `libraries/psizam/include/psizam/detail/aarch64.hpp` — same, aarch64.
- `libraries/psizam/include/psizam/pzam_format.hpp:205-206` — reserved
  `debug_info` / `async_backtrace` flag bytes.
- `libraries/psizam/tools/pzam_compile.cpp` — CLI entry point for flag plumbing.

## Reference Files (psibase side — source to port from)

- `/Users/dlarimer/psibase/libraries/debug_eos_vm/dwarf.cpp` (3034 lines)
- `/Users/dlarimer/psibase/libraries/debug_eos_vm/include/debug_eos_vm/dwarf.hpp` (133 lines)
- `/Users/dlarimer/psibase/libraries/debug_eos_vm/include/debug_eos_vm/debug_eos_vm.hpp` (137 lines)
- `/Users/dlarimer/psibase/libraries/debug_eos_vm/locals.hpp` (108 lines)
- `/Users/dlarimer/psibase/libraries/debug_eos_vm/include/debug_eos_vm/debug_contract.hpp` (75 lines)
- `/Users/dlarimer/psibase/libraries/debug_eos_vm/include/elf.h` — local ELF types
- `/Users/dlarimer/psibase/programs/psitest/main.cpp:672-706` — consumer example
- `/Users/dlarimer/psibase/libraries/psibase/native/src/ExecutionContext.cpp:216-234`
  — integration example
