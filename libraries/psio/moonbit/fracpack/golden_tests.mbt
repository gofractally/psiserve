// Golden binary comparison tests for fracpack.
// Tests all 71 cases from test_vectors/vectors.json using the builder API.

fn hex_to_bytes(hex : String) -> Array[Byte] {
  let result : Array[Byte] = []
  let mut i = 0
  while i < hex.length() {
    let hi = hex_val(hex[i])
    let lo = hex_val(hex[i + 1])
    result.push((hi * 16 + lo).to_byte())
    i = i + 2
  }
  result
}

fn hex_val(c : UInt16) -> Int {
  let code = c.to_int()
  if code >= 48 && code <= 57 {
    code - 48
  } else if code >= 65 && code <= 70 {
    code - 55
  } else if code >= 97 && code <= 102 {
    code - 87
  } else {
    0
  }
}

fn expect_pack(spec : TypeSpec, val : Value, expected_hex : String) -> Unit!Error {
  let expected = hex_to_bytes(expected_hex)
  let actual = pack(spec, val)
  assert_eq!(actual.length(), expected.length())
  for i = 0; i < expected.length(); i = i + 1 {
    assert_eq!(actual[i], expected[i])
  }
}

// ── Reusable specs ─────────────────────────────────────────────────────

fn inner_spec() -> TypeSpec {
  T::object("Inner", [F::u32_("value"), F::string("label")])
}

// ── 1. FixedInts (4 cases) ─────────────────────────────────────────────

test "FixedInts/zeros" {
  let s = T::fixed("FixedInts", [F::i32_("x"), F::i32_("y")])
  expect_pack!(s, V::obj([V::i32_(0), V::i32_(0)]), "0000000000000000")
}

test "FixedInts/positive" {
  let s = T::fixed("FixedInts", [F::i32_("x"), F::i32_("y")])
  expect_pack!(s, V::obj([V::i32_(42), V::i32_(100)]), "2A00000064000000")
}

test "FixedInts/negative" {
  let s = T::fixed("FixedInts", [F::i32_("x"), F::i32_("y")])
  expect_pack!(s, V::obj([V::i32_(-1), V::i32_(-2147483648)]), "FFFFFFFF00000080")
}

test "FixedInts/max" {
  let s = T::fixed("FixedInts", [F::i32_("x"), F::i32_("y")])
  expect_pack!(s, V::obj([V::i32_(2147483647), V::i32_(2147483647)]), "FFFFFF7FFFFFFF7F")
}

// ── 2. FixedMixed (3 cases) ────────────────────────────────────────────

test "FixedMixed/zeros" {
  let s = T::fixed("FixedMixed", [F::bool_("b"), F::u8_("u8"), F::u16_("u16"), F::u32_("u32"), F::u64_("u64")])
  expect_pack!(s, V::obj([V::bool_(false), V::u8_(b'\x00'), V::u16_(0), V::u32_(0U), V::u64_(0UL)]), "00000000000000000000000000000000")
}

test "FixedMixed/ones" {
  let s = T::fixed("FixedMixed", [F::bool_("b"), F::u8_("u8"), F::u16_("u16"), F::u32_("u32"), F::u64_("u64")])
  expect_pack!(s, V::obj([V::bool_(true), V::u8_(b'\x01'), V::u16_(1), V::u32_(1U), V::u64_(1UL)]), "01010100010000000100000000000000")
}

test "FixedMixed/max" {
  let s = T::fixed("FixedMixed", [F::bool_("b"), F::u8_("u8"), F::u16_("u16"), F::u32_("u32"), F::u64_("u64")])
  expect_pack!(s, V::obj([V::bool_(true), V::u8_(b'\xFF'), V::u16_(65535), V::u32_(4294967295U), V::u64_(18446744073709551615UL)]), "01FFFFFFFFFFFFFFFFFFFFFFFFFFFFFF")
}

// ── 3. AllPrimitives (5 cases) ──────────────────────────────────────────

fn all_prim_spec() -> TypeSpec {
  T::object("AllPrimitives", [F::bool_("b"), F::u8_("u8v"), F::i8_("i8v"), F::u16_("u16v"), F::i16_("i16v"), F::u32_("u32v"), F::i32_("i32v"), F::u64_("u64v"), F::i64_("i64v"), F::f32_("f32v"), F::f64_("f64v")])
}

test "AllPrimitives/zeros" {
  expect_pack!(all_prim_spec(), V::obj([V::bool_(false), V::u8_(b'\x00'), V::i8_(0), V::u16_(0), V::i16_(0), V::u32_(0U), V::i32_(0), V::u64_(0UL), V::i64_(0L), V::f32_(0.0), V::f64_(0.0)]), "2B0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000")
}

test "AllPrimitives/ones" {
  expect_pack!(all_prim_spec(), V::obj([V::bool_(true), V::u8_(b'\x01'), V::i8_(1), V::u16_(1), V::i16_(1), V::u32_(1U), V::i32_(1), V::u64_(1UL), V::i64_(1L), V::f32_(1.0), V::f64_(1.0)]), "2B00010101010001000100000001000000010000000000000001000000000000000000803F000000000000F03F")
}

test "AllPrimitives/max_unsigned" {
  expect_pack!(all_prim_spec(), V::obj([V::bool_(true), V::u8_(b'\xFF'), V::i8_(127), V::u16_(65535), V::i16_(32767), V::u32_(4294967295U), V::i32_(2147483647), V::u64_(18446744073709551615UL), V::i64_(9223372036854775807L), V::f32_(3.141590118408203), V::f64_(2.718281828459045)]), "2B0001FF7FFFFFFF7FFFFFFFFFFFFFFF7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF7FD00F49406957148B0ABF0540")
}

test "AllPrimitives/min_signed" {
  expect_pack!(all_prim_spec(), V::obj([V::bool_(false), V::u8_(b'\x00'), V::i8_(-128), V::u16_(0), V::i16_(-32768), V::u32_(0U), V::i32_(-2147483648), V::u64_(0UL), V::i64_(-9223372036854775808L), V::f32_(-1.0), V::f64_(-1.0)]), "2B0000008000000080000000000000008000000000000000000000000000000080000080BF000000000000F0BF")
}

test "AllPrimitives/fractional_floats" {
  expect_pack!(all_prim_spec(), V::obj([V::bool_(false), V::u8_(b'\x00'), V::i8_(0), V::u16_(0), V::i16_(0), V::u32_(0U), V::i32_(0), V::u64_(0UL), V::i64_(0L), V::f32_(0.10000000149011612), V::f64_(0.1)]), "2B0000000000000000000000000000000000000000000000000000000000000000CDCCCC3D9A9999999999B93F")
}

// ── 4. SingleBool (2 cases) ────────────────────────────────────────────

test "SingleBool/false" {
  expect_pack!(T::fixed("SB", [F::bool_("value")]), V::obj([V::bool_(false)]), "00")
}

test "SingleBool/true" {
  expect_pack!(T::fixed("SB", [F::bool_("value")]), V::obj([V::bool_(true)]), "01")
}

// ── 5. SingleU32 (4 cases) ─────────────────────────────────────────────

test "SingleU32/zero" {
  expect_pack!(T::fixed("SU", [F::u32_("value")]), V::obj([V::u32_(0U)]), "00000000")
}

test "SingleU32/one" {
  expect_pack!(T::fixed("SU", [F::u32_("value")]), V::obj([V::u32_(1U)]), "01000000")
}

test "SingleU32/max" {
  expect_pack!(T::fixed("SU", [F::u32_("value")]), V::obj([V::u32_(4294967295U)]), "FFFFFFFF")
}

test "SingleU32/hex_pattern" {
  expect_pack!(T::fixed("SU", [F::u32_("value")]), V::obj([V::u32_(3735928559U)]), "EFBEADDE")
}

// ── 6. SingleString (6 cases) ──────────────────────────────────────────

test "SingleString/empty" {
  expect_pack!(T::object("SS", [F::string("value")]), V::obj([V::str("")]), "040000000000")
}

test "SingleString/hello" {
  expect_pack!(T::object("SS", [F::string("value")]), V::obj([V::str("hello")]), "0400040000000500000068656C6C6F")
}

test "SingleString/with_spaces" {
  expect_pack!(T::object("SS", [F::string("value")]), V::obj([V::str("hello world")]), "0400040000000B00000068656C6C6F20776F726C64")
}

test "SingleString/special_chars" {
  expect_pack!(T::object("SS", [F::string("value")]), V::obj([V::str("tab\there\nnewline")]), "0400040000001000000074616209686572650A6E65776C696E65")
}

test "SingleString/unicode" {
  expect_pack!(T::object("SS", [F::string("value")]), V::obj([V::str("caf\u{00E9} \u{2615} \u{65E5}\u{672C}\u{8A9E}")]), "04000400000013000000636166C3A920E2989520E697A5E69CACE8AA9E")
}

