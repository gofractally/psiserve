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
// PSIO_PACKAGE or an explicit namespace field) will close that gap;
// until then the test uses structural checks rather than byte-equal
// diffs.

#include <psio/reflect.hpp>
#include <psio/structural.hpp>

#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace wasi::cli::environment_ns
{
   // Definitions exist so the address-of in PSIO_INTERFACE resolves.
   // Real host bindings for these live in psiserve; these bodies are
   // never called at runtime because psiserve's Linker wires the
   // imports to host_function closures before instantiation.
   inline std::vector<std::tuple<std::string, std::string>> get_environment()
   {
      return {};
   }

   inline std::vector<std::string> get_arguments() { return {}; }

   inline std::optional<std::string> initial_cwd() { return std::nullopt; }
}  // namespace wasi::cli::environment_ns

PSIO_PACKAGE(wasi_cli, "0.2.3");

PSIO_INTERFACE(environment,
               types(),
               funcs(wasi::cli::environment_ns::get_environment,
                     wasi::cli::environment_ns::get_arguments,
                     wasi::cli::environment_ns::initial_cwd))
