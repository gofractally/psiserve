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