test "SingleString/escapes" {
  expect_pack!(T::object("SS", [F::string("value")]), V::obj([V::str("quote\"backslash\\")]), "0400040000001000000071756F7465226261636B736C6173685C")
}

// ── 7. WithStrings (2 cases) ───────────────────────────────────────────

test "WithStrings/all_empty" {
  let s = T::object("WS", [F::string("empty_str"), F::string("hello"), F::string("unicode")])
  expect_pack!(s, V::obj([V::str(""), V::str(""), V::str("")]), "0C00000000000000000000000000")
}

test "WithStrings/mixed" {
  let s = T::object("WS", [F::string("empty_str"), F::string("hello"), F::string("unicode")])
  expect_pack!(s, V::obj([V::str(""), V::str("hello"), V::str("\u{00E9}mojis: \u{1F389}\u{1F680}")]), "0C0000000000080000000D0000000500000068656C6C6F11000000C3A96D6F6A69733A20F09F8E89F09F9A80")
}

// ── 8. WithVectors (5 cases) ───────────────────────────────────────────

test "WithVectors/both_empty" {
  let s = T::object("WV", [F::vec("ints", U32), F::vec("strings", String_)])
  expect_pack!(s, V::obj([V::vec([]), V::vec([])]), "08000000000000000000")
}

test "WithVectors/ints_only" {
  let s = T::object("WV", [F::vec("ints", U32), F::vec("strings", String_)])
  expect_pack!(s, V::obj([V::vec([V::u32_(1U), V::u32_(2U), V::u32_(3U)]), V::vec([])]), "080008000000000000000C000000010000000200000003000000")
}

test "WithVectors/strings_only" {
  let s = T::object("WV", [F::vec("ints", U32), F::vec("strings", String_)])
  expect_pack!(s, V::obj([V::vec([]), V::vec([V::str("a"), V::str("bb"), V::str("ccc")])]), "080000000000040000000C0000000C0000000D0000000F000000010000006102000000626203000000636363")
}

test "WithVectors/both_filled" {
  let s = T::object("WV", [F::vec("ints", U32), F::vec("strings", String_)])
  expect_pack!(s, V::obj([V::vec([V::u32_(10U), V::u32_(20U)]), V::vec([V::str("hello"), V::str("world")])]), "08000800000010000000080000000A0000001400000008000000080000000D0000000500000068656C6C6F05000000776F726C64")
}

test "WithVectors/single_elements" {
  let s = T::object("WV", [F::vec("ints", U32), F::vec("strings", String_)])
  expect_pack!(s, V::obj([V::vec([V::u32_(42U)]), V::vec([V::str("only")])]), "0800080000000C000000040000002A0000000400000004000000040000006F6E6C79")
}

// ── 9. WithOptionals (5 cases) ─────────────────────────────────────────

test "WithOptionals/both_null" {
  let s = T::object("WO", [F::opt("opt_int", U32), F::opt("opt_str", String_)])
  expect_pack!(s, V::obj([V::opt_none(), V::opt_none()]), "0000")
}

test "WithOptionals/int_only" {
  let s = T::object("WO", [F::opt("opt_int", U32), F::opt("opt_str", String_)])
  expect_pack!(s, V::obj([V::opt(V::u32_(42U)), V::opt_none()]), "0400040000002A000000")
}

test "WithOptionals/str_only" {
  let s = T::object("WO", [F::opt("opt_int", U32), F::opt("opt_str", String_)])
  expect_pack!(s, V::obj([V::opt_none(), V::opt(V::str("hello"))]), "080001000000040000000500000068656C6C6F")
}

test "WithOptionals/both_present" {
  let s = T::object("WO", [F::opt("opt_int", U32), F::opt("opt_str", String_)])
  expect_pack!(s, V::obj([V::opt(V::u32_(99U)), V::opt(V::str("world"))]), "080008000000080000006300000005000000776F726C64")
}

test "WithOptionals/zero_int" {
  let s = T::object("WO", [F::opt("opt_int", U32), F::opt("opt_str", String_)])
  expect_pack!(s, V::obj([V::opt(V::u32_(0U)), V::opt_none()]), "04000400000000000000")
}

// ── 10. Inner (3 cases) ────────────────────────────────────────────────

test "Inner/simple" {
  expect_pack!(inner_spec(), V::obj([V::u32_(42U), V::str("hello")]), "08002A000000040000000500000068656C6C6F")
}

test "Inner/empty_label" {
  expect_pack!(inner_spec(), V::obj([V::u32_(0U), V::str("")]), "08000000000000000000")
}

test "Inner/max_value" {
  expect_pack!(inner_spec(), V::obj([V::u32_(4294967295U), V::str("max")]), "0800FFFFFFFF04000000030000006D6178")
}

// ── 11. Outer (3 cases) ────────────────────────────────────────────────

test "Outer/simple" {
  let s = T::object("Outer", [F::object("inner", inner_spec()), F::string("name")])
  expect_pack!(s, V::obj([V::obj([V::u32_(1U), V::str("inner")]), V::str("outer")]), "080008000000170000000800010000000400000005000000696E6E6572050000006F75746572")
}

test "Outer/empty_strings" {
  let s = T::object("Outer", [F::object("inner", inner_spec()), F::string("name")])
  expect_pack!(s, V::obj([V::obj([V::u32_(0U), V::str("")]), V::str("")]), "0800080000000000000008000000000000000000")
}

test "Outer/nested_unicode" {
  let s = T::object("Outer", [F::object("inner", inner_spec()), F::string("name")])
  expect_pack!(s, V::obj([V::obj([V::u32_(42U), V::str("caf\u{00E9}")]), V::str("na\u{00EF}ve")]), "0800080000001700000008002A0000000400000005000000636166C3A9060000006E61C3AF7665")
}

// ── 12. WithVariant (5 cases) ──────────────────────────────────────────

fn variant_spec() -> TypeSpec {
  let var_def = T::variant("DataVariant", [F::u32_("uint32"), F::string("string"), F::object("Inner", inner_spec())])
  T::object("WithVariant", [F::variant("data", var_def)])
}

test "WithVariant/uint32_alt" {
  expect_pack!(variant_spec(), V::obj([V::variant(0, V::u32_(42U))]), "04000400000000040000002A000000")
}

test "WithVariant/string_alt" {
  expect_pack!(variant_spec(), V::obj([V::variant(1, V::str("hello"))]), "04000400000001090000000500000068656C6C6F")
}

test "WithVariant/struct_alt" {
  expect_pack!(variant_spec(), V::obj([V::variant(2, V::obj([V::u32_(7U), V::str("variant_inner")]))]), "040004000000021B000000080007000000040000000D00000076617269616E745F696E6E6572")
}

test "WithVariant/uint32_zero" {
  expect_pack!(variant_spec(), V::obj([V::variant(0, V::u32_(0U))]), "040004000000000400000000000000")
}

test "WithVariant/string_empty" {
  expect_pack!(variant_spec(), V::obj([V::variant(1, V::str(""))]), "040004000000010400000000000000")
}

// ── 13. VecOfStructs (3 cases) ─────────────────────────────────────────

test "VecOfStructs/empty" {
  expect_pack!(T::object("VOS", [F::vec_obj("items", inner_spec())]), V::obj([V::vec([])]), "040000000000")
}

test "VecOfStructs/single" {
  expect_pack!(T::object("VOS", [F::vec_obj("items", inner_spec())]), V::obj([V::vec([V::obj([V::u32_(1U), V::str("one")])])]), "040004000000040000000400000008000100000004000000030000006F6E65")
}

test "VecOfStructs/multiple" {
  expect_pack!(T::object("VOS", [F::vec_obj("items", inner_spec())]), V::obj([V::vec([V::obj([V::u32_(1U), V::str("one")]), V::obj([V::u32_(2U), V::str("two")]), V::obj([V::u32_(3U), V::str("three")])])]), "0400040000000C0000000C000000190000002600000008000100000004000000030000006F6E65080002000000040000000300000074776F08000300000004000000050000007468726565")
}

// ── 14. OptionalStruct (2 cases) ───────────────────────────────────────

test "OptionalStruct/null" {
  expect_pack!(T::object("OS", [F::opt_obj("item", inner_spec())]), V::obj([V::opt_none()]), "0000")
}

test "OptionalStruct/present" {
  expect_pack!(T::object("OS", [F::opt_obj("item", inner_spec())]), V::obj([V::opt(V::obj([V::u32_(42U), V::str("exists")]))]), "04000400000008002A0000000400000006000000657869737473")
}

// ── 15. VecOfOptionals (4 cases) ───────────────────────────────────────

