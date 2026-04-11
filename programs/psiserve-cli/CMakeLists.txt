# ── psiserve-cli: standalone server binary ─────────────────────────────────────

find_package(Boost REQUIRED COMPONENTS program_options)

add_executable(psiserve-cli
   main.cpp
)

target_link_libraries(psiserve-cli PRIVATE
   psiserve
   Boost::program_options
)

set_target_properties(psiserve-cli PROPERTIES
   OUTPUT_NAME psiserve
)
