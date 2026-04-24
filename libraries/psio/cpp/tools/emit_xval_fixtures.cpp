// Emits hex fixtures for canonical shapes across every format psio
// supports. Rust cross-validation tests consume this output.
#include <psio/fracpack.hpp>
#include <psio/to_bincode.hpp>
#include <psio/to_borsh.hpp>
#include <psio/to_pssz.hpp>
#include <psio/to_ssz.hpp>
#include <psio/reflect.hpp>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <optional>

template <typename Buf>
void emit(const char* label, const Buf& b)
{
   std::printf("%-50s ", label);
   for (auto c : b) std::printf("%02x", (unsigned char)(c));
   std::printf("\n");
}

struct Point { std::uint32_t x, y; };
PSIO_REFLECT(Point, definitionWillNotChange(), x, y)

struct Named { std::string name; std::uint32_t value; };
PSIO_REFLECT(Named, name, value)

int main()
{
   // ── Primitives ──
   std::uint32_t u32v = 0xDEADBEEF;
   emit("u32_DEADBEEF.fracpack",  psio::to_frac(u32v));
   emit("u32_DEADBEEF.fracpack16", psio::to_frac16(u32v));
   emit("u32_DEADBEEF.bincode",   psio::convert_to_bincode(u32v));
   emit("u32_DEADBEEF.borsh",     psio::convert_to_borsh(u32v));
   emit("u32_DEADBEEF.ssz",       psio::convert_to_ssz(u32v));
   emit("u32_DEADBEEF.pssz32",    psio::convert_to_pssz<psio::frac_format_pssz32>(u32v));

   // ── String ──
   std::string s = "hello";
   emit("str_hello.fracpack",   psio::to_frac(s));
   emit("str_hello.fracpack16", psio::to_frac16(s));
   emit("str_hello.bincode",    psio::convert_to_bincode(s));
   emit("str_hello.borsh",      psio::convert_to_borsh(s));
   emit("str_hello.ssz",        psio::convert_to_ssz(s));
   emit("str_hello.pssz32",     psio::convert_to_pssz<psio::frac_format_pssz32>(s));

   // ── Vec<u32> ──
   std::vector<std::uint32_t> vu = {1, 2, 3};
   emit("vec_u32_123.fracpack",   psio::to_frac(vu));
   emit("vec_u32_123.fracpack16", psio::to_frac16(vu));
   emit("vec_u32_123.bincode",    psio::convert_to_bincode(vu));
   emit("vec_u32_123.borsh",      psio::convert_to_borsh(vu));
   emit("vec_u32_123.ssz",        psio::convert_to_ssz(vu));
   emit("vec_u32_123.pssz32",     psio::convert_to_pssz<psio::frac_format_pssz32>(vu));

   // ── Vec<String> ──
   std::vector<std::string> vs = {"a", "bc", "def"};
   emit("vec_str_abc.fracpack",   psio::to_frac(vs));
   emit("vec_str_abc.fracpack16", psio::to_frac16(vs));
   emit("vec_str_abc.bincode",    psio::convert_to_bincode(vs));
   emit("vec_str_abc.borsh",      psio::convert_to_borsh(vs));
   emit("vec_str_abc.ssz",        psio::convert_to_ssz(vs));
   emit("vec_str_abc.pssz32",     psio::convert_to_pssz<psio::frac_format_pssz32>(vs));

   // ── Option<u32> ──
   std::optional<std::uint32_t> some42 = 42;
   std::optional<std::uint32_t> none_u32;
   emit("opt_u32_42.fracpack",    psio::to_frac(some42));
   emit("opt_u32_none.fracpack",  psio::to_frac(none_u32));
   emit("opt_u32_42.bincode",     psio::convert_to_bincode(some42));
   emit("opt_u32_none.bincode",   psio::convert_to_bincode(none_u32));
   emit("opt_u32_42.borsh",       psio::convert_to_borsh(some42));
   emit("opt_u32_none.borsh",     psio::convert_to_borsh(none_u32));
   emit("opt_u32_42.ssz",         psio::convert_to_ssz(some42));
   emit("opt_u32_none.ssz",       psio::convert_to_ssz(none_u32));
   emit("opt_u32_42.pssz32",      psio::convert_to_pssz<psio::frac_format_pssz32>(some42));
   emit("opt_u32_none.pssz32",    psio::convert_to_pssz<psio::frac_format_pssz32>(none_u32));

   // ── DWNC struct Point ──
   Point p{100, 200};
   emit("point_100_200.fracpack",   psio::to_frac(p));
   emit("point_100_200.bincode",    psio::convert_to_bincode(p));
   emit("point_100_200.borsh",      psio::convert_to_borsh(p));
   emit("point_100_200.ssz",        psio::convert_to_ssz(p));
   emit("point_100_200.pssz32",     psio::convert_to_pssz<psio::frac_format_pssz32>(p));

   // ── Extensible struct Named ──
   Named n{"alice", 77};
   emit("named_alice_77.fracpack",   psio::to_frac(n));
   emit("named_alice_77.bincode",    psio::convert_to_bincode(n));
   emit("named_alice_77.borsh",      psio::convert_to_borsh(n));
   emit("named_alice_77.ssz",        psio::convert_to_ssz(n));
   emit("named_alice_77.pssz32",     psio::convert_to_pssz<psio::frac_format_pssz32>(n));
}
