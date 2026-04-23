// guest_alloc.cpp — the single translation unit that emits
// `cabi_realloc` for every wasm target in this service.
//
// <psio/guest_alloc.hpp> defines cabi_realloc as a non-inline extern
// "C" export (see the comment at the top of that header). Including
// it from any header that gets pulled into multiple TUs trips
// duplicate-symbol errors at link time. Centralizing the include
// here keeps the rule trivial to enforce: every wasm target lists
// this file in SOURCES, and no other .cpp or .hpp in the service
// reaches for the allocator definition.

#include <psio/guest_alloc.hpp>
