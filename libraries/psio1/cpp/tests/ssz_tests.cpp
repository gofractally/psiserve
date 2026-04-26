#include <catch2/catch.hpp>
#include <psio1/bitset.hpp>
#include <psio1/bounded.hpp>
#include <psio1/ext_int.hpp>
#include <psio1/from_ssz.hpp>
#include <psio1/ssz_view.hpp>
#include <psio1/to_ssz.hpp>
#include <psio1/fracpack.hpp>

#include <array>
#include <cstring>
#include <span>
#include <string>
#include <vector>

// ── Compile-time classification tests ─────────────────────────────────────────

static_assert(psio1::ssz_is_fixed_size_v<std::uint8_t>);
static_assert(psio1::ssz_is_fixed_size_v<std::uint64_t>);
static_assert(psio1::ssz_is_fixed_size_v<psio1::uint128>);
static_assert(psio1::ssz_is_fixed_size_v<psio1::uint256>);
static_assert(psio1::ssz_is_fixed_size_v<bool>);
static_assert(psio1::ssz_is_fixed_size_v<float>);
static_assert(psio1::ssz_is_fixed_size_v<double>);
static_assert(psio1::ssz_is_fixed_size_v<psio1::bitvector<512>>);
static_assert(psio1::ssz_is_fixed_size_v<std::array<std::uint64_t, 4>>);
static_assert(!psio1::ssz_is_fixed_size_v<std::vector<std::uint64_t>>);
static_assert(!psio1::ssz_is_fixed_size_v<std::string>);
static_assert(!psio1::ssz_is_fixed_size_v<psio1::bitlist<2048>>);

static_assert(psio1::ssz_fixed_size<std::uint32_t>::value == 4);
static_assert(psio1::ssz_fixed_size<psio1::uint256>::value == 32);
static_assert(psio1::ssz_fixed_size<float>::value == 4);
static_assert(psio1::ssz_fixed_size<double>::value == 8);
static_assert(psio1::ssz_fixed_size<std::array<std::uint64_t, 4>>::value == 32);
static_assert(psio1::ssz_fixed_size<psio1::bitvector<513>>::value == 65);

// ── Primitives round-trip ─────────────────────────────────────────────────────

TEST_CASE("ssz: primitive round-trip", "[ssz]")
{
   auto rt_u32 = [](std::uint32_t v) {
      auto bytes = psio1::convert_to_ssz(v);
      REQUIRE(bytes.size() == 4);
      return psio1::convert_from_ssz<std::uint32_t>(bytes);
   };
   REQUIRE(rt_u32(0) == 0);
   REQUIRE(rt_u32(0xDEADBEEF) == 0xDEADBEEF);
   REQUIRE(rt_u32(0xFFFFFFFF) == 0xFFFFFFFF);

   auto b_true = psio1::convert_to_ssz(true);
   REQUIRE(b_true.size() == 1);
   REQUIRE(static_cast<std::uint8_t>(b_true[0]) == 0x01);
   REQUIRE(psio1::convert_from_ssz<bool>(b_true) == true);

   auto b_false = psio1::convert_to_ssz(false);
   REQUIRE(static_cast<std::uint8_t>(b_false[0]) == 0x00);
}

TEST_CASE("ssz: float / double round-trip", "[ssz]")
{
   // Non-zero bit patterns so a silent write-elision bug (as the pre-fix
   // codec had for double) can't round-trip by accident.
   auto rt_f32 = [](float v) {
      auto bytes = psio1::convert_to_ssz(v);
      REQUIRE(bytes.size() == 4);
      return psio1::convert_from_ssz<float>(bytes);
   };
   REQUIRE(rt_f32(1.5f) == 1.5f);
   REQUIRE(rt_f32(-3.14159f) == -3.14159f);
   REQUIRE(rt_f32(0.0f) == 0.0f);

   auto rt_f64 = [](double v) {
      auto bytes = psio1::convert_to_ssz(v);
      REQUIRE(bytes.size() == 8);
      return psio1::convert_from_ssz<double>(bytes);
   };
   REQUIRE(rt_f64(2.718281828459045) == 2.718281828459045);
   REQUIRE(rt_f64(-1e100) == -1e100);
   REQUIRE(rt_f64(0.0) == 0.0);

}

