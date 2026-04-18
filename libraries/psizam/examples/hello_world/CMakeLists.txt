# hello_world — end-to-end run of a guest WASM built from C++ with
# host imports wired by walking PSIO_HOST_MODULE's reflected interface
# list. The guest (guest.cpp) and host (host.cpp) share interface
# declarations via shared.hpp; each side owns its own impl — shared.hpp
# itself is declaration-only.
#
# The guest build uses wasi-sdk (not Homebrew LLVM + wasi-libc alone).
# wasi-sdk ships a wasm-native libc++, which is the realistic dependency
# story for WIT canonical ABI — std::string_view, std::span, and
# wit::string all require libc++ headers that work on wasm32.
#
# The wasi-sdk probe + include resolution is handled by
# psi_add_wasm_module() (see cmake/PsiWasmModule.cmake); we just
# name the CMake targets whose transitive INTERFACE_INCLUDE_DIRECTORIES
# the guest needs — psio pulls in Boost::headers PUBLIC, so Boost.PP
# and Boost.Core demangle are reachable for PSIO_REFLECT.

psi_add_wasm_module(hello_world_guest
   SOURCES guest.cpp
   OUTPUT  guest.wasm
   DEPS    psio psizam wasi-cpp)

if(NOT TARGET hello_world_guest)
   # helper skipped (no wasi-sdk) — nothing else to build
   return()
endif()

set(_wasm ${CMAKE_CURRENT_BINARY_DIR}/guest.wasm)
set(_hpp  ${CMAKE_CURRENT_BINARY_DIR}/guest_wasm.hpp)

add_custom_command(
   OUTPUT  ${_hpp}
   COMMAND ${CMAKE_COMMAND}
           -DINPUT=${_wasm}
           -DOUTPUT=${_hpp}
           -DNAME=guest_wasm_bytes
           -P ${CMAKE_CURRENT_SOURCE_DIR}/bin2hpp.cmake
   DEPENDS ${_wasm} hello_world_guest ${CMAKE_CURRENT_SOURCE_DIR}/bin2hpp.cmake
   COMMENT "Embedding guest.wasm → guest_wasm.hpp"
   VERBATIM)

add_executable(psizam_hello_world host.cpp ${_hpp})
target_include_directories(psizam_hello_world PRIVATE
   ${CMAKE_CURRENT_SOURCE_DIR}
   ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(psizam_hello_world PRIVATE psizam wasi-cpp)
