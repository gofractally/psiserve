# psizam debug mode test fixtures

## `trap_guest.cpp` / `trap_guest.wasm`

Guest module used by `debug_trap_tests.cpp`. Imports `env::capture`,
exports `divide` and `outer`. Built with DWARF (`.debug_info`,
`.debug_line`, etc.) so the test can resolve captured PCs back to source
lines.

**Regenerating the `.wasm`** (only needed after editing the `.cpp`):
```
/path/to/wasi-sdk/bin/clang++ \
  --target=wasm32-unknown-unknown -g -O0 -nostdlib \
  -Wl,--no-entry -Wl,--export-dynamic \
  -o trap_guest.wasm trap_guest.cpp
```

## `attach_helper.cpp`

Standalone program: loads `trap_guest.wasm`, JIT-compiles it with
`debug_instr_map`, calls `psizam::debug::register_with_debugger()` to
link the synthesized ELF into `__jit_debug_descriptor`, then invokes
`outer(10)`.

## Live debugger attach — works on both Linux (gdb) and macOS (lldb)

### macOS (lldb)

Apple's default setting disables the GDB JIT loader plugin. Turn it on
explicitly:

```
lldb --batch \
  -o "settings set plugin.jit-loader.gdb.enable on" \
  -o "breakpoint set -f trap_guest.cpp -l 19" \
  -o "run" \
  -o "thread backtrace" \
  -o "kill" -o "quit" \
  -- build/Debug/bin/psizam_debug_attach_helper \
     libraries/psizam/tests/debug_fixtures/trap_guest.wasm
```

Expected:
```
* thread #1, stop reason = breakpoint 1.1
  * frame #0: JIT(...)`divide at trap_guest.cpp:19
    frame #1: JIT(...)`outer  at trap_guest.cpp:25
    frame #2: JIT(...)`_wasm_entry + 104
    frame #3+: native C++ frames through psizam to main
```

Works on macOS arm64 and x86_64 (via Rosetta) against the checked-in
fixture `trap_guest.wasm`. Validated manually, see
`test_lldb_attach.sh` for the regex-asserted version.

You can put this setting in `~/.lldbinit` to avoid specifying it each
time:
```
settings set plugin.jit-loader.gdb.enable on
```

### Linux (gdb)

gdb enables the JIT interface by default — no setting needed:
```
gdb --batch \
  -ex "b trap_guest.cpp:19" \
  -ex "run" \
  -ex "bt" \
  -ex "continue" \
  --args build/Debug/bin/psizam_debug_attach_helper \
         libraries/psizam/tests/debug_fixtures/trap_guest.wasm
```

## Historical note: blockers found and fixed during porting

Debugger attach on macOS initially appeared broken. Investigation
(rather than assuming "Apple's lldb is incomplete") turned up three
distinct issues — two were upstream bugs in `debug_eos_vm`:

1. **`plugin.jit-loader.gdb.enable` default on macOS** — Apple ships
   lldb with the GDB JIT loader plugin conditionally disabled. Must be
   turned on at the command line or in `~/.lldbinit`. (Not a psizam
   bug.)

2. **`write_symtab` off-by-one** — `if (idx > num_imported)` silently
   dropped the first non-imported exported WASM function from the
   synthesized ELF's symbol table. Fixed to `>=` in `dwarf.cpp`.

3. **DWARF64 format emission** — both the `.debug_info` and
   `.debug_line` sections were emitted in DWARF64 format (the
   `0xffffffff` length escape followed by a uint64 length). macOS lldb's
   JIT loader silently fails to parse DWARF64 compile units from
   gdb-JIT-registered modules — breakpoint-by-source-line stays pending
   with no error. Switched to DWARF32 (uint32 length) in the write path.
   DWARF32 is universal and has 4 GB of headroom per compile unit,
   which is plenty for any realistic WASM.

## `test_lldb_attach.sh`

Scripted version of the above. Asserts the breakpoint resolved, the
process stopped at `trap_guest.cpp:19`, and the backtrace includes
`outer at trap_guest.cpp:25`. Exits with ctest "skip" (77) if lldb is
not installed. Script IO may be flaky in some non-TTY environments —
`bash -x` shows the real execution if debugging.