namespace ssz_float_test
{
   struct BPoint { double x, y; };
   PSIO1_REFLECT(BPoint, definitionWillNotChange(), x, y)
}

TEST_CASE("ssz: std::optional as Union[null, T]", "[ssz]")
{
   // None → single 0x00 byte.
   std::optional<std::uint32_t> none;
   auto nb = psio1::convert_to_ssz(none);
   REQUIRE(nb.size() == 1);
   REQUIRE(static_cast<std::uint8_t>(nb[0]) == 0x00);
   auto round_none = psio1::convert_from_ssz<std::optional<std::uint32_t>>(nb);
   REQUIRE(!round_none.has_value());

   // Some(x) → 0x01 || serialized(x).
   std::optional<std::uint32_t> some = 0xDEADBEEF;
   auto sb = psio1::convert_to_ssz(some);
   REQUIRE(sb.size() == 5);
   REQUIRE(static_cast<std::uint8_t>(sb[0]) == 0x01);
   auto round_some = psio1::convert_from_ssz<std::optional<std::uint32_t>>(sb);
   REQUIRE(round_some.has_value());
   REQUIRE(*round_some == 0xDEADBEEF);

   // Optional<string> — variable-size payload.
   std::optional<std::string> os = std::string("hello");
   auto ob = psio1::convert_to_ssz(os);
   REQUIRE(ob.size() == 6);  // 1 selector + "hello"
   auto round_os = psio1::convert_from_ssz<std::optional<std::string>>(ob);
   REQUIRE(round_os.has_value());
   REQUIRE(*round_os == "hello");

   // Malformed selector is rejected.
   std::vector<char> bad{0x02, 0x00, 0x00, 0x00, 0x00};
   REQUIRE_THROWS(psio1::convert_from_ssz<std::optional<std::uint32_t>>(bad));
}

TEST_CASE("ssz: reflected struct of doubles round-trip", "[ssz]")
{
   // Pre-fix bug: ssz_is_fixed_size<double> was false_type, so BPoint was
   // treated as a variable-size container and serialized as two empty
   // offset slots (8 bytes, no payload). convert_from_ssz silently returned
   // {0.0, 0.0} — the round-trip check below would have failed.
   static_assert(psio1::ssz_is_fixed_size_v<ssz_float_test::BPoint>);
   static_assert(psio1::ssz_fixed_size<ssz_float_test::BPoint>::value == 16);

   ssz_float_test::BPoint p{1.5, -2.5};
   auto bytes = psio1::convert_to_ssz(p);
   REQUIRE(bytes.size() == 16);
   auto decoded = psio1::convert_from_ssz<ssz_float_test::BPoint>(bytes);
   REQUIRE(decoded.x == 1.5);
   REQUIRE(decoded.y == -2.5);
}

TEST_CASE("ssz: uint128 / uint256 round-trip", "[ssz][ext_int]")
{
   psio1::uint128 a = (psio1::uint128{1} << 100) | psio1::uint128{0xDEAD};
   auto          bytes = psio1::convert_to_ssz(a);
   REQUIRE(bytes.size() == 16);
   REQUIRE(psio1::convert_from_ssz<psio1::uint128>(bytes) == a);

   psio1::uint256 b;
   b.limb[0] = 0xAAAAAAAAAAAAAAAA;
   b.limb[3] = 0x1111111111111111;
   auto bb = psio1::convert_to_ssz(b);
   REQUIRE(bb.size() == 32);
   REQUIRE(psio1::convert_from_ssz<psio1::uint256>(bb) == b);
}

// ── std::array = Vector[T, N] ─────────────────────────────────────────────────