test "VecOfOptionals/empty" {
  expect_pack!(T::object("VOO", [F::vec("items", Optional)]), V::obj([V::vec([])]), "040000000000")
}

test "VecOfOptionals/all_null" {
  expect_pack!(T::object("VOO", [F::vec("items", Optional)]), V::obj([V::vec([V::opt_none(), V::opt_none(), V::opt_none()])]), "0400040000000C000000010000000100000001000000")
}

test "VecOfOptionals/all_present" {
  expect_pack!(T::object("VOO", [F::vec("items", Optional)]), V::obj([V::vec([V::opt(V::u32_(1U)), V::opt(V::u32_(2U)), V::opt(V::u32_(3U))])]), "0400040000000C0000000C0000000C0000000C000000010000000200000003000000")
}

test "VecOfOptionals/mixed" {
  expect_pack!(T::object("VOO", [F::vec("items", Optional)]), V::obj([V::vec([V::opt(V::u32_(1U)), V::opt_none(), V::opt(V::u32_(3U)), V::opt_none()])]), "0400040000001000000010000000010000000C000000010000000100000003000000")
}

// ── 16. OptionalVec (3 cases) ──────────────────────────────────────────

test "OptionalVec/null" {
  expect_pack!(T::object("OV", [F::opt("items", Vec)]), V::obj([V::opt_none()]), "0000")
}

test "OptionalVec/empty_vec" {
  expect_pack!(T::object("OV", [F::opt("items", Vec)]), V::obj([V::opt(V::vec([]))]), "040000000000")
}

test "OptionalVec/with_values" {
  expect_pack!(T::object("OV", [F::opt("items", Vec)]), V::obj([V::opt(V::vec([V::u32_(10U), V::u32_(20U), V::u32_(30U)]))]), "0400040000000C0000000A000000140000001E000000")
}

// ── 17. NestedVecs (4 cases) ───────────────────────────────────────────

test "NestedVecs/empty" {
  expect_pack!(T::object("NV", [F::vec("matrix", Vec)]), V::obj([V::vec([])]), "040000000000")
}

test "NestedVecs/empty_rows" {
  expect_pack!(T::object("NV", [F::vec("matrix", Vec)]), V::obj([V::vec([V::vec([]), V::vec([]), V::vec([])])]), "0400040000000C000000000000000000000000000000")
}

test "NestedVecs/identity_2x2" {
  expect_pack!(T::object("NV", [F::vec("matrix", Vec)]), V::obj([V::vec([V::vec([V::u32_(1U), V::u32_(0U)]), V::vec([V::u32_(0U), V::u32_(1U)])])]), "040004000000080000000800000010000000080000000100000000000000080000000000000001000000")
}

test "NestedVecs/ragged" {
  expect_pack!(T::object("NV", [F::vec("matrix", Vec)]), V::obj([V::vec([V::vec([V::u32_(1U)]), V::vec([V::u32_(2U), V::u32_(3U)]), V::vec([V::u32_(4U), V::u32_(5U), V::u32_(6U)])])]), "0400040000000C0000000C000000100000001800000004000000010000000800000002000000030000000C000000040000000500000006000000")
}

// ── 18. FixedArray (3 cases) ───────────────────────────────────────────

test "FixedArray/zeros" {
  expect_pack!(T::fixed("FA", [F::u32_("a0"), F::u32_("a1"), F::u32_("a2")]), V::obj([V::u32_(0U), V::u32_(0U), V::u32_(0U)]), "000000000000000000000000")
}

test "FixedArray/sequence" {
  expect_pack!(T::fixed("FA", [F::u32_("a0"), F::u32_("a1"), F::u32_("a2")]), V::obj([V::u32_(1U), V::u32_(2U), V::u32_(3U)]), "010000000200000003000000")
}

test "FixedArray/max" {
  expect_pack!(T::fixed("FA", [F::u32_("a0"), F::u32_("a1"), F::u32_("a2")]), V::obj([V::u32_(4294967295U), V::u32_(4294967295U), V::u32_(4294967295U)]), "FFFFFFFFFFFFFFFFFFFFFFFF")
}

// ── 19. Complex (3 cases) ──────────────────────────────────────────────

fn complex_spec() -> TypeSpec {
  T::object("Complex", [F::vec_obj("items", inner_spec()), F::opt("opt_vec", Vec), F::vec("vec_opt", Optional), F::opt_obj("opt_struct", inner_spec())])
}

test "Complex/all_empty" {
  expect_pack!(complex_spec(), V::obj([V::vec([]), V::opt_none(), V::vec([]), V::opt_none()]), "0C00000000000100000000000000")
}

test "Complex/all_populated" {
  expect_pack!(complex_spec(), V::obj([V::vec([V::obj([V::u32_(1U), V::str("a")]), V::obj([V::u32_(2U), V::str("b")])]), V::opt(V::vec([V::u32_(10U), V::u32_(20U)])), V::vec([V::opt(V::str("x")), V::opt_none(), V::opt(V::str("z"))]), V::opt(V::obj([V::u32_(99U), V::str("present")]))]), "100010000000360000003E00000054000000080000000800000013000000080001000000040000000100000061080002000000040000000100000062080000000A000000140000000C0000000C00000001000000090000000100000078010000007A080063000000040000000700000070726573656E74")
}

test "Complex/sparse" {
  expect_pack!(complex_spec(), V::obj([V::vec([V::obj([V::u32_(42U), V::str("only")])]), V::opt_none(), V::vec([V::opt_none(), V::opt_none()]), V::opt_none()]), "0C000C000000010000001E000000040000000400000008002A00000004000000040000006F6E6C79080000000100000001000000")
}

// ── 20. EmptyExtensible (2 cases) ──────────────────────────────────────

test "EmptyExtensible/zero" {
  expect_pack!(T::object("EE", [F::u32_("dummy")]), V::obj([V::u32_(0U)]), "040000000000")
}

test "EmptyExtensible/max" {
  expect_pack!(T::object("EE", [F::u32_("dummy")]), V::obj([V::u32_(4294967295U)]), "0400FFFFFFFF")
}

// ── Packable trait demo ────────────────────────────────────────────────

struct PInner {
  value : UInt
  label : String
}

impl Packable for PInner with spec() {
  T::object("Inner", [F::u32_("value"), F::string("label")])
}

impl Packable for PInner with to_value(self) {
  V::obj([V::u32_(self.value), V::str(self.label)])
}

test "Packable/Inner" {
  let inner : PInner = { value: 42U, label: "hello" }
  let data = fracpack_pack(inner)
  let expected = hex_to_bytes("08002A000000040000000500000068656C6C6F")
  assert_eq!(data.length(), expected.length())
  for i = 0; i < expected.length(); i = i + 1 {
    assert_eq!(data[i], expected[i])
  }
}

// ── 21. Tuple tests ──────────────────────────────────────────────────────

// Helper: pack a tuple and compare to expected hex
fn expect_tuple_pack(spec : TypeSpec, val : Value, expected_hex : String) -> Unit!Error {
  let expected = hex_to_bytes(expected_hex)
  let actual = pack_tuple(spec, val)
  assert_eq!(actual.length(), expected.length())
  for i = 0; i < expected.length(); i = i + 1 {
    assert_eq!(actual[i], expected[i])
  }
}

test "Tuple/empty" {
  let s = T::tuple("Empty", [])
  expect_tuple_pack!(s, V::tuple_([]), "0000")
}

test "Tuple/u32_u32" {
  let s = T::tuple("Pair", [F::u32_("0"), F::u32_("1")])
  expect_tuple_pack!(s, V::tuple_([V::u32_(1U), V::u32_(2U)]), "08000100000002000000")
}

test "Tuple/u32_string" {
  let s = T::tuple("T", [F::u32_("0"), F::string("1")])
  expect_tuple_pack!(
    s,
    V::tuple_([V::u32_(42U), V::str("hello")]),
    "08002A000000040000000500000068656C6C6F",
  )
}

test "Tuple/trailing_elision" {
  // (u32, String, Option<u32>) with trailing None elided
  let s = T::tuple("T", [F::u32_("0"), F::string("1"), F::opt("2", U32)])
  expect_tuple_pack!(
    s,
    V::tuple_([V::u32_(1U), V::str("hi"), V::opt_none()]),
    "08000100000004000000020000006869",
  )
}

test "Tuple/opt_present" {
  // (u32, String, Option<u32>) with Some(99)
  let s = T::tuple("T", [F::u32_("0"), F::string("1"), F::opt("2", U32)])
  expect_tuple_pack!(
    s,
    V::tuple_([V::u32_(1U), V::str("hi"), V::opt(V::u32_(99U))]),
    "0C0001000000080000000A00000002000000686963000000",
  )
}

