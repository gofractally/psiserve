#include <psio1/to_ssz.hpp>
#include <psio1/to_pssz.hpp>
#include <psio1/ext_int.hpp>
#include <cstdio>
#include <vector>

template <typename Buf>
void emit(const char* label, const Buf& b)
{
   std::printf("%-45s ", label);
   for (auto c : b) std::printf("%02x", (unsigned char)(c));
   std::printf("\n");
}

int main()
{
   // u128 - pattern 0x00FF00FF_00FF00FF_00FF00FF_00FF00FF
   unsigned __int128 u128v = ((unsigned __int128)0x00FF00FF00FF00FFULL << 64) |
                               (unsigned __int128)0x00FF00FF00FF00FFULL;
   emit("u128_ff.ssz",   psio1::convert_to_ssz(u128v));
   emit("u128_ff.pssz32", psio1::convert_to_pssz<psio1::frac_format_pssz32>(u128v));

   __int128 i128v = -42;
   emit("i128_neg42.ssz",   psio1::convert_to_ssz(i128v));
   emit("i128_neg42.pssz32", psio1::convert_to_pssz<psio1::frac_format_pssz32>(i128v));

   // uint256 - four u64 limbs: [1, 2, 3, 4] little-endian on wire
   psio1::uint256 u256v;
   u256v.limb[0] = 1;
   u256v.limb[1] = 2;
   u256v.limb[2] = 3;
   u256v.limb[3] = 4;
   emit("u256_1234.ssz",   psio1::convert_to_ssz(u256v));
   emit("u256_1234.pssz32", psio1::convert_to_pssz<psio1::frac_format_pssz32>(u256v));
}
