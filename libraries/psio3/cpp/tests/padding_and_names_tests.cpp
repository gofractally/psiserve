// Regression guard for two bug classes that surfaced in v1 psio and
// could have reappeared in psio3:
//
//   Bug 1 — nested-reflected memcpy fast path that assumed C++ sizeof
//           matches the wire fixed_size. Broken when the struct has
//           internal padding (e.g. `struct { u8 a; u64 b; }` is 16
//           bytes in C++ but 9 bytes on the fracpack wire). psio3's
//           codecs walk reflected records field-by-field — this test
//           pins that behavior.
//
//   Bug 2 — view/buffer methods named `data()` / `size()` shadowing
//           reflected field accessors. psio3 uses `_psio3_data()` and
//           exposes storage ops as free functions; this test pins the
//           rule by defining a reflected type with a field literally
//           named `data` and encoding/decoding through every format.
//
// Both classes were discovered by a consumer hitting runtime-wrong
// bytes in v1. Keeping these tests here means psio3 can't regress
// into the same shape.

#include <psio3/avro.hpp>
#include <psio3/bin.hpp>
#include <psio3/bincode.hpp>
#include <psio3/borsh.hpp>
#include <psio3/frac.hpp>
#include <psio3/json.hpp>
#include <psio3/pssz.hpp>
#include <psio3/reflect.hpp>
#include <psio3/ssz.hpp>

#include <catch.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace reg {

   // Padded inner — sizeof(C++) = 16 due to alignment, wire fixed_size
   // on a packed codec = 9. A memcpy of 16 bytes would mis-read wire
   // bytes; codec must walk field-by-field.
   struct PaddedInner
   {
      std::uint8_t  a;   // 1 byte
      std::uint64_t b;   // 8 bytes @ offset 8 in C++, @ offset 1 on wire
   };
   PSIO3_REFLECT(PaddedInner, a, b)

   struct WithInner
   {
      PaddedInner             inner;
      std::uint32_t           id;
   };
   PSIO3_REFLECT(WithInner, inner, id)

   // Fields named `data` / `size` / `bytes` — the exact names v1's
   // view methods collided with.
   struct Segment
   {
      std::uint64_t             id;
      std::vector<std::uint8_t> data;
      std::uint32_t             size;
   };
   PSIO3_REFLECT(Segment, id, data, size)

}  // namespace reg

#define PADDING_CASE(FmtName, FmtValue)                                \
   TEST_CASE("padding: PaddedInner nested roundtrip [" #FmtName "]",   \
             "[padding][" #FmtName "]")                                \
   {                                                                   \
      reg::WithInner in{{0xAB, 0xDEADBEEFCAFEBABEULL}, 42};            \
      auto bv = psio3::encode(FmtValue, in);                           \
      auto out = psio3::decode<reg::WithInner>(                        \
         FmtValue, std::span<const char>{bv});                         \
      REQUIRE(out.inner.a == 0xAB);                                    \
      REQUIRE(out.inner.b == 0xDEADBEEFCAFEBABEULL);                   \
      REQUIRE(out.id      == 42);                                      \
   }
PADDING_CASE(ssz,     ::psio3::ssz{})
PADDING_CASE(pssz32,  ::psio3::pssz32{})
PADDING_CASE(frac32,  ::psio3::frac32{})
PADDING_CASE(bin,     ::psio3::bin{})
PADDING_CASE(borsh,   ::psio3::borsh{})
PADDING_CASE(bincode, ::psio3::bincode{})
PADDING_CASE(avro,    ::psio3::avro{})
#undef PADDING_CASE

#define FIELD_NAME_CASE(FmtName, FmtValue)                             \
   TEST_CASE("field names: Segment with `data`/`size` [" #FmtName "]", \
             "[fieldname][" #FmtName "]")                              \
   {                                                                   \
      reg::Segment in{99, {0x01, 0x02, 0x03, 0x04}, 12345};            \
      auto bv = psio3::encode(FmtValue, in);                           \
      auto out = psio3::decode<reg::Segment>(                          \
         FmtValue, std::span<const char>{bv});                         \
      REQUIRE(out.id   == 99);                                         \
      REQUIRE(out.data == std::vector<std::uint8_t>{1, 2, 3, 4});      \
      REQUIRE(out.size == 12345);                                      \
   }
FIELD_NAME_CASE(ssz,     ::psio3::ssz{})
FIELD_NAME_CASE(pssz32,  ::psio3::pssz32{})
FIELD_NAME_CASE(frac32,  ::psio3::frac32{})
FIELD_NAME_CASE(bin,     ::psio3::bin{})
FIELD_NAME_CASE(borsh,   ::psio3::borsh{})
FIELD_NAME_CASE(bincode, ::psio3::bincode{})
FIELD_NAME_CASE(avro,    ::psio3::avro{})
FIELD_NAME_CASE(json,    ::psio3::json{})
#undef FIELD_NAME_CASE