TEST_CASE("ssz: std::array of uint32 round-trip", "[ssz][vector]")
{
   std::array<std::uint32_t, 4> arr{1, 2, 3, 0x80000000};
   auto                         bytes = psio1::convert_to_ssz(arr);
   REQUIRE(bytes.size() == 16);
   auto back = psio1::convert_from_ssz<std::array<std::uint32_t, 4>>(bytes);
   REQUIRE(back == arr);
}

// ── Fixed-size all-fixed Container ────────────────────────────────────────────

struct SszValidator
{
   psio1::uint256 pubkey_a;
   psio1::uint256 withdrawal;
   std::uint64_t effective_balance;
   bool          slashed;
   std::uint64_t activation_epoch;
};
PSIO1_REFLECT(SszValidator, definitionWillNotChange(),
             pubkey_a, withdrawal, effective_balance, slashed, activation_epoch)

inline bool operator==(const SszValidator& a, const SszValidator& b)
{
   return a.pubkey_a == b.pubkey_a && a.withdrawal == b.withdrawal &&
          a.effective_balance == b.effective_balance && a.slashed == b.slashed &&
          a.activation_epoch == b.activation_epoch;
}

static_assert(psio1::ssz_is_fixed_size_v<SszValidator>);
static_assert(psio1::ssz_fixed_size<SszValidator>::value == 32 + 32 + 8 + 1 + 8);

TEST_CASE("ssz: fixed Container round-trip", "[ssz][container]")
{
   SszValidator v{};
   v.pubkey_a.limb[0]   = 0xAAAAAAAAAAAAAAAA;
   v.withdrawal.limb[3] = 0xBBBBBBBBBBBBBBBB;
   v.effective_balance  = 32ULL * 1'000'000'000ULL;
   v.slashed            = true;
   v.activation_epoch   = 12345;

   auto bytes = psio1::convert_to_ssz(v);
   REQUIRE(bytes.size() == 32 + 32 + 8 + 1 + 8);
   auto back = psio1::convert_from_ssz<SszValidator>(bytes);
   REQUIRE(back == v);
}

// ── Mixed fixed + variable Container ──────────────────────────────────────────

struct SszBlockHeader
{
   std::uint64_t            slot;
   std::uint64_t            proposer_index;
   psio1::uint256            parent_root;
   std::vector<std::uint64_t> transactions;  // variable
   std::string              graffiti;         // variable
   psio1::uint256            state_root;
};
PSIO1_REFLECT(SszBlockHeader, slot, proposer_index, parent_root, transactions,
             graffiti, state_root)

inline bool operator==(const SszBlockHeader& a, const SszBlockHeader& b)
{
   return a.slot == b.slot && a.proposer_index == b.proposer_index &&
          a.parent_root == b.parent_root && a.transactions == b.transactions &&
          a.graffiti == b.graffiti && a.state_root == b.state_root;
}

static_assert(!psio1::ssz_is_fixed_size_v<SszBlockHeader>);

TEST_CASE("ssz: mixed Container round-trip", "[ssz][container][mixed]")
{
   SszBlockHeader h{};
   h.slot              = 12345;
   h.proposer_index    = 42;
   h.parent_root.limb[0] = 0xDEADBEEFCAFEBABE;
   h.transactions      = {100, 200, 300, 400};
   h.graffiti          = "validator-comments";
   h.state_root.limb[3] = 0xFEEDFACE;

   auto bytes = psio1::convert_to_ssz(h);
   auto back  = psio1::convert_from_ssz<SszBlockHeader>(bytes);
   REQUIRE(back == h);
}

TEST_CASE("ssz: mixed Container with empty variable fields", "[ssz][container][mixed]")
{
   SszBlockHeader h{};
   h.slot         = 0;
   h.transactions = {};
   h.graffiti     = "";

   auto bytes = psio1::convert_to_ssz(h);
   auto back  = psio1::convert_from_ssz<SszBlockHeader>(bytes);
   REQUIRE(back == h);
}

// ── Bitvector and Bitlist ────────────────────────────────────────────────────

