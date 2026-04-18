# bin2hpp.cmake — read a binary file, emit a C++ header with its bytes
# as a constexpr std::array. Invoked via cmake -P from a build rule.
#
# Inputs: -DINPUT=<bin> -DOUTPUT=<hpp> -DNAME=<identifier>

file(READ "${INPUT}" _hex HEX)
string(LENGTH "${_hex}" _nibbles)
math(EXPR _count "${_nibbles} / 2")
string(REGEX REPLACE "(..)" "0x\\1, " _bytes "${_hex}")

file(WRITE "${OUTPUT}"
"// Auto-generated from ${INPUT}. Do not edit.
#pragma once

#include <array>
#include <cstdint>

inline constexpr std::array<std::uint8_t, ${_count}> ${NAME} = {
    ${_bytes}
};
")
