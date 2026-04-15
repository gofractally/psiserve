package fracpack

import (
	"encoding/hex"
	"math"
	"strings"
	"testing"
)

// ── Type definitions ────────────────────────────────────────────────────

type TFixedInts struct {
	Fixed
	X int32 `fracpack:"x"`
	Y int32 `fracpack:"y"`
}

type TFixedMixed struct {
	Fixed
	B   bool   `fracpack:"b"`
	U8  uint8  `fracpack:"u8"`
	U16 uint16 `fracpack:"u16"`
	U32 uint32 `fracpack:"u32"`
	U64 uint64 `fracpack:"u64"`
}

type TAllPrimitives struct {
	B    bool    `fracpack:"b"`
	U8v  uint8   `fracpack:"u8v"`
	I8v  int8    `fracpack:"i8v"`
	U16v uint16  `fracpack:"u16v"`
	I16v int16   `fracpack:"i16v"`
	U32v uint32  `fracpack:"u32v"`
	I32v int32   `fracpack:"i32v"`
	U64v uint64  `fracpack:"u64v"`
	I64v int64   `fracpack:"i64v"`
	F32v float32 `fracpack:"f32v"`
	F64v float64 `fracpack:"f64v"`
}

type TSingleBool struct {
	Fixed
	Value bool `fracpack:"value"`
}

type TSingleU32 struct {
	Fixed
	Value uint32 `fracpack:"value"`
}

type TSingleString struct {
	Value string `fracpack:"value"`
}

type TWithStrings struct {
	EmptyStr string `fracpack:"empty_str"`
	Hello    string `fracpack:"hello"`
	Unicode  string `fracpack:"unicode"`
}

type TWithVectors struct {
	Ints    []uint32 `fracpack:"ints"`
	Strings []string `fracpack:"strings"`
}

type TWithOptionals struct {
	OptInt *uint32 `fracpack:"opt_int"`
	OptStr *string `fracpack:"opt_str"`
}

type TInner struct {
	Value uint32 `fracpack:"value"`
	Label string `fracpack:"label"`
}

type TOuter struct {
	Inner TInner `fracpack:"inner"`
	Name  string `fracpack:"name"`
}

type TDataVariant struct {
	Variant
	Uint32 *uint32 `fracpack:"uint32"`
	String *string `fracpack:"string"`
	Inner  *TInner `fracpack:"Inner"`
}

type TWithVariant struct {
	Data TDataVariant `fracpack:"data"`
}

type TVecOfStructs struct {
	Items []TInner `fracpack:"items"`
}

type TOptionalStruct struct {
	Item *TInner `fracpack:"item"`
}

type TVecOfOptionals struct {
	Items []*uint32 `fracpack:"items"`
}

type TOptionalVec struct {
	Items *[]uint32 `fracpack:"items"`
}

type TNestedVecs struct {
	Matrix [][]uint32 `fracpack:"matrix"`
}

type TFixedArray struct {
	Fixed
	Arr [3]uint32 `fracpack:"arr"`
}

type TComplex struct {
	Items     []TInner  `fracpack:"items"`
	OptVec    *[]uint32 `fracpack:"opt_vec"`
	VecOpt    []*string `fracpack:"vec_opt"`
	OptStruct *TInner   `fracpack:"opt_struct"`
}

type TEmptyExtensible struct {
	Dummy uint32 `fracpack:"dummy"`
}

// ── Helpers ─────────────────────────────────────────────────────────────

func expectMarshal(t *testing.T, name string, v any, expectedHex string) {
	t.Helper()
	data, err := Marshal(v)
	if err != nil {
		t.Fatalf("%s: marshal error: %v", name, err)
	}
	expected, _ := hex.DecodeString(expectedHex)
	if !bytesEqual(data, expected) {
		t.Errorf("%s: marshal mismatch\n  got:  %s\n  want: %s",
			name, strings.ToUpper(hex.EncodeToString(data)), expectedHex)
	}
}

