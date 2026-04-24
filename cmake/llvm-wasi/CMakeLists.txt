# Standalone CMake project to cross-compile LLVM static libraries for WASI.
#
# This downloads LLVM source, builds a native llvm-tblgen, then cross-compiles
# the LLVM libraries needed by pzam-compile to wasm32-wasip1.
#
# Usage:
#   cmake -B build/llvm-wasi -S cmake/llvm-wasi \
#         -DWASI_TOOLCHAIN_FILE=$(pwd)/cmake/llvm-wasi/wasi-llvm-toolchain.cmake
#   cmake --build build/llvm-wasi
#
# Or use the wrapper script:
#   scripts/build-llvm-wasi.sh

cmake_minimum_required(VERSION 3.20)
project(llvm-wasi-build NONE)

include(ExternalProject)
include(FetchContent)

# ---------- configuration ----------

set(LLVM_VERSION "22.1.2" CACHE STRING "LLVM version the fork is based on")

# We pull LLVM from the gofractally/llvm-project fork rather than upstream.
# The fork's psi/v<ver> tag carries:
#   - #if LLVM_ENABLE_THREADS guards on Support/DebugInfo headers that
#     unconditionally included <mutex>/<condition_variable>/<shared_mutex>
#     (small upstream-style cleanups).
#   - The wasm32-*-p16 ABI variant: 16-bit pointer ABI on the unchanged
#     wasm32 ISA (DataLayout + Clang TargetInfo).
# See https://github.com/gofractally/llvm-project/tree/psi/22.1.2 .
set(LLVM_URL "https://github.com/gofractally/llvm-project/archive/refs/tags/psi/v${LLVM_VERSION}.tar.gz"
    CACHE STRING "URL to LLVM source tarball (psiserve fork)")

set(WASI_TOOLCHAIN_FILE "" CACHE FILEPATH "Path to WASI CMake toolchain file (required)")
if(NOT WASI_TOOLCHAIN_FILE)
   message(FATAL_ERROR "WASI_TOOLCHAIN_FILE must be set")
endif()

