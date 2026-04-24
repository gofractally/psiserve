#pragma once
//
// psio3 — v2-architecture serialization library.
//
// This is the master include. Pulls in every public header that psio3
// provides. Header contents grow as the 18-phase implementation plan
// proceeds; see .issues/psio-v2-design.md for the full architecture
// spec and /Users/dlarimer/.claude/plans/idempotent-beaming-milner.md
// for the phase-by-phase plan.
//
// During development psio3 lives alongside psio (v1). Once every phase's
// parity gate passes, psio3 replaces psio via a rename + namespace
// rewrite — at which point the `psio3::` identifier goes away and this
// file is replaced by psio/psio.hpp.
//
// Phase 0 scaffold: header is intentionally empty beyond this note.
// Phase 1 adds reflect.hpp. Phase 2 adds shapes.hpp, annotate.hpp,
// wrappers.hpp. Etc.

#include <psio3/annotate.hpp>
#include <psio3/bin.hpp>
#include <psio3/buffer.hpp>
#include <psio3/cpo.hpp>
#include <psio3/dynamic_json.hpp>
#include <psio3/dynamic_value.hpp>
#include <psio3/error.hpp>
#include <psio3/format_tag_base.hpp>
#include <psio3/frac.hpp>
#include <psio3/json.hpp>
#include <psio3/mutable_view.hpp>
#include <psio3/adapter.hpp>
#include <psio3/pssz.hpp>
#include <psio3/reflect.hpp>
#include <psio3/schema.hpp>
#include <psio3/shapes.hpp>
#include <psio3/ssz.hpp>
#include <psio3/storage.hpp>
#include <psio3/transcode.hpp>
#include <psio3/view.hpp>
#include <psio3/wrappers.hpp>

namespace psio3 {}