func u32p(v uint32) *uint32 { return &v }
func strp(v string) *string { return &v }

// ── FixedInts (4 cases) ────────────────────────────────────────────────

func TestMarshalFixedInts(t *testing.T) {
	expectMarshal(t, "zeros", TFixedInts{X: 0, Y: 0}, "0000000000000000")
	expectMarshal(t, "positive", TFixedInts{X: 42, Y: 100}, "2A00000064000000")
	expectMarshal(t, "negative", TFixedInts{X: -1, Y: -2147483648}, "FFFFFFFF00000080")
	expectMarshal(t, "max", TFixedInts{X: 2147483647, Y: 2147483647}, "FFFFFF7FFFFFFF7F")
}

// ── FixedMixed (3 cases) ───────────────────────────────────────────────

func TestMarshalFixedMixed(t *testing.T) {
	expectMarshal(t, "zeros", TFixedMixed{}, "00000000000000000000000000000000")
	expectMarshal(t, "ones", TFixedMixed{B: true, U8: 1, U16: 1, U32: 1, U64: 1},
		"01010100010000000100000000000000")
	expectMarshal(t, "max", TFixedMixed{B: true, U8: 255, U16: 65535, U32: 4294967295, U64: 18446744073709551615},
		"01FFFFFFFFFFFFFFFFFFFFFFFFFFFFFF")
}

// ── AllPrimitives (5 cases) ─────────────────────────────────────────────

func TestMarshalAllPrimitives(t *testing.T) {
	expectMarshal(t, "zeros", TAllPrimitives{},
		"2B0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000")
	expectMarshal(t, "ones", TAllPrimitives{B: true, U8v: 1, I8v: 1, U16v: 1, I16v: 1, U32v: 1, I32v: 1, U64v: 1, I64v: 1, F32v: 1.0, F64v: 1.0},
		"2B00010101010001000100000001000000010000000000000001000000000000000000803F000000000000F03F")
	expectMarshal(t, "max_unsigned", TAllPrimitives{
		B: true, U8v: 255, I8v: 127, U16v: 65535, I16v: 32767,
		U32v: 4294967295, I32v: 2147483647,
		U64v: 18446744073709551615, I64v: 9223372036854775807,
		F32v: math.Float32frombits(0x40490FD0),
		F64v: math.Float64frombits(0x4005BF0A8B145769),
	}, "2B0001FF7FFFFFFF7FFFFFFFFFFFFFFF7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF7FD00F49406957148B0ABF0540")
	expectMarshal(t, "min_signed", TAllPrimitives{
		I8v: -128, I16v: -32768, I32v: -2147483648, I64v: -9223372036854775808,
		F32v: -1.0, F64v: -1.0,
	}, "2B0000008000000080000000000000008000000000000000000000000000000080000080BF000000000000F0BF")
	expectMarshal(t, "fractional", TAllPrimitives{
		F32v: math.Float32frombits(0x3DCCCCCD),
		F64v: math.Float64frombits(0x3FB999999999999A),
	}, "2B0000000000000000000000000000000000000000000000000000000000000000CDCCCC3D9A9999999999B93F")
}

// ── SingleBool (2 cases) ───────────────────────────────────────────────

func TestMarshalSingleBool(t *testing.T) {
	expectMarshal(t, "false", TSingleBool{Value: false}, "00")
	expectMarshal(t, "true", TSingleBool{Value: true}, "01")
}

// ── SingleU32 (4 cases) ────────────────────────────────────────────────

func TestMarshalSingleU32(t *testing.T) {
	expectMarshal(t, "zero", TSingleU32{Value: 0}, "00000000")
	expectMarshal(t, "one", TSingleU32{Value: 1}, "01000000")
	expectMarshal(t, "max", TSingleU32{Value: 4294967295}, "FFFFFFFF")
	expectMarshal(t, "hex", TSingleU32{Value: 3735928559}, "EFBEADDE")
}

// ── SingleString (6 cases) ─────────────────────────────────────────────

