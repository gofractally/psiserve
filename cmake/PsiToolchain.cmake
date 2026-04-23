# cmake/PsiToolchain.cmake
#
# Resolves the three WASI-compiled toolchain artifacts
# (clang.wasm, wasm-ld.wasm, wasi-sysroot bundle) exactly once per
# configure. After `psi_ensure_toolchain()` returns, the following
# cache variables point at real files on disk:
#
#    PSI_TOOLCHAIN_DIR          — directory containing the three
#    PSI_TOOLCHAIN_CLANG        — .../clang.wasm
#    PSI_TOOLCHAIN_WASM_LD      — .../wasm-ld.wasm
#    PSI_TOOLCHAIN_SYSROOT      — .../wasi-sysroot        (directory)
#
# Resolution order (highest priority first):
#
#    1. `-DPSI_TOOLCHAIN_DIR=/path`            — use existing prebuilts.
#                                                  Useful for air-gapped
#                                                  / corp mirrors.
#    2. `-DPSI_BUILD_TOOLCHAIN=ON`              — build from source via
#                                                  `cmake/llvm-wasi/`.
#                                                  ~1-4 h; rare but
#                                                  always available.
#    3. Default: FetchContent from the URL in  — hash-verified download
#       `cmake/llvm-wasi/prebuilts.cmake`.       from the pinned release.
#
# The function is a no-op if already resolved.

if(DEFINED PSI_TOOLCHAIN_RESOLVED)
   return()
endif()

include(FetchContent)
include(${CMAKE_CURRENT_LIST_DIR}/llvm-wasi/prebuilts.cmake)

function(psi_ensure_toolchain)
   # 1. Caller-supplied directory.
   if(PSI_TOOLCHAIN_DIR AND IS_DIRECTORY ${PSI_TOOLCHAIN_DIR})
      set(_clang   ${PSI_TOOLCHAIN_DIR}/clang.wasm)
      set(_wasm_ld ${PSI_TOOLCHAIN_DIR}/wasm-ld.wasm)
      set(_sysroot ${PSI_TOOLCHAIN_DIR}/wasi-sysroot)
      if(EXISTS ${_clang} AND EXISTS ${_wasm_ld} AND IS_DIRECTORY ${_sysroot})
         message(STATUS "psi-toolchain: using local directory ${PSI_TOOLCHAIN_DIR}")
         _psi_publish_toolchain(${PSI_TOOLCHAIN_DIR} ${_clang} ${_wasm_ld} ${_sysroot})
         return()
      endif()
      message(FATAL_ERROR
         "PSI_TOOLCHAIN_DIR=${PSI_TOOLCHAIN_DIR} is set but does not contain "
         "the expected clang.wasm / wasm-ld.wasm / wasi-sysroot/ entries")
   endif()

   # 2. Opt-in source build.
   if(PSI_BUILD_TOOLCHAIN)
      message(STATUS "psi-toolchain: building from source (PSI_BUILD_TOOLCHAIN=ON)")
      _psi_build_from_source()
      return()
   endif()

   # 3. Default: FetchContent from the pinned release.
   _psi_fetch_prebuilts()
endfunction()

# ─── Internals ────────────────────────────────────────────────────────────────

function(_psi_publish_toolchain dir clang wasm_ld sysroot)
   set(PSI_TOOLCHAIN_DIR     ${dir}     CACHE PATH "WASI toolchain directory" FORCE)
   set(PSI_TOOLCHAIN_CLANG   ${clang}   CACHE FILEPATH "clang.wasm" FORCE)
   set(PSI_TOOLCHAIN_WASM_LD ${wasm_ld} CACHE FILEPATH "wasm-ld.wasm" FORCE)
   set(PSI_TOOLCHAIN_SYSROOT ${sysroot} CACHE PATH "wasi-sysroot" FORCE)
   set(PSI_TOOLCHAIN_RESOLVED TRUE CACHE INTERNAL "")
endfunction()

