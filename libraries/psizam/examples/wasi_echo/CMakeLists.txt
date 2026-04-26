# wasi_echo — WASI TCP echo service: guest WASM + host runtime driver.
#
# The guest uses WASI 0.2.3 sockets/IO imports through canonical ABI.
# The host wires WasiIoHost + WasiSocketsHost as providers and runs
# the guest inside a psiber scheduler fiber.

psi_add_wasm_module(wasi_echo_guest
   SOURCES guest.cpp
   OUTPUT  guest.wasm
   DEPS    psio1 psizam wasi-cpp)

if(NOT TARGET wasi_echo_guest)
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
           -P ${CMAKE_CURRENT_SOURCE_DIR}/../hello_world/bin2hpp.cmake
   DEPENDS ${_wasm} wasi_echo_guest
           ${CMAKE_CURRENT_SOURCE_DIR}/../hello_world/bin2hpp.cmake
   COMMENT "Embedding guest.wasm -> guest_wasm.hpp"
   VERBATIM)

add_executable(wasi_echo_host host.cpp ${_hpp})
target_include_directories(wasi_echo_host PRIVATE
   ${CMAKE_CURRENT_SOURCE_DIR}
   ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(wasi_echo_host PRIVATE psizam psizam-exec wasi-host)