func TestMarshalSingleString(t *testing.T) {
	expectMarshal(t, "empty", TSingleString{Value: ""}, "040000000000")
	expectMarshal(t, "hello", TSingleString{Value: "hello"}, "0400040000000500000068656C6C6F")
	expectMarshal(t, "spaces", TSingleString{Value: "hello world"}, "0400040000000B00000068656C6C6F20776F726C64")
	expectMarshal(t, "special", TSingleString{Value: "tab\there\nnewline"}, "0400040000001000000074616209686572650A6E65776C696E65")
	expectMarshal(t, "unicode", TSingleString{Value: "caf\xc3\xa9 \xe2\x98\x95 \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"},
		"04000400000013000000636166C3A920E2989520E697A5E69CACE8AA9E")
	expectMarshal(t, "escapes", TSingleString{Value: "quote\"backslash\\"},
		"0400040000001000000071756F7465226261636B736C6173685C")
}

// ── WithStrings (2 cases) ──────────────────────────────────────────────

func TestMarshalWithStrings(t *testing.T) {
	expectMarshal(t, "all_empty", TWithStrings{}, "0C00000000000000000000000000")
	expectMarshal(t, "mixed", TWithStrings{Hello: "hello", Unicode: "\xc3\xa9mojis: \xf0\x9f\x8e\x89\xf0\x9f\x9a\x80"},
		"0C0000000000080000000D0000000500000068656C6C6F11000000C3A96D6F6A69733A20F09F8E89F09F9A80")
}

// ── WithVectors (5 cases) ──────────────────────────────────────────────

func TestMarshalWithVectors(t *testing.T) {
	expectMarshal(t, "both_empty", TWithVectors{}, "08000000000000000000")
	expectMarshal(t, "ints_only", TWithVectors{Ints: []uint32{1, 2, 3}},
		"080008000000000000000C000000010000000200000003000000")
	expectMarshal(t, "strings_only", TWithVectors{Strings: []string{"a", "bb", "ccc"}},
		"080000000000040000000C0000000C0000000D0000000F000000010000006102000000626203000000636363")
	expectMarshal(t, "both_filled", TWithVectors{Ints: []uint32{10, 20}, Strings: []string{"hello", "world"}},
		"08000800000010000000080000000A0000001400000008000000080000000D0000000500000068656C6C6F05000000776F726C64")
	expectMarshal(t, "single", TWithVectors{Ints: []uint32{42}, Strings: []string{"only"}},
		"0800080000000C000000040000002A0000000400000004000000040000006F6E6C79")
}

// ── WithOptionals (5 cases) ────────────────────────────────────────────

func TestMarshalWithOptionals(t *testing.T) {
	expectMarshal(t, "both_null", TWithOptionals{}, "0000")
	expectMarshal(t, "int_only", TWithOptionals{OptInt: u32p(42)}, "0400040000002A000000")
	expectMarshal(t, "str_only", TWithOptionals{OptStr: strp("hello")}, "080001000000040000000500000068656C6C6F")
	expectMarshal(t, "both", TWithOptionals{OptInt: u32p(99), OptStr: strp("world")},
		"080008000000080000006300000005000000776F726C64")
	expectMarshal(t, "zero_int", TWithOptionals{OptInt: u32p(0)}, "04000400000000000000")
}

// ── Inner (3 cases) ────────────────────────────────────────────────────

func TestMarshalInner(t *testing.T) {
	expectMarshal(t, "simple", TInner{Value: 42, Label: "hello"}, "08002A000000040000000500000068656C6C6F")
	expectMarshal(t, "empty_label", TInner{}, "08000000000000000000")
	expectMarshal(t, "max", TInner{Value: 4294967295, Label: "max"}, "0800FFFFFFFF04000000030000006D6178")
}

// ── Outer (3 cases) ────────────────────────────────────────────────────

