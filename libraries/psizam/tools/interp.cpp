#include <psizam/backend.hpp>
#include <psizam/error_codes.hpp>
#include <psizam/detail/watchdog.hpp>

#include <iostream>

using namespace psizam;

/**
 * Simple implementation of an interpreter using psizam.
 */
int main(int argc, char** argv) {
   // Thread specific `allocator` used for wasm linear memory.
   wasm_allocator wa;

   if (argc < 2) {
      std::cerr << "Error, no wasm file provided\n";
      return -1;
   }

   std::string filename = argv[1];

   watchdog wd{std::chrono::seconds(3)};

   try {
      // Read the wasm into memory.
      auto code = read_wasm( filename );

      // Instaniate a new backend using the wasm provided.
      backend<std::nullptr_t, interpreter, default_options> bkend( code, &wa );

      // Execute any exported functions provided by the wasm.
      bkend.execute_all(std::move(wd));

   } catch ( const psizam::exception& ex ) {
      std::cerr << "psizam interpreter error\n";
      std::cerr << ex.what() << " : " << ex.detail() << "\n";
   }
   return 0;
}
