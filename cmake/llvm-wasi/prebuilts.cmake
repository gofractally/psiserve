# cmake/llvm-wasi/prebuilts.cmake
#
# Pinned manifest for the WASI-compiled toolchain artifacts.
#
# These are the bytes that psiserve's content-addressed build model
# depends on: clang.wasm, wasm-ld.wasm, and the wasi-sysroot bundle.
# They are produced by:
#
#    BUILD_CLANG=1 scripts/build-llvm-wasi.sh
#
# and, once a coordinator tags `llvm-wasi-v<ver>`, published to a
# GitHub Release by the `toolchain-release.yml` workflow.
#
# Every consumer that includes this file gets:
#   * PSI_LLVM_VERSION             — the LLVM version these artifacts were cut from
#   * PSI_TOOLCHAIN_URL_BASE       — the base URL to fetch the artifacts from
#   * PSI_TOOLCHAIN_CLANG_HASH     — SHA256 of clang.wasm
#   * PSI_TOOLCHAIN_WASM_LD_HASH   — SHA256 of wasm-ld.wasm
#   * PSI_TOOLCHAIN_SYSROOT_HASH   — SHA256 of wasi-sysroot.tar.zst
#
# Procedure when bumping LLVM:
#   1. Edit PSI_LLVM_VERSION below.
#   2. Push a branch, open a PR, let CI run `toolchain-release.yml`
#      manually via workflow_dispatch (it will produce artifacts and
#      upload them to a draft release named llvm-wasi-v<ver>).
#   3. Update the three hashes with the sha256sum values the workflow
#      prints at the end. Commit and re-push.
#   4. The release is promoted from draft → published by a tag push:
#      `git tag llvm-wasi-v<ver> && git push --tags`.

set(PSI_LLVM_VERSION "22.1.2"
    CACHE STRING "LLVM version the toolchain artifacts are built against")

# Override with `-DPSI_TOOLCHAIN_URL_BASE=https://my-mirror/...` if
# you host the prebuilts somewhere other than the default release page.
set(PSI_TOOLCHAIN_URL_BASE
    "https://github.com/gofractally/psiserve/releases/download/llvm-wasi-v${PSI_LLVM_VERSION}"
    CACHE STRING "Base URL for the prebuilt toolchain release")

# NOTE: these are placeholder zeros. They are written into the file by
# `toolchain-release.yml` after a successful cross-build. Until the first
# release is published for a given LLVM version, a clean clone MUST
# configure with `-DPSI_BUILD_TOOLCHAIN=ON` to build from source.
set(PSI_TOOLCHAIN_CLANG_HASH
    "0000000000000000000000000000000000000000000000000000000000000000"
    CACHE STRING "SHA256 of clang.wasm")
set(PSI_TOOLCHAIN_WASM_LD_HASH
    "0000000000000000000000000000000000000000000000000000000000000000"
    CACHE STRING "SHA256 of wasm-ld.wasm")
set(PSI_TOOLCHAIN_SYSROOT_HASH
    "0000000000000000000000000000000000000000000000000000000000000000"
    CACHE STRING "SHA256 of wasi-sysroot.tar.zst")