test "Tuple/nested" {
  // (u32, (u32, String))
  let inner = T::tuple("Inner", [F::u32_("0"), F::string("1")])
  let outer = T::tuple("Outer", [F::u32_("0"), F::tuple_("1", inner)])
  expect_tuple_pack!(
    outer,
    V::tuple_([V::u32_(10U), V::tuple_([V::u32_(20U), V::str("nested")])]),
    "08000A0000000400000008001400000004000000060000006E6573746564",
  )
}

test "Tuple/as_struct_field" {
  // A struct containing a tuple field
  let tup = T::tuple("T", [F::u32_("0"), F::u32_("1")])
  let s = T::object("S", [F::u32_("id"), F::tuple_("pair", tup)])
  let val = V::obj([V::u32_(1U), V::tuple_([V::u32_(10U), V::u32_(20U)])])
  expect_pack!(s, val, "0800010000000400000008000A00000014000000")
}

// ── 22. MutView tests ────────────────────────────────────────────────────

fn bytes_equal(a : Array[Byte], b : Array[Byte]) -> Bool {
  if a.length() != b.length() {
    return false
  }
  for i = 0; i < a.length(); i = i + 1 {
    if a[i] != b[i] {
      return false
    }
  }
  true
}

fn to_hex(data : Array[Byte]) -> String {
  let hex_chars = "0123456789ABCDEF"
  let mut result = ""
  for b in data {
    let hi = b.to_int().lsr(4).land(0xF)
    let lo = b.to_int().land(0xF)
    result = result + hex_chars[hi].to_string() + hex_chars[lo].to_string()
  }
  result
}

test "MutView/read_u32" {
  let s = T::object("S", [F::u32_("x"), F::u32_("y")])
  let data = pack(s, V::obj([V::u32_(42U), V::u32_(100U)]))
  let mv = MutView::new(s, data)
  assert_eq!(mv.get_u32("x"), 42U)
  assert_eq!(mv.get_u32("y"), 100U)
}

test "MutView/write_u32" {
  let s = T::object("S", [F::u32_("x"), F::u32_("y")])
  let data = pack(s, V::obj([V::u32_(42U), V::u32_(100U)]))
  let mv = MutView::new(s, data)
  mv.set_u32("x", 99U)
  assert_eq!(mv.get_u32("x"), 99U)
  assert_eq!(mv.get_u32("y"), 100U)
  // Verify canonical output
  let expected = pack(s, V::obj([V::u32_(99U), V::u32_(100U)]))
  assert_true!(bytes_equal(mv.to_bytes(), expected))
}

test "MutView/write_bool" {
  let s = T::object("S", [F::bool_("flag"), F::u32_("val")])
  let data = pack(s, V::obj([V::bool_(false), V::u32_(1U)]))
  let mv = MutView::new(s, data)
  assert_eq!(mv.get_bool("flag"), false)
  mv.set_bool("flag", true)
  assert_eq!(mv.get_bool("flag"), true)
}

test "MutView/write_string_canonical" {
  let s = T::object("S", [F::u32_("id"), F::string("name")])
  let data = pack(s, V::obj([V::u32_(1U), V::str("hello")]))
  let mv = MutView::new(s, data)
  // Read original
  let orig_bytes = mv.get_string("name")
  assert_eq!(orig_bytes.length(), 5)
  // Write a shorter string
  mv.set_string("name", "hi")
  let expected = pack(s, V::obj([V::u32_(1U), V::str("hi")]))
  assert_true!(bytes_equal(mv.to_bytes(), expected))
}

test "MutView/write_string_grow_canonical" {
  let s = T::object("S", [F::u32_("id"), F::string("name")])
  let data = pack(s, V::obj([V::u32_(1U), V::str("hi")]))
  let mv = MutView::new(s, data)
  // Write a longer string
  mv.set_string("name", "hello world")
  let expected = pack(s, V::obj([V::u32_(1U), V::str("hello world")]))
  assert_true!(bytes_equal(mv.to_bytes(), expected))
}

test "MutView/write_string_empty_canonical" {
  let s = T::object("S", [F::u32_("id"), F::string("name")])
  let data = pack(s, V::obj([V::u32_(1U), V::str("hello")]))
  let mv = MutView::new(s, data)
  // Set string to empty
  mv.set_string("name", "")
  let expected = pack(s, V::obj([V::u32_(1U), V::str("")]))
  assert_true!(bytes_equal(mv.to_bytes(), expected))
}

test "MutView/write_string_from_empty_canonical" {
  let s = T::object("S", [F::u32_("id"), F::string("name")])
  let data = pack(s, V::obj([V::u32_(1U), V::str("")]))
  let mv = MutView::new(s, data)
  // Set string from empty to non-empty
  mv.set_string("name", "world")
  let expected = pack(s, V::obj([V::u32_(1U), V::str("world")]))
  assert_true!(bytes_equal(mv.to_bytes(), expected))
}

test "MutView/sibling_offset_patching" {
  // Two variable-size fields: changing one should patch the other
  let s = T::object("S", [F::string("first"), F::string("second")])
  let data = pack(s, V::obj([V::str("aaa"), V::str("bbb")]))
  let mv = MutView::new(s, data)
  // Grow first field
  mv.set_string("first", "aaaaa")
  // Check second is still readable
  let second = mv.get_string("second")
  assert_eq!(second.length(), 3)
  // Verify canonical
  let expected = pack(s, V::obj([V::str("aaaaa"), V::str("bbb")]))
  assert_true!(bytes_equal(mv.to_bytes(), expected))
}

test "MutView/set_none" {
  let s = T::object("S", [F::u32_("id"), F::opt("val", U32)])
  let data = pack(s, V::obj([V::u32_(1U), V::opt(V::u32_(42U))]))
  let mv = MutView::new(s, data)
  // Verify we can read the optional
  let opt_val = mv.get_opt_u32("val")
  assert_eq!(opt_val, Some(42U))
  // Set to None
  mv.set_none("val")
  let opt_val2 = mv.get_opt_u32("val")
  assert_eq!(opt_val2, None)
}

test "MutView/set_opt_u32" {
  let s = T::object("S", [F::u32_("id"), F::opt("val", U32)])
  let data = pack(s, V::obj([V::u32_(1U), V::opt_none()]))
  let mv = MutView::new(s, data)
  // Set from None to Some
  mv.set_opt_u32("val", Some(99U))
  let opt_val = mv.get_opt_u32("val")
  assert_eq!(opt_val, Some(99U))
}

test "MutView/fast_mode_string" {
  let s = T::object("S", [F::u32_("id"), F::string("name")])
  let data = pack(s, V::obj([V::u32_(1U), V::str("hello")]))
  let mv = MutView::new_fast(s, data)
  // Write a shorter string (fits in place)
  mv.set_string("name", "hi")
  // In fast mode, the buffer may not be canonical
  // but reading should still work
  let name_bytes = mv.get_string("name")
  assert_eq!(name_bytes.length(), 2)
  // Compact to get canonical form
  mv.compact()
  let expected = pack(s, V::obj([V::u32_(1U), V::str("hi")]))
  assert_true!(bytes_equal(mv.to_bytes(), expected))
}

test "MutView/fast_mode_grow" {
  let s = T::object("S", [F::u32_("id"), F::string("name")])
  let data = pack(s, V::obj([V::u32_(1U), V::str("hi")]))
  let mv = MutView::new_fast(s, data)
  // Write a longer string (append to end)
  mv.set_string("name", "hello world")
  // Reading should still work via offset following
  let name_bytes = mv.get_string("name")
  assert_eq!(name_bytes.length(), 11)
  // Compact to get canonical form
  mv.compact()
  let expected = pack(s, V::obj([V::u32_(1U), V::str("hello world")]))
  assert_true!(bytes_equal(mv.to_bytes(), expected))
}

test "splice_buffer/grow" {
  let data : Array[Byte] = [b'\x01', b'\x02', b'\x03', b'\x04']
  let delta = splice_buffer(data, 1, 1, [b'\xAA', b'\xBB', b'\xCC'])
  assert_eq!(delta, 2)
  assert_eq!(data.length(), 6)
  assert_eq!(data[0], b'\x01')
  assert_eq!(data[1], b'\xAA')
  assert_eq!(data[2], b'\xBB')
  assert_eq!(data[3], b'\xCC')
  assert_eq!(data[4], b'\x03')
  assert_eq!(data[5], b'\x04')
}

test "splice_buffer/shrink" {
  let data : Array[Byte] = [b'\x01', b'\x02', b'\x03', b'\x04', b'\x05']
  let delta = splice_buffer(data, 1, 3, [b'\xAA'])
  assert_eq!(delta, -2)
  assert_eq!(data.length(), 3)
  assert_eq!(data[0], b'\x01')
  assert_eq!(data[1], b'\xAA')
  assert_eq!(data[2], b'\x05')
}

