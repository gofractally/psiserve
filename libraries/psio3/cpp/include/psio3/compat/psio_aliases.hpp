#pragma once
//
// psio3/compat/psio_aliases.hpp — migration-aid header that re-exports
// psio3 symbols under the historical `psio::` namespace. Intended for
// consumers (psizam, pfs, application code) that currently call
// `psio::encode(…)` / `psio::frac::…` / `psio::reflect<T>` and want to
// validate their code against psio3 before the swap lands in Phase 18.
//
// **Status:** transitional. This header is NOT part of the final v2
// API — after Phase 18 renames `libraries/psio3/` → `libraries/psio/`
// and rewrites `psio3::` → `psio::` across the tree, this header
// disappears along with the `psio3::` namespace itself.
//
// **Scope of aliases:** just enough that a consumer's existing code
// compiles. Format tags, the top-level CPO objects, reflect<T>, schema
// helpers, and the error model are all pulled in. Anything not
// listed here still needs an explicit `::psio3::` qualifier in the
// consumer's code — a deliberate rough edge so the consumer knows
// the migration surface isn't complete.
//
// **Enabling:** this header is NOT included by the master psio3.hpp.
// Consumers include it explicitly when they want the bridge:
//
//   #include <psio3/compat/psio_aliases.hpp>
//
// Once included, `psio::encode(psio::frac32{}, value, sink)` calls the
// psio3 encoder. Mixed usage (some `psio::`, some `psio3::`) also
// works — both refer to the same underlying entities.

#include <psio3/psio3.hpp>

// Top-level namespace alias — makes `psio::` resolve to `psio3::` for
// all uses. Nested symbols, templates, traits, concepts, and macros
// all flow through automatically.
namespace psio = ::psio3;

// Macros: C++ namespaces don't catch macros, so re-export the common
// ones under the old spelling so consumers' existing macro callsites
// continue to work. If the old macro name already exists (e.g. from a
// transitional build where both psio v1 and psio3 are linked), the
// guard skips the redefinition.

#ifndef PSIO_REFLECT
   #define PSIO_REFLECT(...)     PSIO3_REFLECT(__VA_ARGS__)
#endif

#ifndef PSIO_ATTRS
   // Symbolic placeholder; PSIO3_ATTRS is still deferred (see
   // annotate.hpp). Consumers using PSIO_ATTRS get a compile error
   // referring to the plan until the macro lands.
   #define PSIO_ATTRS(...)                                            \
      static_assert(false,                                            \
                    "PSIO_ATTRS → PSIO3_ATTRS not yet implemented — " \
                    "see .issues/psio-v2-design.md § 5.3.5")
#endif