TEST_CASE("ssz: bitvector round-trip", "[ssz][bit]")
{
   psio1::bitvector<512> v;
   for (std::size_t i = 0; i < 512; i += 7)
      v.set(i);
   auto bytes = psio1::convert_to_ssz(v);
   REQUIRE(bytes.size() == 64);
   auto back = psio1::convert_from_ssz<psio1::bitvector<512>>(bytes);
   REQUIRE(back == v);
}

TEST_CASE("ssz: bitlist encodes with delimiter bit", "[ssz][bit]")
{
   psio1::bitlist<2048> v;
   // Empty bitlist: 1 byte = 0x01 (only delimiter at bit 0)
   auto empty_bytes = psio1::convert_to_ssz(v);
   REQUIRE(empty_bytes.size() == 1);
   REQUIRE(static_cast<std::uint8_t>(empty_bytes[0]) == 0x01);

   // 3 bits "1, 0, 1": payload bits 0b101, delimiter at bit 3 → byte 0b00001101 = 0x0D
   psio1::bitlist<2048> small;
   small.push_back(true);
   small.push_back(false);
   small.push_back(true);
   auto sb = psio1::convert_to_ssz(small);
   REQUIRE(sb.size() == 1);
   REQUIRE(static_cast<std::uint8_t>(sb[0]) == 0x0D);

   // 8-bit full byte: data 0b11110000, delimiter at bit 8 → [0xF0, 0x01]
   psio1::bitlist<2048> eight;
   for (int i = 0; i < 4; ++i) eight.push_back(false);
   for (int i = 0; i < 4; ++i) eight.push_back(true);
   auto eb = psio1::convert_to_ssz(eight);
   REQUIRE(eb.size() == 2);
   REQUIRE(static_cast<std::uint8_t>(eb[0]) == 0xF0);
   REQUIRE(static_cast<std::uint8_t>(eb[1]) == 0x01);
}

TEST_CASE("ssz: bitlist round-trip", "[ssz][bit]")
{
   psio1::bitlist<2048> v;
   for (std::size_t i = 0; i < 37; ++i)
      v.push_back(i % 3 == 0);
   auto bytes = psio1::convert_to_ssz(v);
   auto back  = psio1::convert_from_ssz<psio1::bitlist<2048>>(bytes);
   REQUIRE(back.size() == v.size());
   for (std::size_t i = 0; i < v.size(); ++i)
      REQUIRE(back.test(i) == v.test(i));
}

TEST_CASE("ssz: bitlist round-trip at byte boundaries", "[ssz][bit]")
{
   // Test every bit count from 0 through 16 to exercise boundary conditions.
   for (std::size_t bc = 0; bc <= 16; ++bc)
   {
      psio1::bitlist<256> v;
      for (std::size_t i = 0; i < bc; ++i)
         v.push_back((i * 7 + 3) % 2 == 0);
      auto bytes = psio1::convert_to_ssz(v);
      auto back  = psio1::convert_from_ssz<psio1::bitlist<256>>(bytes);
      INFO("bit_count=" << bc);
      REQUIRE(back.size() == v.size());
      for (std::size_t i = 0; i < v.size(); ++i)
         REQUIRE(back.test(i) == v.test(i));
   }
}

// ── Interop check: does our SSZ match the spec's layout description? ─────────

TEST_CASE("ssz: Container with one fixed + one variable — layout verification",
          "[ssz][container][layout]")
{
   // Minimal container shape: [u64][offset_to_bytes][bytes payload]
   struct Msg
   {
      std::uint64_t             a;
      std::vector<std::uint8_t> b;
   };
   (void)Msg{};
}

struct SszMsg
{
   std::uint64_t             a;
   std::vector<std::uint8_t> b;
};
PSIO1_REFLECT(SszMsg, a, b)

inline bool operator==(const SszMsg& a, const SszMsg& b)
{
   return a.a == b.a && a.b == b.b;
}

