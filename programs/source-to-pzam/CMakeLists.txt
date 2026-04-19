# source-to-pzam: host-side driver for deterministic C++ → .pzam toolchain.
#
# Produces a native CLI that invokes pzam-run (under psizam) for the clang and
# wasm-ld stages, then pzam-compile for the final .wasm → .pzam stage.
#
# Configure-time defaults baked in via -D flags. Runtime flags / env vars
# override. The target depends on pzam-run and pzam-compile so ordinary
# `cmake --build` from the top-level puts everything in build/<cfg>/bin/.

# Auto-detect defaults when the user hasn't specified them. The plain
# `-DSOURCE_TO_PZAM_DEFAULT_* = ...` cache vars still take precedence.

# 1. Tool dir: the build's own bin/ — where pzam-run/pzam-compile/psizam-wasi land.
#    clang.wasm / wasm-ld.wasm get copied here below (if llvm-wasi built them).
#    (CMAKE_RUNTIME_OUTPUT_DIRECTORY doesn't propagate to subdirectories at
#    configure time reliably, so just reconstruct the path the top-level sets.)
set(_S2P_AUTO_TOOLDIR "${CMAKE_BINARY_DIR}/bin")

# 2. WASI sysroot paths: on macOS, Homebrew splits the toolchain across three
#    roots. Auto-detect the canonical Homebrew layout when present. Linux users
#    typically point --sysroot at wasi-sdk which has everything under one root.
set(_S2P_AUTO_SYSROOT "")
set(_S2P_AUTO_CXXSYSROOT "")
set(_S2P_AUTO_RESOURCE_DIR "")
set(_S2P_AUTO_CLANG_RT_DIR "")
if(EXISTS "/opt/homebrew/opt/wasi-libc/share/wasi-sysroot")
   set(_S2P_AUTO_SYSROOT "/opt/homebrew/opt/wasi-libc/share/wasi-sysroot")
endif()
if(EXISTS "/opt/homebrew/opt/wasi-runtimes/share/wasi-sysroot")
   set(_S2P_AUTO_CXXSYSROOT "/opt/homebrew/opt/wasi-runtimes/share/wasi-sysroot")
endif()
if(EXISTS "/opt/homebrew/opt/wasi-runtimes/share/wasi-runtimes/lib")
   set(_S2P_AUTO_CLANG_RT_DIR "/opt/homebrew/opt/wasi-runtimes/share/wasi-runtimes/lib")
endif()
# Pick the newest clang resource-dir under Homebrew LLVM, if any.
file(GLOB _S2P_LLVM_RESOURCE_DIRS "/opt/homebrew/opt/llvm/lib/clang/*")
if(_S2P_LLVM_RESOURCE_DIRS)
   list(SORT _S2P_LLVM_RESOURCE_DIRS COMPARE NATURAL ORDER DESCENDING)
   list(GET _S2P_LLVM_RESOURCE_DIRS 0 _S2P_AUTO_RESOURCE_DIR)
endif()

set(SOURCE_TO_PZAM_DEFAULT_TOOLDIR "${_S2P_AUTO_TOOLDIR}" CACHE PATH
   "Compiled-in default directory containing pzam-run, pzam-compile, psizam-wasi, clang.wasm, wasm-ld.wasm")
set(SOURCE_TO_PZAM_DEFAULT_SYSROOT "${_S2P_AUTO_SYSROOT}" CACHE PATH
   "Compiled-in default wasi-sysroot directory (C libs, crt1-command.o)")
set(SOURCE_TO_PZAM_DEFAULT_CXXSYSROOT "${_S2P_AUTO_CXXSYSROOT}" CACHE PATH
   "Compiled-in default C++ sysroot (libc++, libc++abi, C++ headers)")
set(SOURCE_TO_PZAM_DEFAULT_RESOURCE_DIR "${_S2P_AUTO_RESOURCE_DIR}" CACHE PATH
   "Compiled-in default LLVM clang resource dir (builtin headers)")
set(SOURCE_TO_PZAM_DEFAULT_CLANG_RT_DIR "${_S2P_AUTO_CLANG_RT_DIR}" CACHE PATH
   "Compiled-in default directory holding libclang_rt.builtins.a")

add_executable(source-to-pzam source_to_pzam.cpp)

