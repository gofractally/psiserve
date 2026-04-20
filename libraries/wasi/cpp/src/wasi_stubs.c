// wasi_stubs.c — minimal WASI function stubs for guest modules.
//
// libc++'s exception/abort paths reference fd_write even in
// -fno-exceptions mode. These stubs satisfy the linker so guests
// can use std::string, std::vector, etc. without the host needing
// to provide WASI imports.
//
// Link into guest builds via psi_add_wasm_module DEPS wasi-cpp.

#include <stdint.h>

// fd_write — write to a file descriptor (used by stderr abort messages)
int32_t __imported_wasi_snapshot_preview1_fd_write(
    int32_t fd, int32_t iovs_ptr, int32_t iovs_count, int32_t nwritten_ptr)
{
   *(int32_t*)(uintptr_t)nwritten_ptr = 0;
   return 0;
}

// fd_close — close a file descriptor
int32_t __imported_wasi_snapshot_preview1_fd_close(int32_t fd)
{
   return 0;
}

// fd_seek — seek in a file descriptor
int32_t __imported_wasi_snapshot_preview1_fd_seek(
    int32_t fd, int64_t offset, int32_t whence, int32_t newoffset_ptr)
{
   return 0;
}

// proc_exit — exit the process
_Noreturn void __imported_wasi_snapshot_preview1_proc_exit(int32_t code)
{
   __builtin_unreachable();
}