TEST_CASE("ssz: fixed u64 + variable byte list layout", "[ssz][container][layout]")
{
   SszMsg m;
   m.a = 0x0123456789ABCDEFULL;
   m.b = {0xAA, 0xBB, 0xCC};

   auto bytes = psio1::convert_to_ssz(m);
   // [u64 a: 8 bytes][offset to b: 4 bytes = 12][b payload: 3 bytes] = 15 bytes
   REQUIRE(bytes.size() == 15);

   auto read_u64 = [&](std::size_t pos) {
      std::uint64_t v = 0;
      std::memcpy(&v, bytes.data() + pos, 8);
      return v;
   };
   auto read_u32 = [&](std::size_t pos) {
      std::uint32_t v = 0;
      std::memcpy(&v, bytes.data() + pos, 4);
      return v;
   };
   REQUIRE(read_u64(0) == m.a);
   REQUIRE(read_u32(8) == 12);  // offset to b
   REQUIRE(static_cast<std::uint8_t>(bytes[12]) == 0xAA);
   REQUIRE(static_cast<std::uint8_t>(bytes[13]) == 0xBB);
   REQUIRE(static_cast<std::uint8_t>(bytes[14]) == 0xCC);

   auto back = psio1::convert_from_ssz<SszMsg>(bytes);
   REQUIRE(back == m);
}

// ── Bounded list decode enforcement ───────────────────────────────────────────

TEST_CASE("ssz: bounded_list decode rejects oversize", "[ssz][bounded]")
{
   std::vector<std::uint64_t> big(100, 42);
   auto                       bytes = psio1::convert_to_ssz(big);
   REQUIRE_THROWS(psio1::convert_from_ssz<psio1::bounded_list<std::uint64_t, 50>>(bytes));
}

// ── ssz_view<T> zero-copy navigation ─────────────────────────────────────────

TEST_CASE("ssz_view: primitive", "[ssz][view]")
{
   std::uint64_t v     = 0xDEADBEEFCAFEBABEULL;
   auto          bytes = psio1::convert_to_ssz(v);
   auto          view  = psio1::ssz_view_of<std::uint64_t>(bytes);
   REQUIRE(view.get() == v);
   std::uint64_t implicit_copy = view;
   REQUIRE(implicit_copy == v);
}

TEST_CASE("ssz_view: fixed Container field access", "[ssz][view][container]")
{
   SszValidator v{};
   v.pubkey_a.limb[0]  = 0xA;
   v.withdrawal.limb[0] = 0xB;
   v.effective_balance = 32ULL * 1'000'000'000ULL;
   v.slashed           = true;
   v.activation_epoch  = 42;

   auto bytes = psio1::convert_to_ssz(v);
   auto view  = psio1::ssz_view_of<SszValidator>(bytes);

   // Named accessors via PSIO1_REFLECT — same pattern as frac_view.
   auto pk = view.pubkey_a().get();
   REQUIRE(pk.limb[0] == 0xA);

   std::uint64_t eb = view.effective_balance();
   REQUIRE(eb == v.effective_balance);

   bool sl = view.slashed();
   REQUIRE(sl == true);

   std::uint64_t ep = view.activation_epoch();
   REQUIRE(ep == 42);
}

TEST_CASE("ssz_view: mixed Container — variable field span derivation",
          "[ssz][view][container][mixed]")
{
   SszBlockHeader h{};
   h.slot              = 99;
   h.proposer_index    = 7;
   h.parent_root.limb[0] = 0xCAFE;
   h.transactions      = {10, 20, 30};
   h.graffiti          = "hi";
   h.state_root.limb[3] = 0xFACE;

   auto bytes = psio1::convert_to_ssz(h);
   auto view  = psio1::ssz_view_of<SszBlockHeader>(bytes);

   // Fixed fields
   REQUIRE(std::uint64_t(view.slot()) == 99);
   REQUIRE(std::uint64_t(view.proposer_index()) == 7);
   REQUIRE(view.parent_root().get().limb[0] == 0xCAFE);

   auto tx_view = view.transactions();
   REQUIRE(tx_view.size() == 3);
   REQUIRE(std::uint64_t(tx_view[0]) == 10);
   REQUIRE(std::uint64_t(tx_view[1]) == 20);
   REQUIRE(std::uint64_t(tx_view[2]) == 30);

   auto gr_view = view.graffiti();
   REQUIRE(gr_view.view() == "hi");

   REQUIRE(view.state_root().get().limb[3] == 0xFACE);
}

