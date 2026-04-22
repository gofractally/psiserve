// Fixture for the golden DWARF symbolization test.
//
// The WASM imports env::capture() and calls it from a known source line
// inside divide(). The host captures a backtrace at that point, resolves
// each PC through the debug_instr_map, then through DWARF, and verifies
// that this source line is in the resulting trace.
//
// Compiled once with wasi-sdk clang and checked in as trap_guest.wasm.
// To regenerate:
//   /path/to/wasi-sdk/bin/clang++ --target=wasm32-unknown-unknown \
//     -g -O0 -nostdlib -Wl,--no-entry -Wl,--export-dynamic \
//     -o trap_guest.wasm trap_guest.cpp

extern "C" __attribute__((import_module("env"), import_name("capture")))
void host_capture();

__attribute__((noinline, export_name("divide")))
int divide(int a, int b) {
    host_capture();                   // CAPTURE_LINE — asserted by test
    return (b == 0) ? -1 : (a / b);
}

__attribute__((noinline, export_name("outer")))
int outer(int x) {
    return divide(x, 0);
}
