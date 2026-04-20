#pragma once

#include <stdint.h>
#include <psio/guest_attrs.hpp>
#include <psio/structural.hpp>

PSIO_PACKAGE(wasi_echo, "0.1.0");
#undef  PSIO_CURRENT_PACKAGE_
#define PSIO_CURRENT_PACKAGE_ PSIO_PACKAGE_TYPE_(wasi_echo)

struct wasi_echo_service
{
   static void run(uint32_t port);
};

PSIO_INTERFACE(wasi_echo_service,
               types(),
               funcs(func(run, port)))
