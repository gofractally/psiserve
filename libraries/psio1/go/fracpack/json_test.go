package fracpack

import (
	"encoding/hex"
	"encoding/json"
	"math"
	"os"
	"testing"
)

// ── Scalar round-trip tests ─────────────────────────────────────────────

func TestJSONScalarBool(t *testing.T) {
	td := &TypeDef{Name: "S", Fields: []FieldDef{
		{Name: "v", Kind: KindBool, FixedSize: 1},
	}}
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindBool, Bool: true},
	}}
	got, err := ToJSON(td, val)
	if err != nil {
		t.Fatal(err)
	}
	expect := `{"v":true}`
	if string(got) != expect {
		t.Errorf("got %s, want %s", got, expect)
	}

	// Round-trip
	rt, err := FromJSON(td, got)
	if err != nil {
		t.Fatal(err)
	}
	if !rt.Fields[0].Bool {
		t.Error("round-trip: expected true")
	}
}

func TestJSONScalarIntegers(t *testing.T) {
	td := &TypeDef{Name: "S", Fields: []FieldDef{
		{Name: "u8", Kind: KindU8, FixedSize: 1},
		{Name: "i8", Kind: KindI8, FixedSize: 1},
		{Name: "u16", Kind: KindU16, FixedSize: 2},
		{Name: "i16", Kind: KindI16, FixedSize: 2},
		{Name: "u32", Kind: KindU32, FixedSize: 4},
		{Name: "i32", Kind: KindI32, FixedSize: 4},
	}}
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindU8, U8: 255},
		{Kind: KindI8, I8: -128},
		{Kind: KindU16, U16: 65535},
		{Kind: KindI16, I16: -32768},
		{Kind: KindU32, U32: 4294967295},
		{Kind: KindI32, I32: -2147483648},
	}}
	got, err := ToJSON(td, val)
	if err != nil {
		t.Fatal(err)
	}
	expect := `{"u8":255,"i8":-128,"u16":65535,"i16":-32768,"u32":4294967295,"i32":-2147483648}`
	if string(got) != expect {
		t.Errorf("got %s, want %s", got, expect)
	}

	// Round-trip
	rt, err := FromJSON(td, got)
	if err != nil {
		t.Fatal(err)
	}
	if rt.Fields[0].U8 != 255 {
		t.Errorf("u8 round-trip: got %d, want 255", rt.Fields[0].U8)
	}
	if rt.Fields[1].I8 != -128 {
		t.Errorf("i8 round-trip: got %d, want -128", rt.Fields[1].I8)
	}
}

func TestJSONU64AsString(t *testing.T) {
	td := &TypeDef{Name: "S", Fields: []FieldDef{
		{Name: "v", Kind: KindU64, FixedSize: 8},
	}}
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindU64, U64: 18446744073709551615},
	}}
	got, err := ToJSON(td, val)
	if err != nil {
		t.Fatal(err)
	}
	expect := `{"v":"18446744073709551615"}`
	if string(got) != expect {
		t.Errorf("got %s, want %s", got, expect)
	}

	// Round-trip from string
	rt, err := FromJSON(td, got)
	if err != nil {
		t.Fatal(err)
	}
	if rt.Fields[0].U64 != 18446744073709551615 {
		t.Errorf("u64 round-trip: got %d, want max uint64", rt.Fields[0].U64)
	}
}

func TestJSONI64AsString(t *testing.T) {
	td := &TypeDef{Name: "S", Fields: []FieldDef{
		{Name: "v", Kind: KindI64, FixedSize: 8},
	}}
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindI64, I64: -9223372036854775808},
	}}
	got, err := ToJSON(td, val)
	if err != nil {
		t.Fatal(err)
	}
	expect := `{"v":"-9223372036854775808"}`
	if string(got) != expect {
		t.Errorf("got %s, want %s", got, expect)
	}

	// Round-trip
	rt, err := FromJSON(td, got)
	if err != nil {
		t.Fatal(err)
	}
	if rt.Fields[0].I64 != -9223372036854775808 {
		t.Errorf("i64 round-trip: got %d, want min int64", rt.Fields[0].I64)
	}
}

func TestJSONFloats(t *testing.T) {
	td := &TypeDef{Name: "S", Extensible: true, Fields: []FieldDef{
		{Name: "f32", Kind: KindF32, FixedSize: 4},
		{Name: "f64", Kind: KindF64, FixedSize: 8},
	}}
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindF32, F32: 3.14},
		{Kind: KindF64, F64: 2.718281828459045},
	}}
	got, err := ToJSON(td, val)
	if err != nil {
		t.Fatal(err)
	}
	// Verify it parses back correctly
	rt, err := FromJSON(td, got)
	if err != nil {
		t.Fatal(err)
	}
	if math.Abs(float64(rt.Fields[0].F32)-3.14) > 0.001 {
		t.Errorf("f32 round-trip: got %f, want ~3.14", rt.Fields[0].F32)
	}
	if math.Abs(rt.Fields[1].F64-2.718281828459045) > 1e-15 {
		t.Errorf("f64 round-trip: got %f, want 2.718281828459045", rt.Fields[1].F64)
	}
}