function(_psi_fetch_prebuilts)
   # Sanity check: a fresh-clone state ships with all-zero placeholder
   # hashes. Fail loudly in that case so no one accidentally ships with
   # an unverified toolchain.
   if(PSI_TOOLCHAIN_CLANG_HASH STREQUAL
      "0000000000000000000000000000000000000000000000000000000000000000")
      message(FATAL_ERROR
         "psi-toolchain: prebuilt hashes in cmake/llvm-wasi/prebuilts.cmake "
         "are placeholders. Either:\n"
         "   configure with -DPSI_BUILD_TOOLCHAIN=ON to build from source, or\n"
         "   configure with -DPSI_TOOLCHAIN_DIR=/path to use local prebuilts, or\n"
         "   wait for the first llvm-wasi-v${PSI_LLVM_VERSION} release to be "
         "published and for the hashes to be committed.")
   endif()

   message(STATUS
      "psi-toolchain: fetching prebuilts for LLVM ${PSI_LLVM_VERSION}")
   message(STATUS "  source: ${PSI_TOOLCHAIN_URL_BASE}")

   FetchContent_Declare(psi_toolchain_clang
      URL      ${PSI_TOOLCHAIN_URL_BASE}/clang.wasm
      URL_HASH SHA256=${PSI_TOOLCHAIN_CLANG_HASH}
      DOWNLOAD_NO_EXTRACT TRUE)
   FetchContent_Declare(psi_toolchain_wasm_ld
      URL      ${PSI_TOOLCHAIN_URL_BASE}/wasm-ld.wasm
      URL_HASH SHA256=${PSI_TOOLCHAIN_WASM_LD_HASH}
      DOWNLOAD_NO_EXTRACT TRUE)
   FetchContent_Declare(psi_toolchain_sysroot
      URL      ${PSI_TOOLCHAIN_URL_BASE}/wasi-sysroot.tar.zst
      URL_HASH SHA256=${PSI_TOOLCHAIN_SYSROOT_HASH})

   FetchContent_MakeAvailable(
      psi_toolchain_clang
      psi_toolchain_wasm_ld
      psi_toolchain_sysroot)

   # FetchContent's "no-extract" places the raw file at <src>/<basename>.
   set(_dir     ${CMAKE_BINARY_DIR}/psi-toolchain)
   file(MAKE_DIRECTORY ${_dir})
   file(CREATE_LINK ${psi_toolchain_clang_SOURCE_DIR}/clang.wasm
                     ${_dir}/clang.wasm SYMBOLIC)
   file(CREATE_LINK ${psi_toolchain_wasm_ld_SOURCE_DIR}/wasm-ld.wasm
                     ${_dir}/wasm-ld.wasm SYMBOLIC)
   # sysroot archive is already extracted by FetchContent.
   file(CREATE_LINK ${psi_toolchain_sysroot_SOURCE_DIR}
                     ${_dir}/wasi-sysroot SYMBOLIC)

   _psi_publish_toolchain(
      ${_dir}
      ${_dir}/clang.wasm
      ${_dir}/wasm-ld.wasm
      ${_dir}/wasi-sysroot)
endfunction()

function(_psi_build_from_source)
   # Shells out to the existing standalone cmake sub-project. It already
   # handles BUILD_CLANG, mimalloc, host-tblgen, etc. The wrapper script
   # is the source-of-truth invocation.
   set(_build_dir ${CMAKE_BINARY_DIR}/llvm-wasi)
   set(_install   ${_build_dir}/install)

   if(NOT EXISTS ${_install}/bin/clang.wasm)
      message(STATUS "psi-toolchain: invoking scripts/build-llvm-wasi.sh")
      message(STATUS "  target: ${_build_dir} (build takes 1-4 h first time)")
      execute_process(
         COMMAND env BUILD_CLANG=1 ${CMAKE_SOURCE_DIR}/scripts/build-llvm-wasi.sh ${_build_dir}
         RESULT_VARIABLE _rc)
      if(NOT _rc EQUAL 0)
         message(FATAL_ERROR "psi-toolchain: source build failed (rc=${_rc})")
      endif()
   endif()

   _psi_publish_toolchain(
      ${_install}
      ${_install}/bin/clang.wasm
      ${_install}/bin/wasm-ld.wasm
      ${_install}/share/wasi-sysroot)
endfunction()