test "splice_buffer/same_size" {
  let data : Array[Byte] = [b'\x01', b'\x02', b'\x03']
  let delta = splice_buffer(data, 1, 1, [b'\xAA'])
  assert_eq!(delta, 0)
  assert_eq!(data.length(), 3)
  assert_eq!(data[0], b'\x01')
  assert_eq!(data[1], b'\xAA')
  assert_eq!(data[2], b'\x03')
}

test "MutView/multiple_edits_canonical" {
  let s = T::object("S", [F::u32_("id"), F::string("first"), F::string("second")])
  let data = pack(s, V::obj([V::u32_(1U), V::str("aaa"), V::str("bbb")]))
  let mv = MutView::new(s, data)
  // Edit both strings
  mv.set_string("first", "longer_first")
  mv.set_string("second", "x")
  mv.set_u32("id", 42U)
  let expected = pack(s, V::obj([V::u32_(42U), V::str("longer_first"), V::str("x")]))
  assert_true!(bytes_equal(mv.to_bytes(), expected))
}

// ── 23. Validation tests ──────────────────────────────────────────────

fn expect_valid(spec : TypeSpec, data : Array[Byte]) -> Unit {
  let result = validate(spec, data)
  if result.status != Valid {
    abort(
      "Expected Valid but got " +
      result.status.to_string() +
      ": " +
      result.message,
    )
  }
}

fn expect_invalid(spec : TypeSpec, data : Array[Byte]) -> Unit {
  let result = validate(spec, data)
  if result.status != Invalid {
    abort(
      "Expected Invalid but got " +
      result.status.to_string() +
      ": " +
      result.message,
    )
  }
}

fn expect_extended(spec : TypeSpec, data : Array[Byte]) -> Unit {
  let result = validate(spec, data)
  if result.status != Extended {
    abort(
      "Expected Extended but got " +
      result.status.to_string() +
      ": " +
      result.message,
    )
  }
}

// ── Valid data (golden vectors) ───────────────────────────────────────

test "validate/fixed_struct_valid" {
  let s = T::fixed("P", [F::i32_("x"), F::i32_("y")])
  let data = pack(s, V::obj([V::i32_(42), V::i32_(100)]))
  expect_valid(s, data)
}

test "validate/extensible_struct_valid" {
  let s = T::object("S", [F::u32_("id"), F::string("name")])
  let data = pack(s, V::obj([V::u32_(1U), V::str("hello")]))
  expect_valid(s, data)
}

test "validate/all_primitives_valid" {
  let s = all_prim_spec()
  let data = pack(
    s,
    V::obj([
      V::bool_(true), V::u8_(b'\xFF'), V::i8_(127), V::u16_(65535),
      V::i16_(32767), V::u32_(4294967295U), V::i32_(2147483647),
      V::u64_(18446744073709551615UL), V::i64_(9223372036854775807L),
      V::f32_(3.141590118408203), V::f64_(2.718281828459045),
    ]),
  )
  expect_valid(s, data)
}

test "validate/with_strings_valid" {
  let s = T::object(
    "WS",
    [F::string("a"), F::string("b"), F::string("c")],
  )
  let data = pack(s, V::obj([V::str(""), V::str("hello"), V::str("world")]))
  expect_valid(s, data)
}

test "validate/with_vectors_valid" {
  let s = T::object("WV", [F::vec("ints", U32), F::vec("strings", String_)])
  let data = pack(
    s,
    V::obj([
      V::vec([V::u32_(10U), V::u32_(20U)]),
      V::vec([V::str("hello"), V::str("world")]),
    ]),
  )
  expect_valid(s, data)
}

test "validate/with_optionals_valid" {
  let s = T::object("WO", [F::opt("a", U32), F::opt("b", String_)])
  let data = pack(
    s,
    V::obj([V::opt(V::u32_(42U)), V::opt(V::str("hi"))]),
  )
  expect_valid(s, data)
}

test "validate/optionals_none_valid" {
  let s = T::object("WO", [F::opt("a", U32), F::opt("b", String_)])
  let data = pack(s, V::obj([V::opt_none(), V::opt_none()]))
  expect_valid(s, data)
}

test "validate/nested_struct_valid" {
  let inner = T::object("Inner", [F::u32_("value"), F::string("label")])
  let outer = T::object(
    "Outer",
    [F::object("inner", inner), F::string("name")],
  )
  let data = pack(
    outer,
    V::obj([V::obj([V::u32_(1U), V::str("inner")]), V::str("outer")]),
  )
  expect_valid(outer, data)
}

test "validate/variant_valid" {
  let var_def = T::variant(
    "DV",
    [F::u32_("uint32"), F::string("string"), F::object("Inner", inner_spec())],
  )
  let s = T::object("WV", [F::variant("data", var_def)])
  let data = pack(s, V::obj([V::variant(1, V::str("hello"))]))
  expect_valid(s, data)
}

test "validate/tuple_valid" {
  let tup = T::tuple("T", [F::u32_("0"), F::string("1")])
  let s = T::object("S", [F::u32_("id"), F::tuple_("pair", tup)])
  let data = pack(
    s,
    V::obj([V::u32_(1U), V::tuple_([V::u32_(42U), V::str("hello")])]),
  )
  expect_valid(s, data)
}

test "validate/vec_of_structs_valid" {
  let s = T::object("VOS", [F::vec_obj("items", inner_spec())])
  let data = pack(
    s,
    V::obj([
      V::vec([
        V::obj([V::u32_(1U), V::str("one")]),
        V::obj([V::u32_(2U), V::str("two")]),
      ]),
    ]),
  )
  expect_valid(s, data)
}

test "validate/empty_extensible_valid" {
  let s = T::object("EE", [F::u32_("dummy")])
  let data = pack(s, V::obj([V::u32_(0U)]))
  expect_valid(s, data)
}

test "validate/trailing_elision_valid" {
  let s = T::object(
    "S",
    [F::u32_("id"), F::opt("a", U32), F::opt("b", String_)],
  )
  // Trailing None optionals are elided
  let data = pack(
    s,
    V::obj([V::u32_(1U), V::opt_none(), V::opt_none()]),
  )
  expect_valid(s, data)
}

test "validate/unicode_string_valid" {
  let s = T::object("SS", [F::string("value")])
  let data = pack(
    s,
    V::obj([V::str("caf\u{00E9} \u{2615} \u{65E5}\u{672C}\u{8A9E}")]),
  )
  expect_valid(s, data)
}

test "validate/complex_all_populated_valid" {
  let s = complex_spec()
  let data = pack(
    s,
    V::obj([
      V::vec([
        V::obj([V::u32_(1U), V::str("a")]),
        V::obj([V::u32_(2U), V::str("b")]),
      ]), V::opt(V::vec([V::u32_(10U), V::u32_(20U)])),
      V::vec([V::opt(V::str("x")), V::opt_none(), V::opt(V::str("z"))]),
      V::opt(V::obj([V::u32_(99U), V::str("present")])),
    ]),
  )
  expect_valid(s, data)
}

// ── Truncated data -> Invalid ─────────────────────────────────────────

test "validate/truncated_fixed_struct" {
  let s = T::fixed("P", [F::i32_("x"), F::i32_("y")])
  // Only 4 bytes, needs 8
  let data : Array[Byte] = [b'\x01', b'\x00', b'\x00', b'\x00']
  expect_invalid(s, data)
}

test "validate/truncated_extensible_struct" {
  let s = T::object("S", [F::u32_("id"), F::string("name")])
  // Only the u16 header, no fields
  let data : Array[Byte] = [b'\x08']
  expect_invalid(s, data)
}

test "validate/truncated_string_length" {
  let s = T::object("SS", [F::string("value")])
  // Extensible header says 4 bytes, offset points to heap, but string
  // data is truncated
  let data = hex_to_bytes("0400040000000500000068656C") // "hello" truncated
  expect_invalid(s, data)
}

test "validate/empty_buffer" {
  let s = T::object("S", [F::u32_("id")])
  let data : Array[Byte] = []
  expect_invalid(s, data)
}

// ── Extra trailing bytes -> Invalid ───────────────────────────────────

test "validate/extra_trailing_bytes_fixed" {
  let s = T::fixed("P", [F::i32_("x"), F::i32_("y")])
  // 8 bytes of data + 2 extra bytes
  let data = hex_to_bytes("00000000000000001234")
  expect_invalid(s, data)
}

test "validate/extra_trailing_bytes_extensible" {
  let s = T::object("S", [F::u32_("id")])
  let data = pack(s, V::obj([V::u32_(42U)]))
  data.push(b'\xFF') // extra byte
  expect_invalid(s, data)
}