func TestJSONFloatNaN(t *testing.T) {
	td := &TypeDef{Name: "S", Fields: []FieldDef{
		{Name: "v", Kind: KindF64, FixedSize: 8},
	}}
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindF64, F64: math.NaN()},
	}}
	got, err := ToJSON(td, val)
	if err != nil {
		t.Fatal(err)
	}
	expect := `{"v":null}`
	if string(got) != expect {
		t.Errorf("got %s, want %s", got, expect)
	}
}

// ── String tests ────────────────────────────────────────────────────────

func TestJSONString(t *testing.T) {
	td := &TypeDef{Name: "S", Extensible: true, Fields: []FieldDef{
		{Name: "v", Kind: KindString, FixedSize: 4, IsVar: true},
	}}
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindString, Str: "hello \"world\"\n"},
	}}
	got, err := ToJSON(td, val)
	if err != nil {
		t.Fatal(err)
	}
	expect := `{"v":"hello \"world\"\n"}`
	if string(got) != expect {
		t.Errorf("got %s, want %s", got, expect)
	}

	rt, err := FromJSON(td, got)
	if err != nil {
		t.Fatal(err)
	}
	if rt.Fields[0].Str != "hello \"world\"\n" {
		t.Errorf("string round-trip: got %q, want %q", rt.Fields[0].Str, "hello \"world\"\n")
	}
}

// ── Optional tests ──────────────────────────────────────────────────────

func TestJSONOptionalNone(t *testing.T) {
	td := &TypeDef{Name: "S", Extensible: true, Fields: []FieldDef{
		{Name: "v", Kind: KindOptional, FixedSize: 4, IsVar: true, ElemKind: KindU32},
	}}
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindOptional, Opt: nil},
	}}
	got, err := ToJSON(td, val)
	if err != nil {
		t.Fatal(err)
	}
	expect := `{"v":null}`
	if string(got) != expect {
		t.Errorf("got %s, want %s", got, expect)
	}

	rt, err := FromJSON(td, got)
	if err != nil {
		t.Fatal(err)
	}
	if rt.Fields[0].Opt != nil {
		t.Error("expected nil optional")
	}
}

func TestJSONOptionalSome(t *testing.T) {
	td := &TypeDef{Name: "S", Extensible: true, Fields: []FieldDef{
		{Name: "v", Kind: KindOptional, FixedSize: 4, IsVar: true, ElemKind: KindU32},
	}}
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindOptional, Opt: &Value{Kind: KindU32, U32: 42}},
	}}
	got, err := ToJSON(td, val)
	if err != nil {
		t.Fatal(err)
	}
	expect := `{"v":42}`
	if string(got) != expect {
		t.Errorf("got %s, want %s", got, expect)
	}

	rt, err := FromJSON(td, got)
	if err != nil {
		t.Fatal(err)
	}
	if rt.Fields[0].Opt == nil || rt.Fields[0].Opt.U32 != 42 {
		t.Error("expected optional 42")
	}
}

// ── Variant tests ───────────────────────────────────────────────────────

func TestJSONVariant(t *testing.T) {
	inner := innerTypeDef()
	td := &TypeDef{Name: "WithVariant", Extensible: true, Fields: []FieldDef{
		{Name: "data", Kind: KindVariant, FixedSize: 4, IsVar: true,
			InnerDef: &TypeDef{Name: "DataVariant", IsVariant: true, Cases: []FieldDef{
				{Name: "uint32", Kind: KindU32, FixedSize: 4},
				{Name: "string", Kind: KindString, FixedSize: 4, IsVar: true},
				{Name: "Inner", Kind: KindObject, FixedSize: 4, IsVar: true, InnerDef: inner},
			}}},
	}}

	// Test uint32 variant
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindVariant, Tag: 0, Variant: &Value{Kind: KindU32, U32: 42}},
	}}
	got, err := ToJSON(td, val)
	if err != nil {
		t.Fatal(err)
	}
	expect := `{"data":{"uint32":42}}`
	if string(got) != expect {
		t.Errorf("got %s, want %s", got, expect)
	}

	rt, err := FromJSON(td, got)
	if err != nil {
		t.Fatal(err)
	}
	if rt.Fields[0].Tag != 0 || rt.Fields[0].Variant.U32 != 42 {
		t.Error("variant round-trip failed")
	}

	// Test string variant
	val = &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindVariant, Tag: 1, Variant: &Value{Kind: KindString, Str: "hello"}},
	}}
	got, err = ToJSON(td, val)
	if err != nil {
		t.Fatal(err)
	}
	expect = `{"data":{"string":"hello"}}`
	if string(got) != expect {
		t.Errorf("got %s, want %s", got, expect)
	}

	rt, err = FromJSON(td, got)
	if err != nil {
		t.Fatal(err)
	}
	if rt.Fields[0].Tag != 1 || rt.Fields[0].Variant.Str != "hello" {
		t.Error("string variant round-trip failed")
	}
}

