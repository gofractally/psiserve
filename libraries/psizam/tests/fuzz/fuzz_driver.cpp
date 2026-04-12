#include <psizam/backend.hpp>
#include <psizam/detail/watchdog.hpp>

using namespace psizam;
using namespace psizam::detail;

extern "C" int LLVMFuzzerTestOneInput( const uint8_t* data, size_t size ) {
   wasm_allocator wa;
   wasm_code wc; 
   wc.resize(size);
   memcpy((uint8_t*)wc.data(), data, size);
   backend<std::nullptr_t> bkend( wc );
   bkend.execute_all(null_watchdog());
}
