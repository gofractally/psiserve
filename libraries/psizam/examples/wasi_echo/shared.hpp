#pragma once

#include <stdint.h>
#include <psio1/guest_attrs.hpp>
#include <psio1/structural.hpp>

PSIO1_PACKAGE(wasi_echo, "0.1.0");
#undef  PSIO1_CURRENT_PACKAGE_
#define PSIO1_CURRENT_PACKAGE_ PSIO1_PACKAGE_TYPE_(wasi_echo)

struct wasi_echo_service
{
   static void run(uint32_t port);
};

PSIO1_INTERFACE(wasi_echo_service,
               types(),
               funcs(func(run, port)))
