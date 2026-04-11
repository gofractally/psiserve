# ── psiserve: WASM TCP application server library ──────────────────────────────

add_library(psiserve
   src/io_engine_kqueue.cpp
   src/scheduler.cpp
   src/runtime.cpp
)

target_include_directories(psiserve PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(psiserve PUBLIC
   psizam
   Threads::Threads
)

# Boost for program_options (used by CLI, but headers used by lib for utility)
find_package(Boost REQUIRED)
target_link_libraries(psiserve PUBLIC Boost::boost)
