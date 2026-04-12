#include <psizam/backend.hpp>
#include <psizam/detail/wasi_host.hpp>

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
   if (argc < 2) {
      std::cerr << "Usage: psizam-wasi [--dir=guest:host ...] [--backend=interpreter|jit] <module.wasm> [args...]\n";
      return 1;
   }

   std::string backend_str = "interpreter";
   std::string wasm_file;
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
      } else if (arg.starts_with("--backend=")) {
         backend_str = arg.substr(10);
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

   // Collect wasm args (argv[0] = wasm filename)
   wasm_args.push_back(wasm_file);
   for (; i < argc; i++)
      wasm_args.push_back(argv[i]);

   // Default preopens if none specified
   if (dirs.empty()) {
      dirs.push_back({".", "."});
   }

   // Read the wasm module
   auto code = read_wasm(wasm_file);

   // Set up WASI host
   wasi_host wasi;
   wasi.args = std::move(wasm_args);

   // Pass through host environment
   if (environ) {
      for (char** e = environ; *e; e++)
         wasi.env.push_back(*e);
   }

   // Add preopened directories
   for (auto& [guest, host] : dirs)
      wasi.add_preopen(guest, host);

   // Set up host function table with WASI functions
   host_function_table table;
   register_wasi(table);

   // Allocator for wasm linear memory
   wasm_allocator wa;

   try {
      if (backend_str == "interpreter") {
         using backend_t = backend<std::nullptr_t, interpreter>;
         backend_t bkend(code, std::move(table), &wasi, &wa);
         bkend.call(&wasi, "_start");
      } else if (backend_str == "jit") {
         using backend_t = backend<std::nullptr_t, jit>;
         backend_t bkend(code, std::move(table), &wasi, &wa);
         bkend.call(&wasi, "_start");
      } else if (backend_str == "jit2") {
         using backend_t = backend<std::nullptr_t, jit2>;
         backend_t bkend(code, std::move(table), &wasi, &wa);
         bkend.call(&wasi, "_start");
#ifdef PSIZAM_ENABLE_LLVM_BACKEND
      } else if (backend_str == "llvm") {
         using backend_t = backend<std::nullptr_t, jit_llvm>;
         backend_t bkend(code, std::move(table), &wasi, &wa);
         bkend.call(&wasi, "_start");
#endif
      } else {
         std::cerr << "Error: unknown backend '" << backend_str << "'\n";
         std::cerr << "Supported: interpreter, jit, jit2"
#ifdef PSIZAM_ENABLE_LLVM_BACKEND
                   << ", llvm"
#endif
                   << "\n";
         return 1;
      }
   } catch (const wasi_host::wasi_exit_exception& e) {
      return e.code;
   } catch (const psizam::exception& e) {
      std::cerr << "psizam error: " << e.what() << " : " << e.detail() << "\n";
      return 1;
   } catch (const std::exception& e) {
      std::cerr << "Error: " << e.what() << "\n";
      return 1;
   }

   return wasi.exit_code;
}
