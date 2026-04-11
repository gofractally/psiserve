# psio: fracpack serialization library (header-only subset from psibase)
add_library(psio INTERFACE)
target_include_directories(psio INTERFACE include)

# Boost headers (Preprocessor + core::demangle)
find_package(Boost REQUIRED)
target_link_libraries(psio INTERFACE Boost::headers)
