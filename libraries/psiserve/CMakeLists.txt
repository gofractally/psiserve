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
   pfs
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
endif()