// ── Bad offset -> Invalid ─────────────────────────────────────────────

test "validate/bad_offset_out_of_bounds" {
  let s = T::object("SS", [F::string("value")])
  // Extensible struct with u16 fixed_size=4, offset pointing way out
  let data = hex_to_bytes("0400FF000000")
  expect_invalid(s, data)
}

test "validate/reserved_offset_2" {
  let s = T::object("SS", [F::string("value")])
  // Extensible struct with u16 fixed_size=4, offset=2 (reserved)
  let data = hex_to_bytes("040002000000")
  expect_invalid(s, data)
}

test "validate/reserved_offset_3" {
  let s = T::object("SS", [F::string("value")])
  // Extensible struct with u16 fixed_size=4, offset=3 (reserved)
  let data = hex_to_bytes("040003000000")
  expect_invalid(s, data)
}

// ── Bad UTF-8 -> Invalid ──────────────────────────────────────────────

test "validate/bad_utf8_invalid_lead" {
  let s = T::object("SS", [F::string("value")])
  // Pack a valid string, then corrupt the UTF-8 data
  // Header: u16 fixed_size=4, offset=4, string len=1, byte=0xFF (invalid lead)
  let data = hex_to_bytes("040004000000010000" + "00FF")
  // Actually let me construct this more carefully:
  // u16 fixed_size = 4: 0400
  // u32 offset = 4: 04000000
  // At offset (pos 2 + 4 = 6): u32 string_len = 1: 01000000
  // string byte: FF (invalid lead byte)
  let data2 = hex_to_bytes("0400040000000100000" + "0FF")
  // Wait, need to be careful. Let me use proper pack then corrupt.
  let good = pack(
    s,
    V::obj([V::str("A")]),
  )
  // The packed data for "A": 04 00 04 00 00 00 01 00 00 00 41
  // Corrupt the last byte (the 'A' = 0x41) to 0xFF
  good[good.length() - 1] = b'\xFF'
  expect_invalid(s, good)
}

test "validate/bad_utf8_truncated_sequence" {
  let s = T::object("SS", [F::string("value")])
  // Pack a valid 2-byte UTF-8 string then corrupt it
  let good = pack(s, V::obj([V::str("\u{00E9}")]))
  // The UTF-8 for U+00E9 is C3 A9 (2 bytes)
  // Truncate the continuation byte by setting string length to 1
  // but keeping the C3 lead byte
  // Find string length field and set to 1
  // Format: [u16 fixed=4][u32 offset=4][u32 strlen=2][C3 A9]
  // Change strlen from 2 to 1, remove last byte
  let data : Array[Byte] = []
  for i = 0; i < good.length() - 1; i = i + 1 {
    data.push(good[i])
  }
  // Fix the string length to 1 (at position 6)
  data[6] = b'\x01'
  data[7] = b'\x00'
  data[8] = b'\x00'
  data[9] = b'\x00'
  expect_invalid(s, data)
}

test "validate/bad_utf8_continuation" {
  let s = T::object("SS", [F::string("value")])
  // Build: header=04 00, offset=04 00 00 00, strlen=02 00 00 00, C3 00
  // C3 is a 2-byte lead, but 0x00 is not a valid continuation
  let data = hex_to_bytes("04000400000002000000C300")
  expect_invalid(s, data)
}

// ── Bad bool -> Invalid ───────────────────────────────────────────────

test "validate/bad_bool_value_2" {
  let s = T::fixed("SB", [F::bool_("value")])
  let data : Array[Byte] = [b'\x02']
  expect_invalid(s, data)
}

test "validate/bad_bool_value_255" {
  let s = T::fixed("SB", [F::bool_("value")])
  let data : Array[Byte] = [b'\xFF']
  expect_invalid(s, data)
}

test "validate/good_bool_false" {
  let s = T::fixed("SB", [F::bool_("value")])
  let data : Array[Byte] = [b'\x00']
  expect_valid(s, data)
}

test "validate/good_bool_true" {
  let s = T::fixed("SB", [F::bool_("value")])
  let data : Array[Byte] = [b'\x01']
  expect_valid(s, data)
}

// ── Bad variant tag -> Invalid ────────────────────────────────────────

test "validate/bad_variant_tag" {
  let var_def = T::variant("DV", [F::u32_("uint32"), F::string("string")])
  let s = T::object("WV", [F::variant("data", var_def)])
  // Pack a valid variant, then corrupt the tag
  let data = pack(s, V::obj([V::variant(0, V::u32_(42U))]))
  // Variant data starts at the heap; find the tag byte
  // Format: [u16 fixed=4][u32 offset to variant]
  // At heap: [u8 tag][u32 data_size][data]
  // The offset is at position 2, read it
  let off = data[2].to_int()
    | (data[3].to_int() << 8)
    | (data[4].to_int() << 16)
    | (data[5].to_int() << 24)
  let tag_pos = 2 + off // offset is relative to offset field pos
  data[tag_pos] = b'\x05' // tag=5, but only 2 cases
  expect_invalid(s, data)
}

// ── Extended struct -> Extended ───────────────────────────────────────

test "validate/extended_struct" {
  // Create data that has a larger fixed_size than our spec knows
  let s = T::object("S", [F::u32_("id")])
  // Pack with the known spec
  let data = pack(s, V::obj([V::u32_(42U)]))
  // data should be: 04 00 2A 00 00 00
  // Modify: expand fixed_size to 8 (add 4 unknown bytes)
  // New data: 08 00 2A 00 00 00 00 00 00 00
  let extended_data : Array[Byte] = [
    b'\x08', b'\x00', // fixed_size = 8
    b'\x2A', b'\x00', b'\x00', b'\x00', // id = 42
    b'\x00', b'\x00', b'\x00', b'\x00', // unknown extra field
  ]
  expect_extended(s, extended_data)
}

test "validate/extended_struct_with_string" {
  // Spec knows id + name, but data has an extra field
  let s = T::object("S", [F::u32_("id")])
  // Build manually: fixed_size=8 (4 known + 4 unknown), id=1, extra=0
  let data : Array[Byte] = [
    b'\x08', b'\x00', // fixed_size = 8
    b'\x01', b'\x00', b'\x00', b'\x00', // id = 1
    b'\x00', b'\x00', b'\x00', b'\x00', // unknown field (offset=0, empty)
  ]
  expect_extended(s, data)
}

// ── Vec size alignment -> Invalid ─────────────────────────────────────

test "validate/vec_bad_alignment" {
  let s = T::object("WV", [F::vec("ints", U32)])
  // Pack a valid vec, then corrupt the data_size
  let data = pack(s, V::obj([V::vec([V::u32_(1U), V::u32_(2U)])]))
  // The vec is on the heap. Find it via offset.
  // Format: [u16 fixed=4][u32 offset][u32 data_size][data...]
  let off = data[2].to_int()
    | (data[3].to_int() << 8)
    | (data[4].to_int() << 16)
    | (data[5].to_int() << 24)
  let vec_pos = 2 + off
  // data_size is at vec_pos, currently 08 00 00 00 (8 = 2*4)
  // Set to 7 (not divisible by 4)
  data[vec_pos] = b'\x07'
  expect_invalid(s, data)
}

// ── Golden vector validation (pack then validate) ─────────────────────

test "validate/golden_fixed_ints" {
  let s = T::fixed("FI", [F::i32_("x"), F::i32_("y")])
  expect_valid(s, hex_to_bytes("2A00000064000000"))
  expect_valid(s, hex_to_bytes("FFFFFFFF00000080"))
  expect_valid(s, hex_to_bytes("FFFFFF7FFFFFFF7F"))
}

test "validate/golden_single_string" {
  let s = T::object("SS", [F::string("value")])
  expect_valid(s, hex_to_bytes("040000000000")) // empty
  expect_valid(s, hex_to_bytes("0400040000000500000068656C6C6F")) // "hello"
}

test "validate/golden_with_vectors" {
  let s = T::object("WV", [F::vec("ints", U32), F::vec("strings", String_)])
  expect_valid(s, hex_to_bytes("08000000000000000000")) // both empty
  expect_valid(
    s,
    hex_to_bytes(
      "080008000000000000000C000000010000000200000003000000",
    ),
  ) // ints only
}

test "validate/golden_with_optionals" {
  let s = T::object("WO", [F::opt("a", U32), F::opt("b", String_)])
  expect_valid(s, hex_to_bytes("0000")) // both None
  expect_valid(s, hex_to_bytes("0400040000002A000000")) // int only
  expect_valid(
    s,
    hex_to_bytes("080001000000040000000500000068656C6C6F"),
  ) // str only
}

