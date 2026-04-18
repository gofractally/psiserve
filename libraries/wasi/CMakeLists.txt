# WASI — vendored WIT + per-language bindings.
#
# The top-level directory owns only the language-neutral `wit/`. Each
# language subtree is self-contained and built by its own ecosystem's
# tooling (CMake for C++; Cargo for Rust, etc. — not PSIO's problem).
add_subdirectory(cpp)
