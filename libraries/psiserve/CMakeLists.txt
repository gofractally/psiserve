# ── psiserve: WASM TCP application server library ──────────────────────────────

add_library(psiserve
   src/host_api.cpp
   src/io_engine_kqueue.cpp
   src/log.cpp
   src/scheduler.cpp
   src/runtime.cpp
)

target_include_directories(psiserve PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(psiserve PUBLIC
   psizam
   psizam-runtime
   ucc
   quill::quill
   Threads::Threads
)

# Boost for program_options and Boost.Context (fiber switching)
find_package(Boost REQUIRED COMPONENTS context)
target_link_libraries(psiserve PUBLIC Boost::boost Boost::context)

# OpenSSL for TLS
find_package(OpenSSL REQUIRED)
target_link_libraries(psiserve PUBLIC OpenSSL::SSL OpenSSL::Crypto)

# Link LLVM backend when enabled
if(TARGET psizam-llvm)
   target_link_libraries(psiserve PUBLIC psizam-llvm)
endif()
