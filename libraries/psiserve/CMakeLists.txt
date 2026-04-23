# ── psiserve: WASM TCP application server library ──────────────────────────────

add_library(psiserve
   src/fiber_lock_policy_check.cpp
   src/host_api.cpp
   src/log.cpp
   src/module_store.cpp
   src/runtime.cpp
)

target_include_directories(psiserve PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(psiserve PUBLIC
   pcrypt
   pfs
   psi-api
   psiber
   psitri
   psizam
   psizam-exec
   ucc
   quill::quill
   Threads::Threads
)

# Boost for Boost.Context (fiber switching) and Boost.Thread (portable stack sizes)
find_package(Boost REQUIRED COMPONENTS context thread)
target_link_libraries(psiserve PUBLIC Boost::boost Boost::context Boost::thread)

# OpenSSL for TLS
find_package(OpenSSL REQUIRED)
target_link_libraries(psiserve PUBLIC OpenSSL::SSL OpenSSL::Crypto)

# Link LLVM backend when enabled
if(TARGET psizam-llvm)
   target_link_libraries(psiserve PUBLIC psizam-llvm)
endif()

# ── Tests ───────────────────────────────────────────────────────────────────────
if(PSIZAM_ENABLE_TESTS)
   add_executable(db_host_test tests/db_host_test.cpp)
   target_link_libraries(db_host_test PRIVATE psiserve psi-api)
   target_include_directories(db_host_test PRIVATE
      ${CMAKE_SOURCE_DIR}/libraries/psizam/external/Catch2/single_include)
   add_test(NAME db_host_test COMMAND db_host_test)

   add_executable(module_store_test tests/module_store_test.cpp)
   target_link_libraries(module_store_test PRIVATE psiserve)
   target_include_directories(module_store_test PRIVATE
      ${CMAKE_SOURCE_DIR}/libraries/psizam/external/Catch2/single_include)
   add_test(NAME module_store_test COMMAND module_store_test)

   add_executable(runtime_host_test tests/runtime_host_test.cpp)
   target_link_libraries(runtime_host_test PRIVATE psiserve psi-api)
   target_include_directories(runtime_host_test PRIVATE
      ${CMAKE_SOURCE_DIR}/libraries/psizam/external/Catch2/single_include)
   add_test(NAME runtime_host_test COMMAND runtime_host_test)

   add_executable(instance_context_test tests/instance_context_test.cpp)
   target_link_libraries(instance_context_test PRIVATE psiserve psi-api)
   target_include_directories(instance_context_test PRIVATE
      ${CMAKE_SOURCE_DIR}/libraries/psizam/external/Catch2/single_include)
   add_test(NAME instance_context_test COMMAND instance_context_test)

   add_executable(fd_table_test tests/fd_table_test.cpp)
   target_link_libraries(fd_table_test PRIVATE psiserve)
   target_include_directories(fd_table_test PRIVATE
      ${CMAKE_SOURCE_DIR}/libraries/psizam/external/Catch2/single_include)
   add_test(NAME fd_table_test COMMAND fd_table_test)

   add_executable(host_api_log_test tests/host_api_log_test.cpp)
   target_link_libraries(host_api_log_test PRIVATE psiserve)
   target_include_directories(host_api_log_test PRIVATE
      ${CMAKE_SOURCE_DIR}/libraries/psizam/external/Catch2/single_include)
   add_test(NAME host_api_log_test COMMAND host_api_log_test)

   # ── E2E WASM guest test ──────────────────────────────────────────────────
   psi_add_wasm_module(db_guest
      SOURCES tests/db_guest.cpp
      OUTPUT  db_guest.wasm
      DEPS    psio psizam psi-api)

   if(TARGET db_guest)
      get_target_property(_db_guest_wasm db_guest PSI_WASM_OUTPUT)
      set(_db_guest_hpp  ${CMAKE_CURRENT_BINARY_DIR}/db_guest_wasm.hpp)

      add_custom_command(
         OUTPUT  ${_db_guest_hpp}
         COMMAND ${CMAKE_COMMAND}
                 -DINPUT=${_db_guest_wasm}
                 -DOUTPUT=${_db_guest_hpp}
                 -DNAME=db_guest_wasm_bytes
                 -P ${CMAKE_SOURCE_DIR}/libraries/psizam/examples/hello_world/bin2hpp.cmake
         DEPENDS ${_db_guest_wasm} db_guest
         COMMENT "Embedding db_guest.wasm → db_guest_wasm.hpp"
         VERBATIM)

      add_executable(db_e2e_test
         tests/db_e2e_test.cpp
         ${_db_guest_hpp})
      target_include_directories(db_e2e_test PRIVATE
         ${CMAKE_CURRENT_BINARY_DIR}
         ${CMAKE_SOURCE_DIR}/libraries/psizam/external/Catch2/single_include)
      target_link_libraries(db_e2e_test PRIVATE psiserve psi-api)
      add_test(NAME db_e2e_test COMMAND db_e2e_test)
   endif()
endif()
