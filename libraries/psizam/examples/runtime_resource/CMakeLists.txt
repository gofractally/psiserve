# Build two WASM guests + one native host.

psi_add_wasm_module(runtime_resource_contract
   SOURCES contract_guest.cpp
   OUTPUT  contract.wasm
   DEPS    )

psi_add_wasm_module(runtime_resource_blockchain
   SOURCES blockchain_guest.cpp
   OUTPUT  blockchain.wasm
   DEPS    psio psizam wasi-cpp)

if(NOT TARGET runtime_resource_contract OR NOT TARGET runtime_resource_blockchain)
   return()
endif()

# Embed both WASMs into headers
set(_contract_wasm ${CMAKE_CURRENT_BINARY_DIR}/contract.wasm)
set(_contract_hpp  ${CMAKE_CURRENT_BINARY_DIR}/contract_wasm.hpp)

add_custom_command(
   OUTPUT  ${_contract_hpp}
   COMMAND ${CMAKE_COMMAND}
           -DINPUT=${_contract_wasm}
           -DOUTPUT=${_contract_hpp}
           -DNAME=contract_wasm_bytes
           -P ${CMAKE_CURRENT_SOURCE_DIR}/../hello_world/bin2hpp.cmake
   DEPENDS ${_contract_wasm} runtime_resource_contract
   COMMENT "Embedding contract.wasm → contract_wasm.hpp"
   VERBATIM)

set(_blockchain_wasm ${CMAKE_CURRENT_BINARY_DIR}/blockchain.wasm)
set(_blockchain_hpp  ${CMAKE_CURRENT_BINARY_DIR}/blockchain_wasm.hpp)

add_custom_command(
   OUTPUT  ${_blockchain_hpp}
   COMMAND ${CMAKE_COMMAND}
           -DINPUT=${_blockchain_wasm}
           -DOUTPUT=${_blockchain_hpp}
           -DNAME=blockchain_wasm_bytes
           -P ${CMAKE_CURRENT_SOURCE_DIR}/../hello_world/bin2hpp.cmake
   DEPENDS ${_blockchain_wasm} runtime_resource_blockchain
   COMMENT "Embedding blockchain.wasm → blockchain_wasm.hpp"
   VERBATIM)

add_executable(psizam_runtime_resource
   host.cpp
   ${_contract_hpp}
   ${_blockchain_hpp}
   ${CMAKE_SOURCE_DIR}/libraries/psizam/src/runtime.cpp)
target_include_directories(psizam_runtime_resource PRIVATE
   ${CMAKE_CURRENT_SOURCE_DIR}
   ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(psizam_runtime_resource PRIVATE psizam wasi-cpp)
