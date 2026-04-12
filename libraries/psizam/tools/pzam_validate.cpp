// pzam validate: Validate a .wasm or .pzam file.
//
// Usage: pzam validate <module.wasm|module.pzam>

#include <psizam/pzam_format.hpp>
#include <psizam/detail/parser.hpp>
#include <psizam/detail/null_writer.hpp>
#include <psizam/utils.hpp>

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace psizam;

static bool validate_pzam(const std::string& path) {
   std::ifstream in(path, std::ios::binary | std::ios::ate);
   if (!in.is_open()) {
      std::cerr << "Error: cannot open: " << path << "\n";
      return false;
   }
   auto size = in.tellg();
   in.seekg(0);
   std::vector<char> data(size);
   in.read(data.data(), size);

   if (!pzam_validate(data)) {
      std::cerr << path << ": INVALID (fracpack validation failed)\n";
      return false;
   }

   try {
      auto pzam = pzam_load(data);
      if (pzam.magic != PZAM_MAGIC) {
         std::cerr << path << ": INVALID (bad magic)\n";
         return false;
      }
      std::cerr << path << ": OK (v" << pzam.format_version
                << ", " << pzam.code_sections.size() << " code section(s))\n";
      return true;
   } catch (const std::exception& e) {
      std::cerr << path << ": INVALID (" << e.what() << ")\n";
      return false;
   }
}

static bool validate_wasm(const std::string& path) {
   std::vector<uint8_t> wasm_bytes;
   try {
      wasm_bytes = read_wasm(path);
   } catch (...) {
      std::cerr << "Error: cannot read: " << path << "\n";
      return false;
   }

   module mod;
   mod.allocator.use_default_memory();
   null_debug_info debug;

   using parser_t = binary_parser<null_writer, default_options, null_debug_info>;
   parser_t parser(mod.allocator, default_options{}, false, false);

#ifdef __EXCEPTIONS
   try {
      parser.parse_module(wasm_bytes, mod, debug);
      std::cerr << path << ": OK ("
                << mod.code.size() << " functions, "
                << mod.exports.size() << " exports)\n";
      return true;
   } catch (const psizam::exception& ex) {
      std::cerr << path << ": INVALID (" << ex.what() << ": " << ex.detail() << ")\n";
      return false;
   }
#else
   parser.parse_module(wasm_bytes, mod, debug);
   std::cerr << path << ": OK ("
             << mod.code.size() << " functions, "
             << mod.exports.size() << " exports)\n";
   return true;
#endif
}

int pzam_validate_main(int argc, char** argv) {
   if (argc < 2) {
      std::cerr << "Usage: pzam validate <module.wasm|module.pzam>\n";
      return 1;
   }

   std::string path = argv[1];

   // Determine file type by extension
   bool is_pzam = path.ends_with(".pzam");
   bool is_wasm = path.ends_with(".wasm");

   if (!is_pzam && !is_wasm) {
      // Try to detect by magic
      std::ifstream in(path, std::ios::binary);
      char magic[4] = {};
      in.read(magic, 4);
      if (magic[0] == 'P' && magic[1] == 'Z' && magic[2] == 'A' && magic[3] == 'M') {
         is_pzam = true;
      } else if (magic[0] == '\0' && magic[1] == 'a' && magic[2] == 's' && magic[3] == 'm') {
         is_wasm = true;
      } else {
         std::cerr << "Error: cannot determine file type (use .wasm or .pzam extension)\n";
         return 1;
      }
   }

   bool ok = is_pzam ? validate_pzam(path) : validate_wasm(path);
   return ok ? 0 : 1;
}
