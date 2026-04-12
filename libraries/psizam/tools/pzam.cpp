// pzam: Unified CLI for .pzam compiled WASM modules.
//
// Subcommands:
//   pzam compile <input.wasm> [-o output.pzam] [--target=...] [--backend=...]
//   pzam run <module.pzam> [--dir=guest:host] [-- args...]
//   pzam inspect <module.pzam>
//   pzam validate <module.wasm|module.pzam>

#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

// Forward declarations for subcommand entry points.
// Each is defined in its own translation unit (pzam_compile.cpp, pzam_run.cpp).
int pzam_compile_main(int argc, char** argv);
int pzam_run_main(int argc, char** argv);
int pzam_inspect_main(int argc, char** argv);
int pzam_validate_main(int argc, char** argv);

static void usage(const char* prog) {
   std::cerr << "Usage: " << prog << " <command> [options]\n\n"
             << "Commands:\n"
             << "  compile   Compile a .wasm file to a .pzam native module\n"
             << "  run       Execute a pre-compiled .pzam module\n"
             << "  inspect   Show metadata and code section info from a .pzam file\n"
             << "  validate  Validate a .wasm or .pzam file\n"
             << "\n"
             << "Run '" << prog << " <command> --help' for command-specific options.\n";
}

int main(int argc, char** argv) {
   if (argc < 2) {
      usage(argv[0]);
      return 1;
   }

   std::string_view cmd = argv[1];

   // Shift argv so subcommand sees itself as argv[0]
   int sub_argc = argc - 1;
   char** sub_argv = argv + 1;

   if (cmd == "compile") {
      return pzam_compile_main(sub_argc, sub_argv);
   } else if (cmd == "run") {
      return pzam_run_main(sub_argc, sub_argv);
   } else if (cmd == "inspect") {
      return pzam_inspect_main(sub_argc, sub_argv);
   } else if (cmd == "validate") {
      return pzam_validate_main(sub_argc, sub_argv);
   } else if (cmd == "--help" || cmd == "-h" || cmd == "help") {
      usage(argv[0]);
      return 0;
   } else {
      std::cerr << "Unknown command: " << cmd << "\n\n";
      usage(argv[0]);
      return 1;
   }
}
