# PsiWasmModule.cmake — helper for compiling a C++ guest module to a
# wasm32-wasip1 reactor with wasi-sdk.
#
# Consumers write:
#
#    psi_add_wasm_module(my_guest
#       SOURCES  guest.cpp
#       OUTPUT   guest.wasm
#       DEPS     psio wasi-cpp           # any CMake target; INTERFACE_INCLUDE_DIRECTORIES is walked transitively
#       FLAGS    -O2)                    # optional extra clang flags
#
# The helper probes for wasi-sdk (honoring `PSIZAM_WASI_SDK`), derives
# `-I` flags from the requested targets' include closure (so Boost
# headers etc. propagate automatically via psio's
# `target_link_libraries(... PUBLIC Boost::headers)`), and wires up an
# `add_custom_command` producing the .wasm. A phony target of the same
# name is registered so downstream CMake can `add_dependencies(...)`
# against it.
#
# The reactor defaults (`-mexec-model=reactor`, `-Wl,--allow-undefined`,
# `-Wl,--strip-all`, `-fno-exceptions`, `-fno-rtti`, `-std=c++20`) come
# from the canonical-ABI workflow: the guest is a library the host
# instantiates, host-provided imports resolve at instantiate time.

# Probe a set of common install prefixes for wasi-sdk, caching the
# first one that has both the compiler and a libc++ sysroot.
function(_psi_detect_wasi_sdk)
   if(DEFINED PSIZAM_WASI_SDK AND EXISTS ${PSIZAM_WASI_SDK}/bin/clang++)
      return()
   endif()
   set(_candidates
      /Users/dlarimer/wasi-sdk
      /opt/wasi-sdk
      /opt/homebrew/opt/wasi-sdk
      /usr/local/wasi-sdk)
   foreach(_cand ${_candidates})
      if(EXISTS ${_cand}/bin/clang++
         AND EXISTS ${_cand}/share/wasi-sysroot/include/wasm32-wasip1/c++/v1/string_view)
         set(PSIZAM_WASI_SDK ${_cand} CACHE PATH
            "Path to a wasi-sdk install (bin/clang++ + share/wasi-sysroot/ with libc++ headers)")
         return()
      endif()
   endforeach()
endfunction()

# Walk a list of targets and recursively collect their
# INTERFACE_INCLUDE_DIRECTORIES, following INTERFACE_LINK_LIBRARIES
# edges so includes propagate through `target_link_libraries(... PUBLIC ...)`
# chains (this is how psio's Boost::headers dep reaches guest builds).
# Imported interface libraries (Boost::headers) expose their own
# INTERFACE_INCLUDE_DIRECTORIES the same way.
function(_psi_collect_include_dirs out_var)
   set(_seen ${_PSI_INCLUDE_DIR_SEEN})
   set(_dirs "")
   foreach(_t ${ARGN})
      if(NOT TARGET ${_t})
         continue()
      endif()
      list(FIND _seen ${_t} _idx)
      if(NOT _idx EQUAL -1)
         continue()
      endif()
      list(APPEND _seen ${_t})

      get_target_property(_inc ${_t} INTERFACE_INCLUDE_DIRECTORIES)
      if(_inc)
         list(APPEND _dirs ${_inc})
      endif()

      get_target_property(_links ${_t} INTERFACE_LINK_LIBRARIES)
      if(_links)
         set(_PSI_INCLUDE_DIR_SEEN ${_seen})
         _psi_collect_include_dirs(_inner ${_links})
         list(APPEND _dirs ${_inner})
         set(_seen ${_PSI_INCLUDE_DIR_SEEN})
      endif()
   endforeach()
   list(REMOVE_DUPLICATES _dirs)
   set(${out_var} ${_dirs} PARENT_SCOPE)
   set(_PSI_INCLUDE_DIR_SEEN ${_seen} PARENT_SCOPE)
endfunction()

function(psi_add_wasm_module name)
   cmake_parse_arguments(ARG
      ""                      # no flags
      "OUTPUT"                # one-value
      "SOURCES;DEPS;FLAGS"    # multi-value
      ${ARGN})

   if(NOT ARG_SOURCES)
      message(FATAL_ERROR "psi_add_wasm_module(${name}): SOURCES is required")
   endif()
   if(NOT ARG_OUTPUT)
      set(ARG_OUTPUT ${name}.wasm)
   endif()

   _psi_detect_wasi_sdk()
   if(NOT PSIZAM_WASI_SDK)
      message(STATUS
         "${name}: skipped — no wasi-sdk found. "
         "Install from https://github.com/WebAssembly/wasi-sdk/releases "
         "or set -DPSIZAM_WASI_SDK=/path/to/wasi-sdk.")
      return()
   endif()

   set(_cxx     ${PSIZAM_WASI_SDK}/bin/clang++)
   set(_sysroot ${PSIZAM_WASI_SDK}/share/wasi-sysroot)
   set(_out     ${CMAKE_CURRENT_BINARY_DIR}/${ARG_OUTPUT})

   # Absolute-path the sources so add_custom_command's DEPENDS resolves
   # correctly regardless of CMAKE_CURRENT_SOURCE_DIR at invocation.
   set(_srcs "")
   foreach(_s ${ARG_SOURCES})
      if(IS_ABSOLUTE ${_s})
         list(APPEND _srcs ${_s})
      else()
         list(APPEND _srcs ${CMAKE_CURRENT_SOURCE_DIR}/${_s})
      endif()
   endforeach()

   set(_PSI_INCLUDE_DIR_SEEN "")
   _psi_collect_include_dirs(_include_dirs ${ARG_DEPS})
   set(_include_flags "")
   foreach(_dir ${_include_dirs})
      list(APPEND _include_flags -I${_dir})
   endforeach()

   set(_reactor_flags
      --target=wasm32-wasip1
      --sysroot=${_sysroot}
      -mexec-model=reactor     # library-mode: host calls in, no main()
      -std=c++20
      -fno-exceptions
      -fno-rtti
      -O2
      -Wl,--allow-undefined    # host imports resolve at instantiate
      -Wl,--strip-all)

   add_custom_command(
      OUTPUT  ${_out}
      COMMAND ${_cxx}
              ${_reactor_flags}
              ${ARG_FLAGS}
              ${_include_flags}
              ${_srcs}
              -o ${_out}
      DEPENDS ${_srcs}
      COMMENT "Building wasm guest ${ARG_OUTPUT} (wasi-sdk)"
      VERBATIM)

   add_custom_target(${name} ALL DEPENDS ${_out})
   set_target_properties(${name} PROPERTIES PSI_WASM_OUTPUT ${_out})
endfunction()
