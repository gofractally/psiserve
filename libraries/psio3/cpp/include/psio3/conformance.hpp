#pragma once
//
// psio3/conformance.hpp — format-neutral test harness (§4.4).
//
// Writing one test case and sweeping it across every compatible format
// is a success criterion for v2 (§8 #4). This header ships three
// iteration macros:
//
//   PSIO3_FOR_EACH_SYMMETRIC_BINARY_FMT(X)
//       — ssz, pssz, frac32, bin, borsh, bincode, avro, key
//   PSIO3_FOR_EACH_TEXT_FMT(X)
//       — json
//   PSIO3_FOR_EACH_BINARY_FMT(X)
//       — every binary format above
//
// Each X is a macro of shape `X(Name, FmtValue)` — `Name` is a C++
// identifier suitable for concatenation; `FmtValue` is the format tag
// instance. Users expand the iteration with a local X macro that
// generates whatever they need (round-trip assertion, encode parity,
// size probe).
//
//   #include <psio3/conformance.hpp>
//
//   #define MY_ROUND_TRIP(FmtName, FmtValue)                              \
//      TEST_CASE("round-trip " #FmtName, "[conformance][" #FmtName "]")   \
//      {                                                                   \
//         Pt in{3, -7};                                                   \
//         auto bv = psio3::encode(FmtValue, in);                          \
//         auto out = psio3::decode<Pt>(FmtValue, std::span{bv});          \
//         REQUIRE(out.x == 3);                                            \
//         REQUIRE(out.y == -7);                                           \
//      }
//   PSIO3_FOR_EACH_SYMMETRIC_BINARY_FMT(MY_ROUND_TRIP)
//
// Adding a new format: add one entry to the list below. Every existing
// use site picks it up automatically.

#include <psio3/avro.hpp>
#include <psio3/bin.hpp>
#include <psio3/bincode.hpp>
#include <psio3/borsh.hpp>
#include <psio3/capnp.hpp>
#include <psio3/frac.hpp>
#include <psio3/json.hpp>
#include <psio3/key.hpp>
#include <psio3/pssz.hpp>
#include <psio3/ssz.hpp>
#include <psio3/wit.hpp>

// Two sweeps:
//
//   PSIO3_FOR_EACH_SYMMETRIC_BINARY_FMT — every binary format on its
//     supported shapes. Capnp joins here once its MVP lands.
//   PSIO3_FOR_EACH_RECORD_BINARY_FMT — subset that skips free-standing
//     std::variant encoding. Capnp encodes unions only as a *field* of
//     a reflected record (upstream capnp spec), so top-level variant
//     round-trips aren't applicable.
//
// Adding a new format: append to the broad list if it supports every
// shape, or only to the subset if a shape is deferred. Tests pick the
// list that matches the shape they exercise.

#define PSIO3_FOR_EACH_SYMMETRIC_BINARY_FMT(X) \
   X(ssz,     ::psio3::ssz{})                   \
   X(pssz,    ::psio3::pssz{})                \
   X(frac32,  ::psio3::frac32{})                \
   X(bin,     ::psio3::bin{})                   \
   X(borsh,   ::psio3::borsh{})                 \
   X(bincode, ::psio3::bincode{})               \
   X(avro,    ::psio3::avro{})                  \
   X(key,     ::psio3::key{})                   \
   X(capnp,   ::psio3::capnp{})                 \
   X(wit,     ::psio3::wit{})

#define PSIO3_FOR_EACH_RECORD_BINARY_FMT(X) \
   X(ssz,     ::psio3::ssz{})                \
   X(pssz,    ::psio3::pssz{})             \
   X(frac32,  ::psio3::frac32{})             \
   X(bin,     ::psio3::bin{})                \
   X(borsh,   ::psio3::borsh{})              \
   X(bincode, ::psio3::bincode{})            \
   X(avro,    ::psio3::avro{})               \
   X(key,     ::psio3::key{})

#define PSIO3_FOR_EACH_TEXT_FMT(X) \
   X(json,    ::psio3::json{})

#define PSIO3_FOR_EACH_BINARY_FMT(X) \
   PSIO3_FOR_EACH_SYMMETRIC_BINARY_FMT(X)
