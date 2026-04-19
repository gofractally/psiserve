# Standalone sub-project that builds wasi_llvm_stubs.c into a static archive
# under the WASI toolchain. Invoked via ExternalProject_Add from
# cmake/llvm-wasi/CMakeLists.txt. Installs libwasi_llvm_stubs.a into
# ${CMAKE_INSTALL_PREFIX}/lib/ so clang-wasi can link against -lwasi_llvm_stubs.

cmake_minimum_required(VERSION 3.20)
project(wasi-llvm-stubs C)

add_library(wasi_llvm_stubs STATIC ../wasi_llvm_stubs.c)

install(TARGETS wasi_llvm_stubs ARCHIVE DESTINATION lib)