test "validate/golden_variant" {
  let var_def = T::variant(
    "DV",
    [
      F::u32_("uint32"), F::string("string"),
      F::object("Inner", inner_spec()),
    ],
  )
  let s = T::object("WV", [F::variant("data", var_def)])
  expect_valid(s, hex_to_bytes("04000400000000040000002A000000")) // u32 alt
  expect_valid(
    s,
    hex_to_bytes("04000400000001090000000500000068656C6C6F"),
  ) // string alt
}

test "validate/golden_tuple" {
  let tup = T::tuple("T", [F::u32_("0"), F::u32_("1")])
  let s = T::object("S", [F::u32_("id"), F::tuple_("pair", tup)])
  expect_valid(
    s,
    hex_to_bytes("0800010000000400000008000A00000014000000"),
  )
}

test "validate/golden_complex_all_empty" {
  let s = complex_spec()
  expect_valid(s, hex_to_bytes("0C00000000000100000000000000"))
}

// ── Backward offset -> Invalid ────────────────────────────────────────

test "validate/backward_offset" {
  // Two string fields: make the second field's offset point before the
  // first field's heap data (backward offset).
  let s = T::object("S", [F::string("a"), F::string("b")])
  let data = pack(s, V::obj([V::str("hello"), V::str("world")]))
  // data layout:
  //   [u16 fixed=8][u32 off_a][u32 off_b][str_a data][str_b data]
  // Corrupt off_b to point before str_b (e.g., to str_a's location)
  // off_b is at pos 6. off_a target = 2+off_a. We want off_b target < off_a end.
  // Set off_b to a small value that points into str_a's region.
  let off_a = data[2].to_int()
    | (data[3].to_int() << 8)
    | (data[4].to_int() << 16)
    | (data[5].to_int() << 24)
  // Make off_b point to same place as off_a (overlapping)
  // off_b at pos 6; target should be 6 + val = 2 + off_a
  // val = 2 + off_a - 6 = off_a - 4
  let bad_off = off_a - 4
  data[6] = bad_off.land(0xFF).to_byte()
  data[7] = bad_off.lsr(8).land(0xFF).to_byte()
  data[8] = bad_off.lsr(16).land(0xFF).to_byte()
  data[9] = bad_off.lsr(24).land(0xFF).to_byte()
  expect_invalid(s, data)
}

// ── Pack-then-validate round-trip for all golden cases ────────────────

test "validate/roundtrip_vec_of_optionals" {
  let s = T::object("VOO", [F::vec("items", Optional)])
  let data = pack(
    s,
    V::obj([
      V::vec([
        V::opt(V::u32_(1U)), V::opt_none(), V::opt(V::u32_(3U)),
        V::opt_none(),
      ]),
    ]),
  )
  expect_valid(s, data)
}

test "validate/roundtrip_optional_struct_present" {
  let s = T::object("OS", [F::opt_obj("item", inner_spec())])
  let data = pack(
    s,
    V::obj([V::opt(V::obj([V::u32_(42U), V::str("exists")]))]),
  )
  expect_valid(s, data)
}

test "validate/roundtrip_optional_struct_null" {
  let s = T::object("OS", [F::opt_obj("item", inner_spec())])
  let data = pack(s, V::obj([V::opt_none()]))
  expect_valid(s, data)
}

test "validate/roundtrip_nested_vecs" {
  let s = T::object("NV", [F::vec("matrix", Vec)])
  let data = pack(
    s,
    V::obj([
      V::vec([
        V::vec([V::u32_(1U), V::u32_(0U)]),
        V::vec([V::u32_(0U), V::u32_(1U)]),
      ]),
    ]),
  )
  expect_valid(s, data)
}

test "validate/roundtrip_all_primitives_min_signed" {
  let s = all_prim_spec()
  let data = pack(
    s,
    V::obj([
      V::bool_(false), V::u8_(b'\x00'), V::i8_(-128), V::u16_(0),
      V::i16_(-32768), V::u32_(0U), V::i32_(-2147483648), V::u64_(0UL),
      V::i64_(-9223372036854775808L), V::f32_(-1.0), V::f64_(-1.0),
    ]),
  )
  expect_valid(s, data)
}

// ── JSON tests ────────────────────────────────────────────────────────

/// Helper: verify to_json produces the expected string.
fn expect_json(spec : TypeSpec, val : Value, expected : String) -> Unit!Error {
  let actual = to_json(spec, val)
  assert_eq!(actual, expected)
}

/// Helper: round-trip value → JSON → value → JSON, check strings match.
fn expect_json_roundtrip(spec : TypeSpec, val : Value) -> Unit!Error {
  let json1 = to_json(spec, val)
  let val2 = from_json!(spec, json1)
  let json2 = to_json(spec, val2)
  assert_eq!(json1, json2)
}

// ── JSON/to_json basic types ──────────────────────────────────────────

test "JSON/bool_true" {
  let s = T::fixed("SB", [F::bool_("value")])
  expect_json!(s, V::obj([V::bool_(true)]), "{\"value\":true}")
}

test "JSON/bool_false" {
  let s = T::fixed("SB", [F::bool_("value")])
  expect_json!(s, V::obj([V::bool_(false)]), "{\"value\":false}")
}

test "JSON/u32" {
  let s = T::fixed("SU", [F::u32_("value")])
  expect_json!(s, V::obj([V::u32_(42U)]), "{\"value\":42}")
}

test "JSON/i32_negative" {
  let s = T::fixed("SI", [F::i32_("value")])
  expect_json!(s, V::obj([V::i32_(-123)]), "{\"value\":-123}")
}

test "JSON/u64_as_string" {
  // u64 must be serialized as a JSON string for JS interop
  let s = T::fixed("S", [F::u64_("value")])
  expect_json!(s, V::obj([V::u64_(12345678901234UL)]), "{\"value\":\"12345678901234\"}")
}

test "JSON/i64_as_string" {
  // i64 must be serialized as a JSON string
  let s = T::fixed("S", [F::i64_("value")])
  expect_json!(s, V::obj([V::i64_(-9876543210L)]), "{\"value\":\"-9876543210\"}")
}

test "JSON/u8" {
  let s = T::fixed("S", [F::u8_("value")])
  expect_json!(s, V::obj([V::u8_(b'\xFF')]), "{\"value\":255}")
}

test "JSON/i8" {
  let s = T::fixed("S", [F::i8_("value")])
  expect_json!(s, V::obj([V::i8_(-128)]), "{\"value\":-128}")
}

test "JSON/u16" {
  let s = T::fixed("S", [F::u16_("value")])
  expect_json!(s, V::obj([V::u16_(65535)]), "{\"value\":65535}")
}

test "JSON/i16" {
  let s = T::fixed("S", [F::i16_("value")])
  expect_json!(s, V::obj([V::i16_(-32768)]), "{\"value\":-32768}")
}

// ── JSON/string escaping ──────────────────────────────────────────────

test "JSON/string_simple" {
  let s = T::object("SS", [F::string("value")])
  expect_json!(s, V::obj([V::str("hello")]), "{\"value\":\"hello\"}")
}

test "JSON/string_empty" {
  let s = T::object("SS", [F::string("value")])
  expect_json!(s, V::obj([V::str("")]), "{\"value\":\"\"}")
}

test "JSON/string_escapes" {
  let s = T::object("SS", [F::string("value")])
  expect_json!(s, V::obj([V::str("quote\"backslash\\")]), "{\"value\":\"quote\\\"backslash\\\\\"}")
}

test "JSON/string_control_chars" {
  let s = T::object("SS", [F::string("value")])
  expect_json!(s, V::obj([V::str("tab\there\nnewline")]), "{\"value\":\"tab\\there\\nnewline\"}")
}

// ── JSON/optional ─────────────────────────────────────────────────────

test "JSON/optional_none" {
  let s = T::object("WO", [F::opt("opt_int", U32)])
  expect_json!(s, V::obj([V::opt_none()]), "{\"opt_int\":null}")
}

test "JSON/optional_some" {
  let s = T::object("WO", [F::opt("opt_int", U32)])
  expect_json!(s, V::obj([V::opt(V::u32_(42U))]), "{\"opt_int\":42}")
}

test "JSON/optional_some_string" {
  let s = T::object("WO", [F::opt("opt_str", String_)])
  expect_json!(s, V::obj([V::opt(V::str("hello"))]), "{\"opt_str\":\"hello\"}")
}

// ── JSON/variant ──────────────────────────────────────────────────────

test "JSON/variant_u32" {
  let var_def = T::variant("DV", [F::u32_("uint32"), F::string("string")])
  let s = T::object("WV", [F::variant("data", var_def)])
  expect_json!(s, V::obj([V::variant(0, V::u32_(42U))]), "{\"data\":{\"uint32\":42}}")
}

