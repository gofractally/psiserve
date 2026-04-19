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

set(LLVM_VERSION "22.1.2" CACHE STRING "LLVM version to build")
set(LLVM_URL "https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VERSION}/llvm-project-${LLVM_VERSION}.src.tar.xz"
    CACHE STRING "URL to LLVM source tarball")

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
)

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
   )

   set(_CLANG_WASI_DEPS wasi-llvm-stubs)
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
