// Stub definitions of runtime helper functions for WASI compile-only builds.
//
// These functions are never called at runtime in the compiler — only their
// addresses are taken by the JIT code generators and embedded as relocatable
// pointers in the generated native code. The relocation system records
// symbol IDs, and the actual addresses are patched at load time on the
// target platform.
//
// Providing stub definitions satisfies the linker without pulling in the
// full runtime_helpers.cpp (which depends on execution_context, etc.).

#include <cstdint>
#include <cstdlib>

extern "C" {

const void* __psizam_resolve_indirect(void*, uint32_t, uint32_t, uint32_t) {
   __builtin_trap();
}

uint32_t __psizam_table_get(void*, uint32_t, uint32_t) {
   __builtin_trap();
}

void __psizam_table_set(void*, uint32_t, uint32_t, uint32_t) {
   __builtin_trap();
}

uint32_t __psizam_table_grow(void*, uint32_t, uint32_t, uint32_t) {
   __builtin_trap();
}

uint32_t __psizam_table_size(void*, uint32_t) {
   __builtin_trap();
}

void __psizam_table_fill(void*, uint32_t, uint32_t, uint32_t, uint32_t) {
   __builtin_trap();
}

uint64_t __psizam_atomic_rmw(void*, uint8_t, uint32_t, uint32_t, uint64_t, uint64_t) {
   __builtin_trap();
}

} // extern "C"
