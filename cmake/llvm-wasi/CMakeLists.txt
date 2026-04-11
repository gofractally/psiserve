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
   # Disable tools/utils/tests
   -DLLVM_BUILD_TOOLS=OFF
   -DLLVM_BUILD_UTILS=OFF
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

ExternalProject_Add(llvm-wasi
   SOURCE_DIR        "${LLVM_SRC_DIR}"
   BINARY_DIR        "${CMAKE_BINARY_DIR}/llvm-wasi-build"
   INSTALL_DIR       "${LLVM_WASI_INSTALL_PREFIX}"
   CMAKE_ARGS        ${_LLVM_WASI_ARGS}
   # Build only libraries, not tools (llvm-tblgen can't link as WASM).
   # The .td → .inc generation uses the host llvm-tblgen via LLVM_TABLEGEN.
   BUILD_COMMAND     ${CMAKE_COMMAND} --build <BINARY_DIR>
                     --target LLVMCore
                     --target LLVMSupport
                     --target LLVMDemangle
                     --target LLVMBinaryFormat
                     --target LLVMRemarks
                     --target LLVMBitstreamReader
                     --target LLVMPasses
                     --target LLVMInstCombine
                     --target LLVMScalarOpts
                     --target LLVMX86CodeGen
                     --target LLVMX86AsmParser
                     --target LLVMX86Desc
                     --target LLVMX86Info
                     --target LLVMAArch64CodeGen
                     --target LLVMAArch64AsmParser
                     --target LLVMAArch64Desc
                     --target LLVMAArch64Info
                     --target LLVMObject
                     --target LLVMCodeGen
                     --target LLVMTarget
                     --target LLVMAnalysis
                     --target LLVMTransformUtils
                     --target LLVMipo
                     --target LLVMVectorize
                     --target LLVMMC
                     --target LLVMMCParser
                     --target LLVMMCDisassembler
                     --target LLVMAsmPrinter
                     --target LLVMSelectionDAG
                     --target LLVMGlobalISel
                     --target LLVMCodeGenTypes
                     --target LLVMIRReader
                     --target LLVMAsmParser
                     --target LLVMBitReader
                     --target LLVMBitWriter
                     --target LLVMLinker
                     --target LLVMProfileData
                     --target LLVMTextAPI
                     --target LLVMDebugInfoDWARF
                     --target LLVMDebugInfoCodeView
                     --target LLVMCFGuard
                     --target LLVMIRPrinter
                     --target LLVMInstrumentation
                     --target LLVMObjCARCOpts
                     --target LLVMCoroutines
                     --target LLVMAggressiveInstCombine
                     --target LLVMHipStdPar
   INSTALL_COMMAND   ${CMAKE_COMMAND} --install <BINARY_DIR>
   DEPENDS           ${_LLVM_WASI_DEPS}
)
