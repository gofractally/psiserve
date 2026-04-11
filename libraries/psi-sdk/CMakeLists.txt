# psi-sdk: WASM static library for psi platform services
#
# Cross-compiles SHA-1, Base64, WebSocket, and host declarations
# to a wasm32 static library (libpsi.a) that services link against.
#
# Uses Homebrew LLVM's clang (must support --target=wasm32).

find_program(WASM_CC
   NAMES clang
   PATHS "${LLVM_PREFIX}/bin" "$ENV{LLVM_PREFIX}/bin"
   NO_DEFAULT_PATH
)
if(NOT WASM_CC)
   # Fallback: try brew --prefix llvm
   execute_process(
      COMMAND brew --prefix llvm
      OUTPUT_VARIABLE _llvm_prefix
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
   )
   find_program(WASM_CC NAMES clang PATHS "${_llvm_prefix}/bin" NO_DEFAULT_PATH)
endif()

find_program(WASM_AR
   NAMES llvm-ar
   PATHS "${LLVM_PREFIX}/bin" "$ENV{LLVM_PREFIX}/bin"
   NO_DEFAULT_PATH
)
if(NOT WASM_AR)
   execute_process(
      COMMAND brew --prefix llvm
      OUTPUT_VARIABLE _llvm_prefix
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
   )
   find_program(WASM_AR NAMES llvm-ar PATHS "${_llvm_prefix}/bin" NO_DEFAULT_PATH)
endif()

if(NOT WASM_CC OR NOT WASM_AR)
   message(WARNING "psi-sdk: Cannot find wasm32 clang/llvm-ar — skipping SDK build")
   return()
endif()

message(STATUS "psi-sdk: WASM_CC=${WASM_CC}")
message(STATUS "psi-sdk: WASM_AR=${WASM_AR}")

set(PSI_SDK_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(PSI_SDK_INCLUDE ${PSI_SDK_DIR}/include)
set(PSI_SDK_SRC ${PSI_SDK_DIR}/src)
set(PSI_SDK_LIB ${CMAKE_CURRENT_BINARY_DIR})

set(WASM_CFLAGS
   --target=wasm32
   -nostdlib
   -fno-builtin
   -O2
   -I${PSI_SDK_INCLUDE}
   -I${PSI_SDK_SRC}
   -Wno-incompatible-library-redeclaration
)

set(PSI_SDK_SOURCES
   sha1_vendor
   base64_vendor
   ws_parser_vendor
   websocket
)

set(PSI_SDK_OBJECTS "")

foreach(src ${PSI_SDK_SOURCES})
   set(obj ${PSI_SDK_LIB}/${src}.o)
   add_custom_command(
      OUTPUT ${obj}
      COMMAND ${WASM_CC} ${WASM_CFLAGS}
         -c ${PSI_SDK_SRC}/${src}.c
         -o ${obj}
      DEPENDS ${PSI_SDK_SRC}/${src}.c
      COMMENT "Compiling ${src}.c -> wasm32"
   )
   list(APPEND PSI_SDK_OBJECTS ${obj})
endforeach()

add_custom_command(
   OUTPUT ${PSI_SDK_LIB}/libpsi.a
   COMMAND ${WASM_AR} rcs ${PSI_SDK_LIB}/libpsi.a ${PSI_SDK_OBJECTS}
   DEPENDS ${PSI_SDK_OBJECTS}
   COMMENT "Archiving libpsi.a (wasm32)"
)

add_custom_target(psi-sdk ALL DEPENDS ${PSI_SDK_LIB}/libpsi.a)