func TestMarshalOuter(t *testing.T) {
	expectMarshal(t, "simple", TOuter{Inner: TInner{Value: 1, Label: "inner"}, Name: "outer"},
		"080008000000170000000800010000000400000005000000696E6E6572050000006F75746572")
	expectMarshal(t, "empty", TOuter{},
		"0800080000000000000008000000000000000000")
	expectMarshal(t, "unicode", TOuter{Inner: TInner{Value: 42, Label: "caf\xc3\xa9"}, Name: "na\xc3\xafve"},
		"0800080000001700000008002A0000000400000005000000636166C3A9060000006E61C3AF7665")
}

// ── WithVariant (5 cases) ──────────────────────────────────────────────

func TestMarshalWithVariant(t *testing.T) {
	expectMarshal(t, "uint32", TWithVariant{Data: TDataVariant{Uint32: u32p(42)}},
		"04000400000000040000002A000000")
	expectMarshal(t, "string", TWithVariant{Data: TDataVariant{String: strp("hello")}},
		"04000400000001090000000500000068656C6C6F")
	expectMarshal(t, "struct", TWithVariant{Data: TDataVariant{Inner: &TInner{Value: 7, Label: "variant_inner"}}},
		"040004000000021B000000080007000000040000000D00000076617269616E745F696E6E6572")
	expectMarshal(t, "uint32_zero", TWithVariant{Data: TDataVariant{Uint32: u32p(0)}},
		"040004000000000400000000000000")
	expectMarshal(t, "string_empty", TWithVariant{Data: TDataVariant{String: strp("")}},
		"040004000000010400000000000000")
}

// ── VecOfStructs (3 cases) ─────────────────────────────────────────────

func TestMarshalVecOfStructs(t *testing.T) {
	expectMarshal(t, "empty", TVecOfStructs{}, "040000000000")
	expectMarshal(t, "single", TVecOfStructs{Items: []TInner{{Value: 1, Label: "one"}}},
		"040004000000040000000400000008000100000004000000030000006F6E65")
	expectMarshal(t, "multiple", TVecOfStructs{Items: []TInner{
		{Value: 1, Label: "one"}, {Value: 2, Label: "two"}, {Value: 3, Label: "three"},
	}}, "0400040000000C0000000C000000190000002600000008000100000004000000030000006F6E65080002000000040000000300000074776F08000300000004000000050000007468726565")
}

// ── OptionalStruct (2 cases) ───────────────────────────────────────────

func TestMarshalOptionalStruct(t *testing.T) {
	expectMarshal(t, "null", TOptionalStruct{}, "0000")
	expectMarshal(t, "present", TOptionalStruct{Item: &TInner{Value: 42, Label: "exists"}},
		"04000400000008002A0000000400000006000000657869737473")
}

// ── VecOfOptionals (4 cases) ───────────────────────────────────────────

func TestMarshalVecOfOptionals(t *testing.T) {
	expectMarshal(t, "empty", TVecOfOptionals{}, "040000000000")
	expectMarshal(t, "all_null", TVecOfOptionals{Items: []*uint32{nil, nil, nil}},
		"0400040000000C000000010000000100000001000000")
	expectMarshal(t, "all_present", TVecOfOptionals{Items: []*uint32{u32p(1), u32p(2), u32p(3)}},
		"0400040000000C0000000C0000000C0000000C000000010000000200000003000000")
	expectMarshal(t, "mixed", TVecOfOptionals{Items: []*uint32{u32p(1), nil, u32p(3), nil}},
		"0400040000001000000010000000010000000C000000010000000100000003000000")
}

// ── OptionalVec (3 cases) ──────────────────────────────────────────────

func TestMarshalOptionalVec(t *testing.T) {
	expectMarshal(t, "null", TOptionalVec{}, "0000")
	empty := []uint32{}
	expectMarshal(t, "empty_vec", TOptionalVec{Items: &empty}, "040000000000")
	vals := []uint32{10, 20, 30}
	expectMarshal(t, "with_values", TOptionalVec{Items: &vals},
		"0400040000000C0000000A000000140000001E000000")
}

// ── NestedVecs (4 cases) ───────────────────────────────────────────────

