#pragma once

// wasi:cli@0.2.3 — environment variables, arguments, and initial cwd.
//
// Canonical WIT: libraries/wasi/wit/0.2.3/cli/environment.wit. These
// C++ declarations reflect back to that WIT through PSIO; the
// tests/round_trip.cpp golden-diff suite is what enforces agreement.
//
// The `wasi_cli` package identifier renders as `package wasi_cli@0.2.3;`
// in emit_wit output; canonical WIT requires `wasi:cli`. PSIO
// packages are currently keyed by a C++ identifier, so the colon
// separator isn't expressible yet. A follow-up in PSIO (string-form
// PSIO_PACKAGE or an explicit namespace field) will close that gap.
//
// Interface tag note: PSIO_INTERFACE takes the interface's own class
// as its tag, and the class name becomes the WIT interface name.
// Placing `struct environment { … }` at global scope is deliberate —
// any alternative (namespacing it, aliasing it) would either desync
// the WIT name or require a new macro variant. Consumers that want
// their own `environment` type should wrap the include in their own
// namespace or #undef/redefine.

#include <psio/reflect.hpp>
#include <psio/structural.hpp>

#include <optional>
#include <string>
#include <tuple>
#include <vector>

// Definitions exist so the address-of in PSIO_INTERFACE resolves.
// Real host bindings for these live in psiserve; these bodies are
// never called at runtime because psiserve's Linker wires the imports
// to host_function closures before instantiation.
struct environment
{
   static inline std::vector<std::tuple<std::string, std::string>> get_environment()
   {
      return {};
   }

   static inline std::vector<std::string> get_arguments() { return {}; }

   static inline std::optional<std::string> initial_cwd() { return std::nullopt; }
};

PSIO_PACKAGE(wasi_cli, "0.2.3");

PSIO_INTERFACE(environment,
               types(),
               funcs(func(get_environment),
                     func(get_arguments),
                     func(initial_cwd)))