// Struct where validators sits in a list. Simulates a beacon-state-like
// navigation: view.validators()[42].effective_balance.
struct SszState
{
   std::uint64_t             slot;
   std::vector<SszValidator> validators;
};
PSIO1_REFLECT(SszState, slot, validators)

TEST_CASE("ssz_view: nested list-of-fixed-containers", "[ssz][view][beacon]")
{
   SszState st;
   st.slot = 12345;
   for (std::size_t i = 0; i < 100; ++i)
   {
      SszValidator v{};
      v.effective_balance = 32ULL * 1'000'000'000ULL + i;
      v.activation_epoch  = i * 10;
      st.validators.push_back(v);
   }

   auto bytes = psio1::convert_to_ssz(st);
   auto view  = psio1::ssz_view_of<SszState>(bytes);

   REQUIRE(std::uint64_t(view.slot()) == 12345);

   auto vs = view.validators();
   REQUIRE(vs.size() == 100);

   // Spot-check: validator[42].effective_balance / .activation_epoch via
   // the PSIO1_REFLECT named-accessor proxy, same as frac_view.
   auto v42 = vs[42];
   REQUIRE(std::uint64_t(v42.effective_balance()) == 32ULL * 1'000'000'000ULL + 42);
   REQUIRE(std::uint64_t(v42.activation_epoch()) == 42 * 10);
}

// ── ssz_validate<T> ──────────────────────────────────────────────────────────

TEST_CASE("ssz_validate: accepts well-formed buffer", "[ssz][validate]")
{
   SszBlockHeader h{};
   h.slot         = 1;
   h.transactions = {1, 2, 3};
   h.graffiti     = "ok";
   auto bytes = psio1::convert_to_ssz(h);
   REQUIRE_NOTHROW(psio1::ssz_validate<SszBlockHeader>(bytes));
}

TEST_CASE("ssz_validate: rejects truncated buffer", "[ssz][validate]")
{
   SszBlockHeader h{};
   h.transactions = {1, 2};
   auto bytes = psio1::convert_to_ssz(h);
   bytes.resize(bytes.size() - 4);  // truncate
   REQUIRE_THROWS(psio1::ssz_validate<SszBlockHeader>(bytes));
}

TEST_CASE("ssz_validate: rejects tampered offset (out of range)", "[ssz][validate]")
{
   SszBlockHeader h{};
   h.transactions = {1, 2};
   h.graffiti     = "hi";
   auto bytes = psio1::convert_to_ssz(h);

   // Overwrite the first variable offset (at position 8 + 8 + 32 = 48) with a
   // value > container size. That should fail validation.
   std::uint32_t bad = static_cast<std::uint32_t>(bytes.size()) + 100;
   std::memcpy(bytes.data() + 48, &bad, 4);

   REQUIRE_THROWS(psio1::ssz_validate<SszBlockHeader>(bytes));
}

TEST_CASE("ssz_validate: bounded_list rejects oversize", "[ssz][validate][bounded]")
{
   std::vector<std::uint64_t> big(100, 42);
   auto                       bytes = psio1::convert_to_ssz(big);
   REQUIRE_THROWS(
       psio1::ssz_validate<psio1::bounded_list<std::uint64_t, 50>>(bytes));
}

TEST_CASE("ssz_validate: bitlist without delimiter is invalid", "[ssz][validate][bit]")
{
   // A bitlist buffer of all-zero bytes has no delimiter — invalid per spec.
   std::vector<char> bad(4, 0);
   REQUIRE_THROWS(psio1::ssz_validate<psio1::bitlist<1024>>(bad));
}