test "JSON/variant_string" {
  let var_def = T::variant("DV", [F::u32_("uint32"), F::string("string")])
  let s = T::object("WV", [F::variant("data", var_def)])
  expect_json!(s, V::obj([V::variant(1, V::str("hello"))]), "{\"data\":{\"string\":\"hello\"}}")
}

test "JSON/variant_struct" {
  let var_def = T::variant("DV", [F::u32_("uint32"), F::string("string"), F::object("Inner", inner_spec())])
  let s = T::object("WV", [F::variant("data", var_def)])
  expect_json!(
    s,
    V::obj([V::variant(2, V::obj([V::u32_(7U), V::str("test")]))]),
    "{\"data\":{\"Inner\":{\"value\":7,\"label\":\"test\"}}}",
  )
}

// ── JSON/nested struct ────────────────────────────────────────────────

test "JSON/nested_struct" {
  let s = T::object("Outer", [F::object("inner", inner_spec()), F::string("name")])
  expect_json!(
    s,
    V::obj([V::obj([V::u32_(1U), V::str("inner")]), V::str("outer")]),
    "{\"inner\":{\"value\":1,\"label\":\"inner\"},\"name\":\"outer\"}",
  )
}

// ── JSON/vec ──────────────────────────────────────────────────────────

test "JSON/vec_u32" {
  let s = T::object("WV", [F::vec("ints", U32)])
  expect_json!(s, V::obj([V::vec([V::u32_(1U), V::u32_(2U), V::u32_(3U)])]), "{\"ints\":[1,2,3]}")
}

test "JSON/vec_empty" {
  let s = T::object("WV", [F::vec("ints", U32)])
  expect_json!(s, V::obj([V::vec([])]), "{\"ints\":[]}")
}

test "JSON/vec_strings" {
  let s = T::object("WV", [F::vec("strs", String_)])
  expect_json!(s, V::obj([V::vec([V::str("a"), V::str("b")])]), "{\"strs\":[\"a\",\"b\"]}")
}

// ── JSON/tuple ────────────────────────────────────────────────────────

test "JSON/tuple" {
  let s = T::tuple("T", [F::u32_("0"), F::string("1")])
  let val = V::tuple_([V::u32_(42U), V::str("hello")])
  let json = to_json(s, val)
  assert_eq!(json, "[42,\"hello\"]")
}

// ── JSON/floats ───────────────────────────────────────────────────────

test "JSON/f64_normal" {
  let s = T::fixed("S", [F::f64_("value")])
  expect_json!(s, V::obj([V::f64_(3.14)]), "{\"value\":3.14}")
}

test "JSON/f64_nan" {
  let s = T::fixed("S", [F::f64_("value")])
  expect_json!(s, V::obj([V::f64_(0.0 / 0.0)]), "{\"value\":null}")
}

test "JSON/f64_infinity" {
  let s = T::fixed("S", [F::f64_("value")])
  expect_json!(s, V::obj([V::f64_(1.0 / 0.0)]), "{\"value\":null}")
}

test "JSON/f32_nan" {
  let s = T::fixed("S", [F::f32_("value")])
  expect_json!(s, V::obj([V::f32_((0.0 / 0.0).to_float())]), "{\"value\":null}")
}

// ── JSON/from_json ────────────────────────────────────────────────────

test "JSON/from_json_u32" {
  let s = T::fixed("S", [F::u32_("value")])
  let val = from_json!(s, "{\"value\":42}")
  assert_eq!(to_json(s, val), "{\"value\":42}")
}

test "JSON/from_json_bool" {
  let s = T::fixed("S", [F::bool_("value")])
  let val = from_json!(s, "{\"value\":true}")
  assert_eq!(to_json(s, val), "{\"value\":true}")
}

test "JSON/from_json_string" {
  let s = T::object("SS", [F::string("value")])
  let val = from_json!(s, "{\"value\":\"hello world\"}")
  assert_eq!(to_json(s, val), "{\"value\":\"hello world\"}")
}

test "JSON/from_json_string_escapes" {
  let s = T::object("SS", [F::string("value")])
  let val = from_json!(s, "{\"value\":\"quote\\\"backslash\\\\tab\\tnewline\\n\"}")
  assert_eq!(to_json(s, val), "{\"value\":\"quote\\\"backslash\\\\tab\\tnewline\\n\"}")
}

test "JSON/from_json_u64_as_string" {
  let s = T::fixed("S", [F::u64_("value")])
  let val = from_json!(s, "{\"value\":\"18446744073709551615\"}")
  assert_eq!(to_json(s, val), "{\"value\":\"18446744073709551615\"}")
}

test "JSON/from_json_i64_as_string" {
  let s = T::fixed("S", [F::i64_("value")])
  let val = from_json!(s, "{\"value\":\"-9223372036854775808\"}")
  assert_eq!(to_json(s, val), "{\"value\":\"-9223372036854775808\"}")
}

test "JSON/from_json_optional_none" {
  let s = T::object("WO", [F::opt("opt_int", U32)])
  let val = from_json!(s, "{\"opt_int\":null}")
  assert_eq!(to_json(s, val), "{\"opt_int\":null}")
}

test "JSON/from_json_optional_some" {
  let s = T::object("WO", [F::opt("opt_int", U32)])
  let val = from_json!(s, "{\"opt_int\":99}")
  assert_eq!(to_json(s, val), "{\"opt_int\":99}")
}

test "JSON/from_json_variant" {
  let var_def = T::variant("DV", [F::u32_("uint32"), F::string("string")])
  let s = T::object("WV", [F::variant("data", var_def)])
  let val = from_json!(s, "{\"data\":{\"string\":\"hello\"}}")
  assert_eq!(to_json(s, val), "{\"data\":{\"string\":\"hello\"}}")
}

test "JSON/from_json_vec" {
  let s = T::object("WV", [F::vec("ints", U32)])
  let val = from_json!(s, "{\"ints\":[10,20,30]}")
  assert_eq!(to_json(s, val), "{\"ints\":[10,20,30]}")
}

test "JSON/from_json_nested_struct" {
  let s = T::object("Outer", [F::object("inner", inner_spec()), F::string("name")])
  let val = from_json!(s, "{\"inner\":{\"value\":42,\"label\":\"test\"},\"name\":\"outer\"}")
  assert_eq!(to_json(s, val), "{\"inner\":{\"value\":42,\"label\":\"test\"},\"name\":\"outer\"}")
}

// ── JSON/round-trip ───────────────────────────────────────────────────

test "JSON/roundtrip_all_primitives" {
  let s = all_prim_spec()
  let val = V::obj([
    V::bool_(true), V::u8_(b'\xFF'), V::i8_(-128), V::u16_(65535),
    V::i16_(-32768), V::u32_(4294967295U), V::i32_(-2147483648),
    V::u64_(18446744073709551615UL), V::i64_(-9223372036854775808L),
    V::f32_(3.14.to_float()), V::f64_(2.718281828459045),
  ])
  expect_json_roundtrip!(s, val)
}

test "JSON/roundtrip_nested_with_optionals" {
  let s = T::object("S", [
    F::u32_("id"),
    F::string("name"),
    F::opt("maybe", U32),
    F::opt("maybe_str", String_),
    F::object("nested", inner_spec()),
  ])
  let val = V::obj([
    V::u32_(1U),
    V::str("test"),
    V::opt(V::u32_(42U)),
    V::opt_none(),
    V::obj([V::u32_(7U), V::str("inner")]),
  ])
  expect_json_roundtrip!(s, val)
}

test "JSON/roundtrip_variant" {
  let var_def = T::variant("DV", [F::u32_("uint32"), F::string("string"), F::object("Inner", inner_spec())])
  let s = T::object("WV", [F::variant("data", var_def)])
  // Test each case
  expect_json_roundtrip!(s, V::obj([V::variant(0, V::u32_(42U))]))
  expect_json_roundtrip!(s, V::obj([V::variant(1, V::str("hello"))]))
  expect_json_roundtrip!(s, V::obj([V::variant(2, V::obj([V::u32_(7U), V::str("test")]))]))
}

test "JSON/roundtrip_tuple" {
  let s = T::tuple("T", [F::u32_("0"), F::string("1"), F::opt("2", U32)])
  expect_json_roundtrip!(s, V::tuple_([V::u32_(42U), V::str("hello"), V::opt(V::u32_(99U))]))
  expect_json_roundtrip!(s, V::tuple_([V::u32_(0U), V::str(""), V::opt_none()]))
}

test "JSON/roundtrip_vec_of_structs" {
  let s = T::object("VOS", [F::vec_obj("items", inner_spec())])
  let val = V::obj([V::vec([
    V::obj([V::u32_(1U), V::str("one")]),
    V::obj([V::u32_(2U), V::str("two")]),
  ])])
  expect_json_roundtrip!(s, val)
}