func TestMarshalNestedVecs(t *testing.T) {
	expectMarshal(t, "empty", TNestedVecs{}, "040000000000")
	expectMarshal(t, "empty_rows", TNestedVecs{Matrix: [][]uint32{{}, {}, {}}},
		"0400040000000C000000000000000000000000000000")
	expectMarshal(t, "identity_2x2", TNestedVecs{Matrix: [][]uint32{{1, 0}, {0, 1}}},
		"040004000000080000000800000010000000080000000100000000000000080000000000000001000000")
	expectMarshal(t, "ragged", TNestedVecs{Matrix: [][]uint32{{1}, {2, 3}, {4, 5, 6}}},
		"0400040000000C0000000C000000100000001800000004000000010000000800000002000000030000000C000000040000000500000006000000")
}

// ── FixedArray (3 cases) ───────────────────────────────────────────────

func TestMarshalFixedArray(t *testing.T) {
	expectMarshal(t, "zeros", TFixedArray{Arr: [3]uint32{0, 0, 0}}, "000000000000000000000000")
	expectMarshal(t, "sequence", TFixedArray{Arr: [3]uint32{1, 2, 3}}, "010000000200000003000000")
	expectMarshal(t, "max", TFixedArray{Arr: [3]uint32{4294967295, 4294967295, 4294967295}}, "FFFFFFFFFFFFFFFFFFFFFFFF")
}

// ── Complex (3 cases) ──────────────────────────────────────────────────

func TestMarshalComplex(t *testing.T) {
	expectMarshal(t, "all_empty", TComplex{}, "0C00000000000100000000000000")

	optVec := []uint32{10, 20}
	expectMarshal(t, "all_populated", TComplex{
		Items:     []TInner{{Value: 1, Label: "a"}, {Value: 2, Label: "b"}},
		OptVec:    &optVec,
		VecOpt:    []*string{strp("x"), nil, strp("z")},
		OptStruct: &TInner{Value: 99, Label: "present"},
	}, "100010000000360000003E00000054000000080000000800000013000000080001000000040000000100000061080002000000040000000100000062080000000A000000140000000C0000000C00000001000000090000000100000078010000007A080063000000040000000700000070726573656E74")

	expectMarshal(t, "sparse", TComplex{
		Items:  []TInner{{Value: 42, Label: "only"}},
		VecOpt: []*string{nil, nil},
	}, "0C000C000000010000001E000000040000000400000008002A00000004000000040000006F6E6C79080000000100000001000000")
}

// ── EmptyExtensible (2 cases) ──────────────────────────────────────────

func TestMarshalEmptyExtensible(t *testing.T) {
	expectMarshal(t, "zero", TEmptyExtensible{Dummy: 0}, "040000000000")
	expectMarshal(t, "max", TEmptyExtensible{Dummy: 4294967295}, "0400FFFFFFFF")
}

// ── Unmarshal round-trip ───────────────────────────────────────────────

func TestUnmarshalInner(t *testing.T) {
	data, _ := hex.DecodeString("08002A000000040000000500000068656C6C6F")
	var out TInner
	if err := Unmarshal(data, &out); err != nil {
		t.Fatal(err)
	}
	if out.Value != 42 {
		t.Errorf("value = %d, want 42", out.Value)
	}
	if out.Label != "hello" {
		t.Errorf("label = %q, want hello", out.Label)
	}
}

func TestUnmarshalOuter(t *testing.T) {
	data, _ := hex.DecodeString("080008000000170000000800010000000400000005000000696E6E6572050000006F75746572")
	var out TOuter
	if err := Unmarshal(data, &out); err != nil {
		t.Fatal(err)
	}
	if out.Inner.Value != 1 {
		t.Errorf("inner.value = %d, want 1", out.Inner.Value)
	}
	if out.Inner.Label != "inner" {
		t.Errorf("inner.label = %q, want inner", out.Inner.Label)
	}
	if out.Name != "outer" {
		t.Errorf("name = %q, want outer", out.Name)
	}
}
