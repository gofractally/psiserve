#include <psio/to_ssz.hpp>
#include <psio/to_pssz.hpp>
#include <psio/bounded.hpp>
#include <array>
#include <cstdio>
#include <vector>
#include <string>

template <typename Buf>
void emit(const char* label, const Buf& b)
{
   std::printf("%-50s ", label);
   for (auto c : b) std::printf("%02x", (unsigned char)(c));
   std::printf("\n");
}

int main()
{
   // Fixed-element array [u32; 3] = {1, 2, 3}
   std::array<std::uint32_t, 3> arr_fixed{1, 2, 3};
   emit("array_u32_3.ssz",   psio::convert_to_ssz(arr_fixed));
   emit("array_u32_3.pssz32", psio::convert_to_pssz<psio::frac_format_pssz32>(arr_fixed));

   // Variable-element array [string; 3]
   std::array<std::string, 3> arr_var{"a", "bc", "def"};
   emit("array_str_3.ssz",   psio::convert_to_ssz(arr_var));
   emit("array_str_3.pssz32", psio::convert_to_pssz<psio::frac_format_pssz32>(arr_var));

   // bounded_list<u32, 8> with 3 elements
   psio::bounded_list<std::uint32_t, 8> bl{std::vector<std::uint32_t>{10, 20, 30}};
   emit("bounded_list_u32_3of8.ssz", psio::convert_to_ssz(bl));
   emit("bounded_list_u32_3of8.pssz32", psio::convert_to_pssz<psio::frac_format_pssz32>(bl));

   // bounded_string<16> with "hello"
   psio::bounded_string<16> bs{std::string("hello")};
   emit("bounded_str_hello.ssz",   psio::convert_to_ssz(bs));
   emit("bounded_str_hello.pssz32", psio::convert_to_pssz<psio::frac_format_pssz32>(bs));
}