set(LLVM_WASI_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/install" CACHE PATH "Installation prefix for LLVM WASI libraries")

# Optional: path to a pre-built host llvm-tblgen (skips host build)
set(LLVM_HOST_TBLGEN "" CACHE FILEPATH "Path to host llvm-tblgen binary (optional, skips host build)")

# Optional: also build clang.wasm + wasm-ld.wasm (front end for source-to-pzam).
# Separate LLVM build: different LLVM_ENABLE_PROJECTS (clang+lld) and
# LLVM_TARGETS_TO_BUILD (WebAssembly, so clang emits .wasm) vs the AOT lib build.
option(LLVM_WASI_BUILD_CLANG "Also build clang.wasm + wasm-ld.wasm for the source-to-pzam toolchain" OFF)

# Optional: path to a pre-built host clang-tblgen (skips host build for clang)
set(CLANG_HOST_TBLGEN "" CACHE FILEPATH "Path to host clang-tblgen binary (optional, skips host build)")

# LTO mode for the WASI cross-build. ThinLTO across LLVM's ~500 .cpp files
# cuts clang.wasm's static size and working-set materially — the default
# MinSizeRel build leaves a lot of never-inlined template instantiations
# and duplicated small functions across .a archives. Options: OFF, Thin,
# Full. Full is slower to link and (empirically) not much better than Thin
# for LLVM itself. Set to OFF for fast iteration.
set(LLVM_WASI_LTO "Thin" CACHE STRING "LTO mode for WASI-targeted LLVM/clang (OFF, Thin, Full)")
set_property(CACHE LLVM_WASI_LTO PROPERTY STRINGS OFF Thin Full)

# ThinLTO's final link spins up N=nproc parallel optimizer workers, each
# holding much of the clang module graph. On small-RAM CI runners (7–16 GB)
# that OOMs. Empty (default) means "use LLVM's default = nproc"; set to 1
# or 2 on memory-constrained hosts. Affects link wall-clock, not the size
# or speed of the resulting artifact.
set(LLVM_WASI_LINK_JOBS "" CACHE STRING
   "Parallel link jobs for the LLVM cross-build (blank = nproc)")

# Swap wasi-libc's default dlmalloc for mimalloc on the wasm clang/lld
# binaries. dlmalloc fragments badly under clang's long-running, mixed-size
# allocation pattern (AST + IR + analysis passes), which pushes the wasm
# linear memory past 4 GB on non-trivial C++ compiles. mimalloc's segregated
# free lists + page reclaim keep the working set much tighter. OFF means
# keep wasi-libc's bundled malloc.
option(LLVM_WASI_USE_MIMALLOC "Link mimalloc ahead of wasi-libc's malloc in clang.wasm / wasm-ld.wasm" ON)
set(MIMALLOC_VERSION "2.1.7" CACHE STRING "mimalloc version when LLVM_WASI_USE_MIMALLOC=ON")
set(MIMALLOC_URL
    "https://github.com/microsoft/mimalloc/archive/refs/tags/v${MIMALLOC_VERSION}.tar.gz"
    CACHE STRING "URL to mimalloc source tarball")

# ---------- download LLVM source ----------

message(STATUS "Downloading LLVM ${LLVM_VERSION} source...")
FetchContent_Declare(llvm_src
   URL ${LLVM_URL}
   DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(llvm_src)

set(LLVM_SRC_DIR "${llvm_src_SOURCE_DIR}/llvm")
message(STATUS "LLVM source: ${LLVM_SRC_DIR}")

# ---------- Phase 1: host llvm-tblgen ----------

if(LLVM_HOST_TBLGEN)
   set(_HOST_TBLGEN "${LLVM_HOST_TBLGEN}")
   message(STATUS "Using provided host llvm-tblgen: ${_HOST_TBLGEN}")
else()
   # Try to find a system llvm-tblgen with matching version
   find_program(_SYS_TBLGEN llvm-tblgen)
   if(_SYS_TBLGEN)
      execute_process(
         COMMAND ${_SYS_TBLGEN} --version
         OUTPUT_VARIABLE _tblgen_ver
         OUTPUT_STRIP_TRAILING_WHITESPACE
         ERROR_QUIET
      )
      if(_tblgen_ver MATCHES "${LLVM_VERSION}")
         set(_HOST_TBLGEN "${_SYS_TBLGEN}")
         message(STATUS "Using system llvm-tblgen (version matches): ${_HOST_TBLGEN}")
      else()
         message(STATUS "System llvm-tblgen version mismatch (${_tblgen_ver}), building from source")
         unset(_SYS_TBLGEN)
      endif()
   endif()

   if(NOT _HOST_TBLGEN)
      message(STATUS "Building host llvm-tblgen from source...")
      ExternalProject_Add(llvm-host
         SOURCE_DIR        "${LLVM_SRC_DIR}"
         BINARY_DIR        "${CMAKE_BINARY_DIR}/llvm-host-build"
         INSTALL_COMMAND    ""
         CMAKE_ARGS
            -DCMAKE_BUILD_TYPE=Release
            -DLLVM_TARGETS_TO_BUILD=X86$<SEMICOLON>AArch64
            -DLLVM_BUILD_TOOLS=ON
            -DLLVM_BUILD_UTILS=OFF
            -DLLVM_INCLUDE_TESTS=OFF
            -DLLVM_INCLUDE_BENCHMARKS=OFF
            -DLLVM_INCLUDE_EXAMPLES=OFF
            -DLLVM_INCLUDE_DOCS=OFF
            -DLLVM_ENABLE_BINDINGS=OFF
         BUILD_COMMAND
            ${CMAKE_COMMAND} --build <BINARY_DIR> --target llvm-tblgen
      )
      set(_HOST_TBLGEN "${CMAKE_BINARY_DIR}/llvm-host-build/bin/llvm-tblgen")
   endif()
endif()

# ---------- Phase 2: cross-compile LLVM for WASI ----------

set(_LLVM_WASI_ARGS
   -DCMAKE_TOOLCHAIN_FILE=${WASI_TOOLCHAIN_FILE}
   -DCMAKE_INSTALL_PREFIX=${LLVM_WASI_INSTALL_PREFIX}
   -DCMAKE_BUILD_TYPE=MinSizeRel
   -DLLVM_TABLEGEN=${_HOST_TBLGEN}
   -DCMAKE_CROSSCOMPILING=ON
   # WASI is Unix-like; LLVM needs UNIX=ON to select Unix code paths
   -DUNIX=ON
   # Target triple for WASI (not the host triple)
   -DLLVM_HOST_TRIPLE=wasm32-wasip1
   -DLLVM_DEFAULT_TARGET_TRIPLE=wasm32-wasip1
   # Target backends (codegen targets, not the host)
   -DLLVM_TARGETS_TO_BUILD=X86$<SEMICOLON>AArch64
   -DLLVM_ENABLE_PROJECTS=
   # Disable tools/utils/tests. INCLUDE_TOOLS=OFF (in addition to BUILD_TOOLS=OFF)
   # is required: BUILD_TOOLS only gates install, but INCLUDE_TOOLS skips
   # add_subdirectory(tools) entirely. Without it, fuzzer binaries under
   # tools/llvm-*-fuzzer/ get built and fail to link against wasi-libc
   # (missing sigprocmask, posix_madvise, etc).
   -DLLVM_BUILD_TOOLS=OFF
   -DLLVM_INCLUDE_TOOLS=OFF
   -DLLVM_BUILD_UTILS=OFF
   -DLLVM_INCLUDE_UTILS=OFF
   -DLLVM_INCLUDE_TESTS=OFF
   -DLLVM_INCLUDE_BENCHMARKS=OFF
   -DLLVM_INCLUDE_EXAMPLES=OFF
   -DLLVM_INCLUDE_DOCS=OFF
   -DLLVM_BUILD_LLVM_DYLIB=OFF
   -DLLVM_LINK_LLVM_DYLIB=OFF
   -DLLVM_ENABLE_PLUGINS=OFF
   -DLLVM_ENABLE_BINDINGS=OFF
   # Disable external dependencies
   -DLLVM_ENABLE_THREADS=OFF
   -DLLVM_ENABLE_ZLIB=OFF
   -DLLVM_ENABLE_ZSTD=OFF
   -DLLVM_ENABLE_TERMINFO=OFF
   -DLLVM_ENABLE_LIBXML2=OFF
   -DLLVM_ENABLE_LIBEDIT=OFF
   -DLLVM_ENABLE_LIBPFM=OFF
   -DLLVM_ENABLE_Z3_SOLVER=OFF
   # C++ settings
   -DLLVM_ENABLE_RTTI=OFF
   -DLLVM_ENABLE_EH=OFF
   # Not needed in sandbox
   -DLLVM_ENABLE_CRASH_OVERRIDES=OFF
   -DLLVM_ENABLE_BACKTRACES=OFF
   -DLLVM_ENABLE_PIC=OFF
   -DLLVM_ENABLE_PEDANTIC=OFF
   -DLLVM_ENABLE_WARNINGS=OFF
   -DLLVM_CCACHE_BUILD=ON
)
if(NOT LLVM_WASI_LTO STREQUAL "OFF")
   list(APPEND _LLVM_WASI_ARGS -DLLVM_ENABLE_LTO=${LLVM_WASI_LTO})
endif()
if(LLVM_WASI_LINK_JOBS)
   list(APPEND _LLVM_WASI_ARGS -DLLVM_PARALLEL_LINK_JOBS=${LLVM_WASI_LINK_JOBS})
endif()

# Determine dependencies
if(TARGET llvm-host)
   set(_LLVM_WASI_DEPS llvm-host)
else()
   set(_LLVM_WASI_DEPS "")
endif()

# Use LLVM's per-library install-<Name> targets rather than the global
# `cmake --install`. The global install tries to copy every library LLVM
# defines regardless of what we built, which breaks whenever a new release
# adds a library (LLVMSupportLSP in 22.1, LLVMTableGen referenced from its
# own cmake_install.cmake, etc.). Listing specific install-* targets gives
# us exactly the libs we built plus LLVM's cmake package and headers.
#
# The set below matches what psizam-llvm actually links against (X86 + AArch64
# AOT codegen), plus transitive deps. Tools are already suppressed by
# LLVM_BUILD_TOOLS=OFF / LLVM_INCLUDE_TOOLS=OFF above.
ExternalProject_Add(llvm-wasi
   SOURCE_DIR        "${LLVM_SRC_DIR}"
   BINARY_DIR        "${CMAKE_BINARY_DIR}/llvm-wasi-build"
   INSTALL_DIR       "${LLVM_WASI_INSTALL_PREFIX}"
   CMAKE_ARGS        ${_LLVM_WASI_ARGS}
   BUILD_COMMAND     ${CMAKE_COMMAND} --build <BINARY_DIR>
                     --target install-cmake-exports
                     --target install-llvm-headers
                     --target install-LLVMCore
                     --target install-LLVMSupport
                     --target install-LLVMDemangle
                     --target install-LLVMBinaryFormat
                     --target install-LLVMRemarks
                     --target install-LLVMBitstreamReader
                     --target install-LLVMTargetParser
                     --target install-LLVMPasses
                     --target install-LLVMInstCombine
                     --target install-LLVMScalarOpts
                     --target install-LLVMX86CodeGen
                     --target install-LLVMX86AsmParser
                     --target install-LLVMX86Desc
                     --target install-LLVMX86Info
                     --target install-LLVMAArch64CodeGen
                     --target install-LLVMAArch64AsmParser
                     --target install-LLVMAArch64Desc
                     --target install-LLVMAArch64Info
                     --target install-LLVMAArch64Utils
                     --target install-LLVMObject
                     --target install-LLVMCodeGen
                     --target install-LLVMTarget
                     --target install-LLVMAnalysis
                     --target install-LLVMTransformUtils
                     --target install-LLVMipo
                     --target install-LLVMVectorize
                     --target install-LLVMMC
                     --target install-LLVMMCParser
                     --target install-LLVMMCDisassembler
                     --target install-LLVMAsmPrinter
                     --target install-LLVMSelectionDAG
                     --target install-LLVMGlobalISel
                     --target install-LLVMCodeGenTypes
                     --target install-LLVMIRReader
                     --target install-LLVMAsmParser
                     --target install-LLVMBitReader
                     --target install-LLVMBitWriter
                     --target install-LLVMLinker
                     --target install-LLVMProfileData
                     --target install-LLVMTextAPI
                     --target install-LLVMDebugInfoDWARF
                     --target install-LLVMDebugInfoDWARFLowLevel
                     --target install-LLVMDebugInfoCodeView
                     --target install-LLVMDebugInfoBTF
                     --target install-LLVMDebugInfoMSF
                     --target install-LLVMCFGuard
                     --target install-LLVMIRPrinter
                     --target install-LLVMInstrumentation
                     --target install-LLVMObjCARCOpts
                     --target install-LLVMCoroutines
                     --target install-LLVMAggressiveInstCombine
                     --target install-LLVMHipStdPar
                     --target install-LLVMFrontendHLSL
                     --target install-LLVMSandboxIR
   # Install already done by the install-* targets in BUILD_COMMAND.
   INSTALL_COMMAND   ""
   DEPENDS           ${_LLVM_WASI_DEPS}
)

# ---------- Phase 2b: WASI stubs static library ----------
#
# wasi_llvm_stubs.c provides POSIX stubs (sigaction, getrlimit, getpwuid_r, ...)
# that LLVM's Support library references but wasi-libc doesn't provide.
# Needed by clang-wasi (produces an executable) but not by llvm-wasi itself
# (only produces static archives — undefined symbols don't fail archive builds).
#
# Built as a standalone sub-project via ExternalProject_Add using the same
# WASI toolchain, then installed as libwasi_llvm_stubs.a alongside the LLVM
# libraries so clang-wasi can link -lwasi_llvm_stubs.
ExternalProject_Add(wasi-llvm-stubs
   SOURCE_DIR        "${CMAKE_CURRENT_SOURCE_DIR}/stubs-project"
   BINARY_DIR        "${CMAKE_BINARY_DIR}/wasi-llvm-stubs-build"
   INSTALL_DIR       "${LLVM_WASI_INSTALL_PREFIX}"
   CMAKE_ARGS
      -DCMAKE_TOOLCHAIN_FILE=${WASI_TOOLCHAIN_FILE}
      -DCMAKE_INSTALL_PREFIX=${LLVM_WASI_INSTALL_PREFIX}
      -DCMAKE_BUILD_TYPE=MinSizeRel
)

# ---------- Phase 3: (optional) clang + wasm-ld cross-compiled to WASI ----------
#
# Produces clang.wasm and wasm-ld.wasm as WASI executables. These are the
# front-end artifacts for the source-to-pzam toolchain (plans/source-to-pzam.md).
#
# Separate from llvm-wasi above because:
#   - LLVM_ENABLE_PROJECTS=clang;lld (vs none in the lib build)
#   - LLVM_TARGETS_TO_BUILD=WebAssembly (clang emits .wasm, vs X86+AArch64
#     native codegen inside pzam-compile.wasm)
#   - LLVM_BUILD_TOOLS=ON (vs OFF in the lib build)
#
# Both builds share the same LLVM source tree (downloaded once) and the same
# host llvm-tblgen.
if(LLVM_WASI_BUILD_CLANG)

   # ---------- Optional: mimalloc for wasm32-wasip1 ----------
   #
   # clang does a lot of small, mixed-size, long-lived allocations (AST
   # nodes, IR, pass state). wasi-libc's bundled dlmalloc fragments badly
   # under that workload — enough that a hello-world with #include <string>
   # pushes the linear memory past the 4 GB wasm32 ceiling on non-trivial
   # C++. mimalloc's segregated free lists + page-level reclaim keep the
   # working set tight. We cross-compile the library once, then the
   # toolchain file links it ahead of libc.a for clang-wasi and wasm-ld.
   set(_MIMALLOC_LIB_DIR "")
   if(LLVM_WASI_USE_MIMALLOC)
      set(_MIMALLOC_PREFIX "${CMAKE_BINARY_DIR}/mimalloc-install")
      # mimalloc installs to lib/mimalloc-<major>.<minor>/libmimalloc.a, not
      # lib/libmimalloc.a. Use the versioned subdir so -lmimalloc resolves.
      string(REGEX MATCH "^[0-9]+\\.[0-9]+" _MI_MAJOR_MINOR "${MIMALLOC_VERSION}")
      set(_MIMALLOC_LIB_DIR "${_MIMALLOC_PREFIX}/lib/mimalloc-${_MI_MAJOR_MINOR}")
      # ExternalProject_Add handles download + extract + cross-configure +
      # build + install in one go. No configure-time populate required.
      ExternalProject_Add(mimalloc-wasi
         URL          "${MIMALLOC_URL}"
         PREFIX       "${CMAKE_BINARY_DIR}/mimalloc"
         BINARY_DIR   "${CMAKE_BINARY_DIR}/mimalloc-build"
         INSTALL_DIR  "${_MIMALLOC_PREFIX}"
         DOWNLOAD_EXTRACT_TIMESTAMP TRUE
         CMAKE_ARGS
            -DCMAKE_TOOLCHAIN_FILE=${WASI_TOOLCHAIN_FILE}
            -DCMAKE_INSTALL_PREFIX=${_MIMALLOC_PREFIX}
            -DCMAKE_BUILD_TYPE=Release
            # WASI: no threads, no TLS, no _Atomic pthread, no siginfo.
            -DMI_BUILD_SHARED=OFF
            -DMI_BUILD_STATIC=ON
            -DMI_BUILD_TESTS=OFF
            -DMI_BUILD_OBJECT=OFF
            -DMI_USE_CXX=OFF
            # MI_OVERRIDE=ON compiles alloc-override.c, which defines strong
            # malloc/free/calloc/realloc/aligned_alloc/posix_memalign. Without
            # it the archive only exports mi_* symbols, the LLVM link never
            # references them, and wasi-libc's dlmalloc silently wins. ON is
            # the right flag for replacement, but as of mimalloc 2.1.7 the
            # resulting binary traps at startup on wasm32-wasip1 — the
            # __attribute__((constructor)) in init.c calls atexit + getenv
            # and hits a null-deref at 0xffffffff before main() runs. Leaving
            # OFF for now so clang.wasm starts; LTO alone already keeps the
            # peak under the 4 GB wasm32 ceiling for C hello worlds. Revisit
            # if C++ <string> compile becomes the forcing function.
            -DMI_OVERRIDE=OFF
            -DMI_LIBC_MUSL=ON
            -DMI_OPT_ARCH=OFF
            -DMI_NO_THP=ON
            -DMI_WIN_REDIRECT=OFF
            # mimalloc wants mmap; wasi-libc supplies it via _WASI_EMULATED_MMAN
            # (toolchain file already adds -lwasi-emulated-mman).
            "-DCMAKE_C_FLAGS=-D_WASI_EMULATED_MMAN -DMI_OSX_ZONE=0 -DMI_PADDING=0 -DMI_DEBUG=0"
      )
   endif()


   # clang needs its own tblgen host binary (clang-tblgen).
   if(CLANG_HOST_TBLGEN)
      set(_HOST_CLANG_TBLGEN "${CLANG_HOST_TBLGEN}")
      message(STATUS "Using provided host clang-tblgen: ${_HOST_CLANG_TBLGEN}")
   else()
      find_program(_SYS_CLANG_TBLGEN clang-tblgen)
      if(_SYS_CLANG_TBLGEN)
         execute_process(
            COMMAND ${_SYS_CLANG_TBLGEN} --version
            OUTPUT_VARIABLE _ctblgen_ver
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
         )
         if(_ctblgen_ver MATCHES "${LLVM_VERSION}")
            set(_HOST_CLANG_TBLGEN "${_SYS_CLANG_TBLGEN}")
            message(STATUS "Using system clang-tblgen (version matches): ${_HOST_CLANG_TBLGEN}")
         else()
            message(STATUS "System clang-tblgen version mismatch (${_ctblgen_ver}), building from source")
            unset(_SYS_CLANG_TBLGEN)
         endif()
      endif()

      if(NOT _HOST_CLANG_TBLGEN)
         message(STATUS "Building host clang-tblgen from source...")
         # Separate host clang build (can't reuse llvm-host because that one doesn't
         # enable the clang project).
         ExternalProject_Add(clang-host
            SOURCE_DIR        "${LLVM_SRC_DIR}"
            BINARY_DIR        "${CMAKE_BINARY_DIR}/clang-host-build"
            INSTALL_COMMAND   ""
            CMAKE_ARGS
               -DCMAKE_BUILD_TYPE=Release
               -DLLVM_ENABLE_PROJECTS=clang
               -DLLVM_TARGETS_TO_BUILD=WebAssembly
               -DLLVM_BUILD_TOOLS=ON
               -DLLVM_BUILD_UTILS=OFF
               -DLLVM_INCLUDE_TESTS=OFF
               -DLLVM_INCLUDE_BENCHMARKS=OFF
               -DLLVM_INCLUDE_EXAMPLES=OFF
               -DLLVM_INCLUDE_DOCS=OFF
               -DLLVM_ENABLE_BINDINGS=OFF
            BUILD_COMMAND
               ${CMAKE_COMMAND} --build <BINARY_DIR> --target clang-tblgen
         )
         set(_HOST_CLANG_TBLGEN "${CMAKE_BINARY_DIR}/clang-host-build/bin/clang-tblgen")
      endif()
   endif()

   set(_CLANG_WASI_ARGS
      -DCMAKE_TOOLCHAIN_FILE=${WASI_TOOLCHAIN_FILE}
      -DCMAKE_INSTALL_PREFIX=${LLVM_WASI_INSTALL_PREFIX}
      -DCMAKE_BUILD_TYPE=MinSizeRel
      # Toolchain opt-in: adds -L.../lib -lwasi_llvm_stubs to the default
      # executable link line. Keeps the toolchain's -lc++ -lc++abi defaults
      # intact (a full -DCMAKE_EXE_LINKER_FLAGS override would drop them).
      -DWASI_LINK_STUBS=ON
      -DWASI_LLVM_STUBS_DIR=${LLVM_WASI_INSTALL_PREFIX}/lib
      -DLLVM_TABLEGEN=${_HOST_TBLGEN}
      -DCLANG_TABLEGEN=${_HOST_CLANG_TBLGEN}
      -DCMAKE_CROSSCOMPILING=ON
      -DUNIX=ON
      -DLLVM_HOST_TRIPLE=wasm32-wasip1
      -DLLVM_DEFAULT_TARGET_TRIPLE=wasm32-wasip1
      # clang generates WASM code (source-to-pzam produces .wasm as intermediate)
      -DLLVM_TARGETS_TO_BUILD=WebAssembly
      # Enable clang + lld
      -DLLVM_ENABLE_PROJECTS=clang$<SEMICOLON>lld
      # Build tools (clang executable, wasm-ld executable)
      -DLLVM_BUILD_TOOLS=ON
      -DLLVM_BUILD_UTILS=OFF
      -DCLANG_BUILD_TOOLS=ON
      -DCLANG_ENABLE_STATIC_ANALYZER=OFF
      -DCLANG_ENABLE_ARCMT=OFF
      -DCLANG_PLUGIN_SUPPORT=OFF
      # WASI has no fork/exec — clang's driver must invoke cc1 in-process.
      -DCLANG_DEFAULT_CC1_INPROCESS=ON
      -DLLVM_INCLUDE_TESTS=OFF
      -DLLVM_INCLUDE_BENCHMARKS=OFF
      -DLLVM_INCLUDE_EXAMPLES=OFF
      -DLLVM_INCLUDE_DOCS=OFF
      -DLLVM_BUILD_LLVM_DYLIB=OFF
      -DLLVM_LINK_LLVM_DYLIB=OFF
      -DLLVM_ENABLE_PLUGINS=OFF
      -DLLVM_ENABLE_BINDINGS=OFF
      -DLLVM_ENABLE_THREADS=OFF
      -DLLVM_ENABLE_ZLIB=OFF
      -DLLVM_ENABLE_ZSTD=OFF
      -DLLVM_ENABLE_TERMINFO=OFF
      -DLLVM_ENABLE_LIBXML2=OFF
      -DLLVM_ENABLE_LIBEDIT=OFF
      -DLLVM_ENABLE_LIBPFM=OFF
      -DLLVM_ENABLE_Z3_SOLVER=OFF
      -DLLVM_ENABLE_RTTI=OFF
      -DLLVM_ENABLE_EH=OFF
      -DLLVM_ENABLE_CRASH_OVERRIDES=OFF
      -DLLVM_ENABLE_BACKTRACES=OFF
      -DLLVM_ENABLE_PIC=OFF
      -DLLVM_ENABLE_PEDANTIC=OFF
      -DLLVM_ENABLE_WARNINGS=OFF
      -DLLVM_CCACHE_BUILD=ON
   )
   if(NOT LLVM_WASI_LTO STREQUAL "OFF")
      list(APPEND _CLANG_WASI_ARGS -DLLVM_ENABLE_LTO=${LLVM_WASI_LTO})
   endif()
   if(LLVM_WASI_LINK_JOBS)
      list(APPEND _CLANG_WASI_ARGS -DLLVM_PARALLEL_LINK_JOBS=${LLVM_WASI_LINK_JOBS})
   endif()
   if(LLVM_WASI_USE_MIMALLOC)
      list(APPEND _CLANG_WASI_ARGS
         -DWASI_LINK_MIMALLOC=ON
         -DWASI_MIMALLOC_DIR=${_MIMALLOC_LIB_DIR})
   endif()

   set(_CLANG_WASI_DEPS wasi-llvm-stubs)
   if(LLVM_WASI_USE_MIMALLOC)
      list(APPEND _CLANG_WASI_DEPS mimalloc-wasi)
   endif()
   if(TARGET llvm-host)
      list(APPEND _CLANG_WASI_DEPS llvm-host)
   endif()
   if(TARGET clang-host)
      list(APPEND _CLANG_WASI_DEPS clang-host)
   endif()

   # Target names: clang produces bin/clang-22 (real binary, no .wasm suffix —
   # LLVM's add_llvm_executable overrides CMAKE_EXECUTABLE_SUFFIX). lld is a
   # single multi-tool binary dispatched on argv[0] (wasm-ld / ld.lld /
   # lld-link / ld64.lld). Install copies both to <prefix>/bin/ with stable
   # .wasm filenames; source-to-pzam invokes them via pzam-run, which sets
   # argv[0] to the basename, so wasm-ld.wasm triggers the wasm-ld dispatch.
   ExternalProject_Add(clang-wasi
      SOURCE_DIR        "${LLVM_SRC_DIR}"
      BINARY_DIR        "${CMAKE_BINARY_DIR}/clang-wasi-build"
      INSTALL_DIR       "${LLVM_WASI_INSTALL_PREFIX}"
      CMAKE_ARGS        ${_CLANG_WASI_ARGS}
      BUILD_COMMAND     ${CMAKE_COMMAND} --build <BINARY_DIR>
                        --target clang
                        --target lld
      INSTALL_COMMAND   ${CMAKE_COMMAND} -E make_directory <INSTALL_DIR>/bin
              COMMAND   ${CMAKE_COMMAND} -E copy
                        <BINARY_DIR>/bin/clang-22
                        <INSTALL_DIR>/bin/clang.wasm
              COMMAND   ${CMAKE_COMMAND} -E copy
                        <BINARY_DIR>/bin/lld
                        <INSTALL_DIR>/bin/wasm-ld.wasm
      DEPENDS           ${_CLANG_WASI_DEPS}
   )
endif()