target_compile_definitions(source-to-pzam PRIVATE
   SOURCE_TO_PZAM_DEFAULT_TOOLDIR="${SOURCE_TO_PZAM_DEFAULT_TOOLDIR}"
   SOURCE_TO_PZAM_DEFAULT_SYSROOT="${SOURCE_TO_PZAM_DEFAULT_SYSROOT}"
   SOURCE_TO_PZAM_DEFAULT_CXXSYSROOT="${SOURCE_TO_PZAM_DEFAULT_CXXSYSROOT}"
   SOURCE_TO_PZAM_DEFAULT_RESOURCE_DIR="${SOURCE_TO_PZAM_DEFAULT_RESOURCE_DIR}"
   SOURCE_TO_PZAM_DEFAULT_CLANG_RT_DIR="${SOURCE_TO_PZAM_DEFAULT_CLANG_RT_DIR}"
)

# Must wait for the tools we shell out to.
foreach(_dep pzam-run pzam-compile psizam-wasi)
   if(TARGET ${_dep})
      add_dependencies(source-to-pzam ${_dep})
   endif()
endforeach()

# End-to-end CTest: compile hello.cc → hello.pzam → run, expect "hello world".
# Skipped automatically when the toolchain pieces aren't available (Linux CI
# without Homebrew paths, or an llvm-wasi build that hasn't been run yet).
if(EXISTS "${SOURCE_TO_PZAM_DEFAULT_SYSROOT}" AND
   EXISTS "${LLVM_WASI_INSTALL}/bin/clang.wasm")
   set(_s2p_test_script "${CMAKE_CURRENT_BINARY_DIR}/source_to_pzam_hello_test.sh")
   file(WRITE "${_s2p_test_script}"
"#!/bin/sh
set -e
TOOLDIR='${CMAKE_BINARY_DIR}/bin'
WORK=\$(mktemp -d \"\${TMPDIR:-/tmp}/s2p-ctest.XXXXXX\")
trap 'rm -rf \"\$WORK\"' EXIT
cat > \"\$WORK/hello.cc\" <<'CC'
#include <cstdio>
int main() { std::printf(\"hello world\\n\"); return 0; }
CC
\"\$TOOLDIR/source-to-pzam\" \"\$WORK/hello.cc\" -o \"\$WORK/a.pzam\"
\"\$TOOLDIR/source-to-pzam\" \"\$WORK/hello.cc\" -o \"\$WORK/b.pzam\"
out=\$(\"\$TOOLDIR/pzam-run\" \"\$WORK/a.pzam\")
test \"\$out\" = 'hello world' || { echo \"unexpected output: \$out\"; exit 1; }
cmp \"\$WORK/a.pzam\" \"\$WORK/b.pzam\" || { echo \"determinism regression: two builds differ\"; exit 1; }
")
   file(CHMOD "${_s2p_test_script}" PERMISSIONS
      OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
   add_test(NAME source_to_pzam_hello_world COMMAND "${_s2p_test_script}")
   set_tests_properties(source_to_pzam_hello_world PROPERTIES
      TIMEOUT 600
      LABELS "source-to-pzam")
endif()

# If llvm-wasi produced clang.wasm / wasm-ld.wasm (standalone build outside
# the main tree), stage them into our bin/ so SOURCE_TO_PZAM_DEFAULT_TOOLDIR
# points at one dir containing every tool. Checked at configure time — pass
# -DLLVM_WASI_INSTALL=<path> to override the default search location.
set(LLVM_WASI_INSTALL "${CMAKE_SOURCE_DIR}/build/llvm-wasi/install" CACHE PATH
   "Install prefix of the standalone cmake/llvm-wasi build (clang.wasm, wasm-ld.wasm)")
foreach(_wasi_tool clang.wasm wasm-ld.wasm)
   set(_src "${LLVM_WASI_INSTALL}/bin/${_wasi_tool}")
   if(EXISTS "${_src}")
      add_custom_command(TARGET source-to-pzam POST_BUILD
         COMMAND ${CMAKE_COMMAND} -E copy_if_different
                 "${_src}" "${SOURCE_TO_PZAM_DEFAULT_TOOLDIR}/${_wasi_tool}"
         COMMENT "Staging ${_wasi_tool} into source-to-pzam tooldir"
         VERBATIM)
   endif()
endforeach()
