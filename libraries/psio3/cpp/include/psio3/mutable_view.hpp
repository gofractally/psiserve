#pragma once
//
// psio3/mutable_view.hpp — arena-backed editable view.
//
// Design doc § 5.5 / § 9. v3.0 ships a minimal implementation with a
// growable buffer + edit log; the full in-place / overlay-with-replay
// representation choice is an implementation detail deferred to later
// phases.
//
// Access surface follows the same rule as `view`: field mutations
// dispatched through reflected accessors, storage ops as free
// functions. Full surface arrives alongside the first format that
// needs mutation (flatbuf in phase 11 or capnp in phase 12).

#include <psio3/buffer.hpp>
#include <psio3/storage.hpp>

namespace psio3 {

   // Placeholder declaration so later phases can reference the type
   // without redefining it. Concrete template landing in phase 11+.
   template <typename T, typename Fmt>
   class mutable_view;

}  // namespace psio3
