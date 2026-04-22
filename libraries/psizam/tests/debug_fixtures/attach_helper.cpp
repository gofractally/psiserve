// Demonstrates live debugger attach: parses trap_guest.wasm, registers
// the synthesized ELF with __jit_debug_descriptor, runs WASM code, then
// idles briefly so a scripted debugger has time to attach and catch the
// JIT-registered breakpoint.

#include <psizam/backend.hpp>
#include <psizam/debug/debug.hpp>
#include <psizam/host_function.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace psizam;

namespace {
std::vector<uint8_t> load_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(2);
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string blob = ss.str();
    return std::vector<uint8_t>(blob.begin(), blob.end());
}

// Host-side state — only used so the imported env::capture has somewhere
// to land. Not asserting anything here; the point is that the WASM function
// actually runs under the debugger.
struct host_state {
    int captures = 0;
    void host_capture() { ++captures; }
};
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s path/to/trap_guest.wasm\n", argv[0]);
        return 1;
    }
    auto code = load_file(argv[1]);

    psio::input_stream wasm_stream(reinterpret_cast<const char*>(code.data()),
                                   code.size());
    auto dwarf_info = psizam::debug::get_info_from_wasm(wasm_stream);

    using rhf_t = registered_host_functions<host_state, detail::execution_interface>;
    rhf_t::add<&host_state::host_capture>("env", "capture");
    using backend_t = backend<rhf_t, jit_profile, default_options,
                              psizam::debug::debug_instr_map>;

    wasm_allocator wa;
    host_state     hs;
    backend_t      bkend(code, hs, &wa);

    auto reg = psizam::debug::register_with_debugger(
        dwarf_info, bkend.get_debug().locs, bkend.get_module(), wasm_stream);
    if (!reg) {
        std::fprintf(stderr, "register_with_debugger failed\n");
        return 1;
    }

    // Call outer(10). The WASM calls divide(10, 0), which calls the host
    // capture — if a debugger is attached with a breakpoint on
    // trap_guest.cpp:19, this is where it will hit.
    bkend.call(hs, "env", "outer", uint32_t(10));

    std::printf("attach_helper: captures=%d\n", hs.captures);
    return hs.captures == 1 ? 0 : 1;
}
