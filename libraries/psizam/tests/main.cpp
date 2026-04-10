/**
 *  @file
 *  @copyright defined in eos/LICENSE.txt
 */
#include <psizam/allocator.hpp>

#include "utils.hpp"

#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

psizam::wasm_allocator wa;

template void psizam::execution_context::execute(psizam::interpret_visitor<psizam::execution_context>& visitor);

template class psizam::backend<psizam::standalone_function_t, psizam::interpreter>;
#ifdef __x86_64__
template class psizam::backend<psizam::standalone_function_t, psizam::jit>;
template class psizam::backend<psizam::standalone_function_t, psizam::jit2>;
#elif defined(__aarch64__)
template class psizam::backend<psizam::standalone_function_t, psizam::jit>;
template class psizam::backend<psizam::standalone_function_t, psizam::jit2>;
#endif
