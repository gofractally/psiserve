#include <psio/to_ssz.hpp>
#include <psio/to_pssz.hpp>
#include <psio/bitset.hpp>
#include <cstdio>

template <typename Buf>
void emit(const char* label, const Buf& b)
{
   std::printf("%-45s ", label);
   for (auto c : b) std::printf("%02x", (unsigned char)(c));
   std::printf("\n");
}

int main()
{
   // bitvector<12>, bits 0, 3, 11 set
   // Byte 0: bits 0-7 = 00001001 → 0x09
   // Byte 1: bits 8-11 = 1000 (bit 11 set) → 0x08
   // Total = 2 bytes = 0x09 0x08
   psio::bitvector<12> bv;
   bv.set(0); bv.set(3); bv.set(11);
   emit("bitvector_12.ssz",   psio::convert_to_ssz(bv));
   emit("bitvector_12.pssz32", psio::convert_to_pssz<psio::frac_format_pssz32>(bv));

   // bitlist<16> length 5, bits 0, 2 set
   // data bits: 10100 → byte 0 = 00000101 = 0x05
   // delimiter bit at position 5 → byte 0 becomes 00100101 = 0x25
   psio::bitlist<16> bl;
   bl.resize(5);
   bl.set(0); bl.set(2);
   emit("bitlist_len5.ssz",   psio::convert_to_ssz(bl));
   emit("bitlist_len5.pssz32", psio::convert_to_pssz<psio::frac_format_pssz32>(bl));

   // Empty bitlist: len 0, only the delimiter bit at position 0 → 0x01
   psio::bitlist<16> bl0;
   emit("bitlist_len0.ssz",   psio::convert_to_ssz(bl0));
   emit("bitlist_len0.pssz32", psio::convert_to_pssz<psio::frac_format_pssz32>(bl0));
}
