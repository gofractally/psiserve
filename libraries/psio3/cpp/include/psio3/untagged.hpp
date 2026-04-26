#pragma once
//
// psio3/untagged.hpp — ADL hook for marking variants as untagged.
//
// Variants whose alternatives have unambiguously distinct shapes can
// be encoded without a discriminator tag — receivers infer the
// alternative from the value's wire layout alone.  Specialise
// `psio3_is_untagged(const MyVariant*)` to opt in:
//
//     constexpr bool psio3_is_untagged(const MyVariant*) { return true; }
//
// Format codecs consult this hook during variant encode/decode.
// (Codec wiring is a follow-up — the hook is registered here so call
// sites can mark types now.)

namespace psio3 {

   template <typename T>
   constexpr bool psio3_is_untagged(const T*)
   {
      return false;
   }

}  // namespace psio3