// ── Nested struct tests ─────────────────────────────────────────────────

func TestJSONNestedStruct(t *testing.T) {
	inner := innerTypeDef()
	td := &TypeDef{Name: "Outer", Extensible: true, Fields: []FieldDef{
		{Name: "inner", Kind: KindObject, FixedSize: 4, IsVar: true, InnerDef: inner},
		{Name: "name", Kind: KindString, FixedSize: 4, IsVar: true},
	}}

	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindObject, Fields: []Value{
			{Kind: KindU32, U32: 1},
			{Kind: KindString, Str: "inner"},
		}},
		{Kind: KindString, Str: "outer"},
	}}

	got, err := ToJSON(td, val)
	if err != nil {
		t.Fatal(err)
	}
	expect := `{"inner":{"value":1,"label":"inner"},"name":"outer"}`
	if string(got) != expect {
		t.Errorf("got %s, want %s", got, expect)
	}

	rt, err := FromJSON(td, got)
	if err != nil {
		t.Fatal(err)
	}
	if rt.Fields[0].Fields[0].U32 != 1 {
		t.Error("inner.value round-trip failed")
	}
	if rt.Fields[0].Fields[1].Str != "inner" {
		t.Error("inner.label round-trip failed")
	}
	if rt.Fields[1].Str != "outer" {
		t.Error("name round-trip failed")
	}
}

// ── FixedArray (JSONArray) tests ────────────────────────────────────────

func TestJSONFixedArray(t *testing.T) {
	td := buildTypeDef("FixedArray")

	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindU32, U32: 1},
		{Kind: KindU32, U32: 2},
		{Kind: KindU32, U32: 3},
	}}

	got, err := ToJSON(td, val)
	if err != nil {
		t.Fatal(err)
	}
	expect := `{"arr":[1,2,3]}`
	if string(got) != expect {
		t.Errorf("got %s, want %s", got, expect)
	}

	rt, err := FromJSON(td, got)
	if err != nil {
		t.Fatal(err)
	}
	if rt.Fields[0].U32 != 1 || rt.Fields[1].U32 != 2 || rt.Fields[2].U32 != 3 {
		t.Error("FixedArray round-trip failed")
	}
}

// ── Golden vector round-trip tests ──────────────────────────────────────

func TestJSONGoldenVectors(t *testing.T) {
	data, err := os.ReadFile("../../test_vectors/vectors.json")
	if err != nil {
		t.Fatalf("cannot read vectors.json: %v", err)
	}
	var vectors testVectors
	if err := json.Unmarshal(data, &vectors); err != nil {
		t.Fatalf("cannot parse vectors.json: %v", err)
	}

	for _, tt := range vectors.Types {
		td := buildTypeDef(tt.Name)
		if td == nil {
			t.Logf("Skipping %s (no type definition)", tt.Name)
			continue
		}

		for _, tc := range tt.Cases {
			t.Run(tt.Name+"/"+tc.Name, func(t *testing.T) {
				// Parse the expected JSON into a canonical form for comparison
				expectedJSON := normalizeJSON(t, tc.JSON)

				// Test FromJSON -> Pack round-trip matches packed_hex
				packed, err := hex.DecodeString(tc.PackedHex)
				if err != nil {
					t.Fatalf("bad hex: %v", err)
				}

				// Test ToJSONFromPacked: packed bytes -> canonical JSON
				gotJSON, err := ToJSONFromPacked(td, packed)
				if err != nil {
					t.Fatalf("ToJSONFromPacked: %v", err)
				}

				// Normalize both JSONs for comparison
				gotNormalized := normalizeJSON(t, json.RawMessage(gotJSON))

				if gotNormalized != expectedJSON {
					t.Errorf("ToJSONFromPacked mismatch\n  got:  %s\n  want: %s", gotNormalized, expectedJSON)
				}

				// Test FromJSON -> Pack matches packed_hex
				val, err := FromJSON(td, tc.JSON)
				if err != nil {
					t.Fatalf("FromJSON: %v", err)
				}
				gotPacked := Pack(td, val)
				if !bytesEqual(gotPacked, packed) {
					t.Errorf("FromJSON->Pack mismatch\n  got:  %s\n  want: %s",
						hex.EncodeToString(gotPacked), tc.PackedHex)
				}
			})
		}
	}
}

// normalizeJSON parses and re-serializes JSON to get consistent formatting.
func normalizeJSON(t *testing.T, raw json.RawMessage) string {
	t.Helper()
	var v interface{}
	if err := json.Unmarshal(raw, &v); err != nil {
		t.Fatalf("cannot parse JSON: %v", err)
	}
	b, err := json.Marshal(v)
	if err != nil {
		t.Fatalf("cannot marshal JSON: %v", err)
	}
	return string(b)
}
