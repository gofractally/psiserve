# composition — two-module WASM composition demo.
# Provider module exports greeter interface; consumer module imports it
# and exports processor interface. Host wires them together.

psi_add_wasm_module(composition_provider_guest
   SOURCES provider_guest.cpp
   OUTPUT  provider.wasm
   DEPS    psio1 psizam wasi-cpp)

psi_add_wasm_module(composition_consumer_guest
   SOURCES consumer_guest.cpp
   OUTPUT  consumer.wasm
   DEPS    psio1 psizam wasi-cpp)

if(NOT TARGET composition_provider_guest OR NOT TARGET composition_consumer_guest)
   return()
endif()

set(_provider_wasm ${CMAKE_CURRENT_BINARY_DIR}/provider.wasm)
set(_provider_hpp  ${CMAKE_CURRENT_BINARY_DIR}/provider_wasm.hpp)
set(_consumer_wasm ${CMAKE_CURRENT_BINARY_DIR}/consumer.wasm)
set(_consumer_hpp  ${CMAKE_CURRENT_BINARY_DIR}/consumer_wasm.hpp)

# Use the same bin2hpp.cmake as hello_world
set(_bin2hpp ${CMAKE_CURRENT_SOURCE_DIR}/../hello_world/bin2hpp.cmake)

add_custom_command(
   OUTPUT  ${_provider_hpp}
   COMMAND ${CMAKE_COMMAND}
           -DINPUT=${_provider_wasm}
           -DOUTPUT=${_provider_hpp}
           -DNAME=provider_wasm_bytes
           -P ${_bin2hpp}
   DEPENDS ${_provider_wasm} composition_provider_guest ${_bin2hpp}
   COMMENT "Embedding provider.wasm -> provider_wasm.hpp"
   VERBATIM)

add_custom_command(
   OUTPUT  ${_consumer_hpp}
   COMMAND ${CMAKE_COMMAND}
           -DINPUT=${_consumer_wasm}
           -DOUTPUT=${_consumer_hpp}
           -DNAME=consumer_wasm_bytes
           -P ${_bin2hpp}
   DEPENDS ${_consumer_wasm} composition_consumer_guest ${_bin2hpp}
   COMMENT "Embedding consumer.wasm -> consumer_wasm.hpp"
   VERBATIM)

add_executable(psizam_composition host.cpp ${_provider_hpp} ${_consumer_hpp})
target_include_directories(psizam_composition PRIVATE
   ${CMAKE_CURRENT_SOURCE_DIR}
   ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(psizam_composition PRIVATE psizam wasi-cpp)
