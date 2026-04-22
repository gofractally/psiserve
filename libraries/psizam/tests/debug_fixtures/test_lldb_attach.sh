#!/bin/bash
# Scripted lldb attach test. Runs psizam_debug_attach_helper under lldb,
# sets a source-line breakpoint on trap_guest.cpp:19, confirms the
# breakpoint resolves and fires, and that the backtrace includes the
# expected source frames. Fails the script on any unexpected output.
#
# Two things must be true for this to work:
#   - plugin.jit-loader.gdb.enable set to "on" (Apple's default is off)
#   - psizam's JIT-registered ELF must carry DWARF32 with a non-empty
#     symtab for exported WASM functions (fixed during wasm-debug-mode).

set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 path/to/psizam_debug_attach_helper path/to/trap_guest.wasm" >&2
    exit 2
fi

HELPER=$1
WASM=$2

if ! command -v lldb >/dev/null 2>&1; then
    echo "SKIP: lldb not found"
    exit 77   # CTest "skip"
fi

OUT=$(lldb --batch \
    -o "settings set plugin.jit-loader.gdb.enable on" \
    -o "breakpoint set -f trap_guest.cpp -l 19" \
    -o "run" \
    -o "thread backtrace 3" \
    -o "kill" \
    -o "quit" \
    -- "$HELPER" "$WASM" 2>&1)

echo "$OUT"
echo "---"

# Require: breakpoint resolved (not pending), JIT stopped at divide, and
# at least one backtrace frame showing source line 19.
if ! echo "$OUT" | grep -q "location added to breakpoint"; then
    echo "FAIL: breakpoint never resolved; GDB JIT interface likely disabled" >&2
    exit 1
fi

if ! echo "$OUT" | grep -qE "stop reason = breakpoint.*JIT.*divide at trap_guest\\.cpp:19"; then
    echo "FAIL: did not stop at trap_guest.cpp:19 in JIT'd divide" >&2
    exit 1
fi

if ! echo "$OUT" | grep -qE "frame #1:.*outer at trap_guest\\.cpp:25"; then
    echo "FAIL: backtrace missing outer frame at trap_guest.cpp:25" >&2
    exit 1
fi

echo "PASS: lldb source-line breakpoint hit, backtrace includes WASM source frames"
