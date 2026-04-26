package fracpack

import (
	"encoding/hex"
	"testing"
)

// ── Valid data tests ───────────────────────────────────────────────────

func TestValidateFixedStruct(t *testing.T) {
	// FixedInts: x=42, y=100 — non-extensible, two i32 fields
	data, _ := hex.DecodeString("2A00000064000000")
	td := buildTypeDef("FixedInts")
	r := Validate(td, data)
	if r.Status != Valid {
		t.Fatalf("expected Valid, got %v: %s", r.Status, r.Message)
	}
}

func TestValidateExtensibleStruct(t *testing.T) {
	// SingleString: value="hi"
	// u16 fixed=4, offset=4(relative), then "hi": u32 len=2 + "hi"
	data, _ := hex.DecodeString("0400" + "04000000" + "02000000" + "6869")
	td := buildTypeDef("SingleString")
	r := Validate(td, data)
	if r.Status != Valid {
		t.Fatalf("expected Valid, got %v: %s", r.Status, r.Message)
	}
}

func TestValidateAllPrimitives(t *testing.T) {
	// AllPrimitives: ones case from test vectors
	data, _ := hex.DecodeString("2B00010101010001000100000001000000010000000000000001000000000000000000803F000000000000F03F")
	td := buildTypeDef("AllPrimitives")
	r := Validate(td, data)
	if r.Status != Valid {
		t.Fatalf("expected Valid, got %v: %s", r.Status, r.Message)
	}
}

func TestValidateWithStrings(t *testing.T) {
	// WithStrings: empty, "hello", unicode
	data, _ := hex.DecodeString("0C0000000000080000000D0000000500000068656C6C6F11000000C3A96D6F6A69733A20F09F8E89F09F9A80")
	td := buildTypeDef("WithStrings")
	r := Validate(td, data)
	if r.Status != Valid {
		t.Fatalf("expected Valid, got %v: %s", r.Status, r.Message)
	}
}

func TestValidateWithVectors(t *testing.T) {
	td := buildTypeDef("WithVectors")
	// Pack known values and validate
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindVec, Vec: []Value{
			{Kind: KindU32, U32: 1},
			{Kind: KindU32, U32: 2},
			{Kind: KindU32, U32: 3},
		}},
		{Kind: KindVec, Vec: []Value{
			{Kind: KindString, Str: "a"},
			{Kind: KindString, Str: "bb"},
		}},
	}}
	data := Pack(td, val)
	r := Validate(td, data)
	if r.Status != Valid {
		t.Fatalf("expected Valid, got %v: %s", r.Status, r.Message)
	}
}

func TestValidateWithOptionals(t *testing.T) {
	td := buildTypeDef("WithOptionals")
	// opt_int=Some(42), opt_str=None
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindOptional, Opt: &Value{Kind: KindU32, U32: 42}},
		{Kind: KindOptional, Opt: nil},
	}}
	data := Pack(td, val)
	r := Validate(td, data)
	if r.Status != Valid {
		t.Fatalf("expected Valid, got %v: %s", r.Status, r.Message)
	}
}

func TestValidateOptionalNone(t *testing.T) {
	td := buildTypeDef("WithOptionals")
	// Both absent — trailing elision means fixed_size=0
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindOptional, Opt: nil},
		{Kind: KindOptional, Opt: nil},
	}}
	data := Pack(td, val)
	r := Validate(td, data)
	if r.Status != Valid {
		t.Fatalf("expected Valid, got %v: %s", r.Status, r.Message)
	}
}

func TestValidateNested(t *testing.T) {
	// Outer: simple case
	data, _ := hex.DecodeString("080008000000170000000800010000000400000005000000696E6E6572050000006F75746572")
	td := buildTypeDef("Outer")
	r := Validate(td, data)
	if r.Status != Valid {
		t.Fatalf("expected Valid, got %v: %s", r.Status, r.Message)
	}
}

func TestValidateVariant(t *testing.T) {
	td := buildTypeDef("WithVariant")
	// variant case 0 (u32): tag=0, size=4, value=255
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindVariant, Tag: 0, Variant: &Value{Kind: KindU32, U32: 255}},
	}}
	data := Pack(td, val)
	r := Validate(td, data)
	if r.Status != Valid {
		t.Fatalf("expected Valid, got %v: %s", r.Status, r.Message)
	}
}

func TestValidateVariantString(t *testing.T) {
	td := buildTypeDef("WithVariant")
	// variant case 1 (string)
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindVariant, Tag: 1, Variant: &Value{Kind: KindString, Str: "hello"}},
	}}
	data := Pack(td, val)
	r := Validate(td, data)
	if r.Status != Valid {
		t.Fatalf("expected Valid, got %v: %s", r.Status, r.Message)
	}
}

func TestValidateVecOfStructs(t *testing.T) {
	td := buildTypeDef("VecOfStructs")
	inner := innerTypeDef()
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindVec, Vec: []Value{
			{Kind: KindObject, Fields: []Value{
				{Kind: KindU32, U32: 1},
				{Kind: KindString, Str: "a"},
			}},
			{Kind: KindObject, Fields: []Value{
				{Kind: KindU32, U32: 2},
				{Kind: KindString, Str: "bb"},
			}},
		}},
	}}
	_ = inner
	data := Pack(td, val)
	r := Validate(td, data)
	if r.Status != Valid {
		t.Fatalf("expected Valid, got %v: %s", r.Status, r.Message)
	}
}

func TestValidateFixedArray(t *testing.T) {
	// FixedArray: [10, 20, 30] — non-extensible, three u32 fields
	data, _ := hex.DecodeString("0A000000140000001E000000")
	td := buildTypeDef("FixedArray")
	r := Validate(td, data)
	if r.Status != Valid {
		t.Fatalf("expected Valid, got %v: %s", r.Status, r.Message)
	}
}

// ── Invalid data tests ─────────────────────────────────────────────────

func TestValidateTruncated(t *testing.T) {
	// FixedInts needs 8 bytes, give it 4
	data, _ := hex.DecodeString("2A000000")
	td := buildTypeDef("FixedInts")
	r := Validate(td, data)
	if r.Status != Invalid {
		t.Fatalf("expected Invalid for truncated data, got %v", r.Status)
	}
}

func TestValidateExtraTrailingBytes(t *testing.T) {
	// FixedInts (8 bytes) + 2 extra bytes
	data, _ := hex.DecodeString("2A00000064000000DEAD")
	td := buildTypeDef("FixedInts")
	r := Validate(td, data)
	if r.Status != Invalid {
		t.Fatalf("expected Invalid for extra trailing bytes, got %v", r.Status)
	}
	if r.Message == "" {
		t.Fatal("expected error message for extra trailing bytes")
	}
}

func TestValidateEmptyBuffer(t *testing.T) {
	td := buildTypeDef("FixedInts")
	r := Validate(td, []byte{})
	if r.Status != Invalid {
		t.Fatalf("expected Invalid for empty buffer, got %v", r.Status)
	}
}

func TestValidateBadBool(t *testing.T) {
	// SingleBool with value=2 (invalid, must be 0 or 1)
	data := []byte{0x02}
	td := buildTypeDef("SingleBool")
	r := Validate(td, data)
	if r.Status != Invalid {
		t.Fatalf("expected Invalid for bad bool value, got %v", r.Status)
	}
}

func TestValidateBadBoolMax(t *testing.T) {
	// SingleBool with value=0xFF
	data := []byte{0xFF}
	td := buildTypeDef("SingleBool")
	r := Validate(td, data)
	if r.Status != Invalid {
		t.Fatalf("expected Invalid for bad bool 0xFF, got %v", r.Status)
	}
}

func TestValidateBadUTF8(t *testing.T) {
	// SingleString with invalid UTF-8
	// u16 fixed_size=4, offset=4(relative to offset field pos),
	// then string: u32 len=2, bytes=0xFF 0xFE (invalid UTF-8)
	data := []byte{
		0x04, 0x00, // fixed_size = 4
		0x04, 0x00, 0x00, 0x00, // offset = 4
		0x02, 0x00, 0x00, 0x00, // string length = 2
		0xFF, 0xFE, // invalid UTF-8 bytes
	}
	td := buildTypeDef("SingleString")
	r := Validate(td, data)
	if r.Status != Invalid {
		t.Fatalf("expected Invalid for bad UTF-8, got %v", r.Status)
	}
}

func TestValidateBadVariantTag(t *testing.T) {
	// Variant with tag=5, but only 3 cases defined
	// Build raw data: extensible struct with one field (offset to variant)
	// Variant data: [tag=5][size=4][u32 dummy]
	data := []byte{
		0x04, 0x00, // fixed_size = 4
		0x04, 0x00, 0x00, 0x00, // offset = 4 (relative)
		0x05,                   // tag = 5 (out of range)
		0x04, 0x00, 0x00, 0x00, // data_size = 4
		0x00, 0x00, 0x00, 0x00, // dummy payload
	}
	td := buildTypeDef("WithVariant")
	r := Validate(td, data)
	if r.Status != Invalid {
		t.Fatalf("expected Invalid for bad variant tag, got %v", r.Status)
	}
}

func TestValidateBadOffset(t *testing.T) {
	// SingleString with offset pointing past end of buffer
	data := []byte{
		0x04, 0x00, // fixed_size = 4
		0xFF, 0x00, 0x00, 0x00, // offset = 255 (way past end)
	}
	td := buildTypeDef("SingleString")
	r := Validate(td, data)
	if r.Status != Invalid {
		t.Fatalf("expected Invalid for bad offset, got %v", r.Status)
	}
}

func TestValidateReservedOffset(t *testing.T) {
	// SingleString with reserved offset value 2
	data := []byte{
		0x04, 0x00, // fixed_size = 4
		0x02, 0x00, 0x00, 0x00, // offset = 2 (reserved)
	}
	td := buildTypeDef("SingleString")
	r := Validate(td, data)
	if r.Status != Invalid {
		t.Fatalf("expected Invalid for reserved offset 2, got %v", r.Status)
	}
}

func TestValidateReservedOffset3(t *testing.T) {
	// SingleString with reserved offset value 3
	data := []byte{
		0x04, 0x00, // fixed_size = 4
		0x03, 0x00, 0x00, 0x00, // offset = 3 (reserved)
	}
	td := buildTypeDef("SingleString")
	r := Validate(td, data)
	if r.Status != Invalid {
		t.Fatalf("expected Invalid for reserved offset 3, got %v", r.Status)
	}
}

func TestValidateVecBadAlignment(t *testing.T) {
	// Vec with data_size not divisible by element size (u32 = 4 bytes)
	// Extensible struct with one vec field
	data := []byte{
		0x04, 0x00, // fixed_size = 4
		0x04, 0x00, 0x00, 0x00, // offset = 4 (relative)
		0x05, 0x00, 0x00, 0x00, // data_size = 5 (not divisible by 4)
		0x01, 0x02, 0x03, 0x04, 0x05, // 5 bytes of data
	}
	td := &TypeDef{Name: "VecTest", Extensible: true, Fields: []FieldDef{
		{Name: "items", Kind: KindVec, FixedSize: 4, IsVar: true, ElemKind: KindU32},
	}}
	r := Validate(td, data)
	if r.Status != Invalid {
		t.Fatalf("expected Invalid for vec with bad alignment, got %v", r.Status)
	}
}

func TestValidateStringTruncated(t *testing.T) {
	// String claims 100 bytes but buffer is short
	data := []byte{
		0x04, 0x00, // fixed_size = 4
		0x04, 0x00, 0x00, 0x00, // offset = 4
		0x64, 0x00, 0x00, 0x00, // string length = 100
		0x41, 0x42, // only 2 bytes of data
	}
	td := buildTypeDef("SingleString")
	r := Validate(td, data)
	if r.Status != Invalid {
		t.Fatalf("expected Invalid for truncated string, got %v", r.Status)
	}
}

func TestValidateVariantTruncated(t *testing.T) {
	// Variant header truncated (need 5 bytes for tag+size)
	data := []byte{
		0x04, 0x00, // fixed_size = 4
		0x04, 0x00, 0x00, 0x00, // offset = 4
		0x00, 0x01, // only 2 bytes (need 5)
	}
	td := buildTypeDef("WithVariant")
	r := Validate(td, data)
	if r.Status != Invalid {
		t.Fatalf("expected Invalid for truncated variant, got %v", r.Status)
	}
}

func TestValidateOffset1OnNonOptional(t *testing.T) {
	// String field with offset=1 (None) — only valid for optionals
	data := []byte{
		0x04, 0x00, // fixed_size = 4
		0x01, 0x00, 0x00, 0x00, // offset = 1 (None — invalid for string)
	}
	td := buildTypeDef("SingleString")
	r := Validate(td, data)
	if r.Status != Invalid {
		t.Fatalf("expected Invalid for offset=1 on non-optional, got %v", r.Status)
	}
}

// ── Extended tests ─────────────────────────────────────────────────────

func TestValidateExtendedStruct(t *testing.T) {
	// SingleString but with extra bytes in the fixed region.
	// Normal SingleString has fixed_size=4 (one offset field).
	// An "extended" version would have a larger fixed_size.
	//
	// fixed_size=8, first 4 bytes = offset to string, next 4 = unknown field
	data := []byte{
		0x08, 0x00, // fixed_size = 8
		0x08, 0x00, 0x00, 0x00, // offset = 8 (relative, pointing past unknown field)
		0x00, 0x00, 0x00, 0x00, // unknown extra fixed field
		0x02, 0x00, 0x00, 0x00, // string length = 2
		0x68, 0x69, // "hi"
	}
	td := buildTypeDef("SingleString")
	r := Validate(td, data)
	if r.Status != Extended {
		t.Fatalf("expected Extended for struct with extra fixed bytes, got %v: %s", r.Status, r.Message)
	}
}

func TestValidateExtendedStructEmptyString(t *testing.T) {
	// Extended struct where the known string field is empty (offset=0)
	// and there's an extra fixed field.
	data := []byte{
		0x08, 0x00, // fixed_size = 8
		0x00, 0x00, 0x00, 0x00, // offset = 0 (empty string)
		0x2A, 0x00, 0x00, 0x00, // unknown extra fixed field = 42
	}
	td := buildTypeDef("SingleString")
	r := Validate(td, data)
	if r.Status != Extended {
		t.Fatalf("expected Extended, got %v: %s", r.Status, r.Message)
	}
}

// ── Golden vector tests ────────────────────────────────────────────────

func TestValidateGoldenVectors(t *testing.T) {
	vectors := loadVectors(t)

	for _, tt := range vectors.Types {
		td := buildTypeDef(tt.Name)
		if td == nil {
			t.Logf("Skipping %s (no type definition)", tt.Name)
			continue
		}

		for _, tc := range tt.Cases {
			t.Run(tt.Name+"/"+tc.Name, func(t *testing.T) {
				data, err := hex.DecodeString(tc.PackedHex)
				if err != nil {
					t.Fatalf("bad hex: %v", err)
				}
				r := Validate(td, data)
				if r.Status == Invalid {
					t.Errorf("expected Valid or Extended, got Invalid: %s", r.Message)
				}
			})
		}
	}
}

// ── Round-trip validation (pack then validate) ─────────────────────────

func TestValidateRoundTripOptionalStruct(t *testing.T) {
	td := buildTypeDef("OptionalStruct")
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindOptional, Opt: &Value{Kind: KindObject, Fields: []Value{
			{Kind: KindU32, U32: 99},
			{Kind: KindString, Str: "test"},
		}}},
	}}
	data := Pack(td, val)
	r := Validate(td, data)
	if r.Status != Valid {
		t.Fatalf("expected Valid, got %v: %s", r.Status, r.Message)
	}
}

func TestValidateRoundTripComplex(t *testing.T) {
	td := buildTypeDef("Complex")
	if td == nil {
		t.Skip("Complex type not defined")
	}
	val := &Value{Kind: KindObject, Fields: []Value{
		// items: vec of structs
		{Kind: KindVec, Vec: []Value{
			{Kind: KindObject, Fields: []Value{
				{Kind: KindU32, U32: 1},
				{Kind: KindString, Str: "first"},
			}},
		}},
		// opt_vec: Some([10, 20])
		{Kind: KindOptional, Opt: &Value{Kind: KindVec, Vec: []Value{
			{Kind: KindU32, U32: 10},
			{Kind: KindU32, U32: 20},
		}}},
		// vec_opt: [Some(5), None, Some(7)]
		{Kind: KindVec, Vec: []Value{
			{Kind: KindOptional, Opt: &Value{Kind: KindU32, U32: 5}},
			{Kind: KindOptional, Opt: nil},
			{Kind: KindOptional, Opt: &Value{Kind: KindU32, U32: 7}},
		}},
		// opt_struct: None
		{Kind: KindOptional, Opt: nil},
	}}
	data := Pack(td, val)
	r := Validate(td, data)
	if r.Status != Valid {
		t.Fatalf("expected Valid, got %v: %s", r.Status, r.Message)
	}
}

func TestValidateRoundTripEmptyExtensible(t *testing.T) {
	td := buildTypeDef("EmptyExtensible")
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindU32, U32: 0},
	}}
	data := Pack(td, val)
	r := Validate(td, data)
	if r.Status != Valid {
		t.Fatalf("expected Valid, got %v: %s", r.Status, r.Message)
	}
}

func TestValidateRoundTripNestedVecs(t *testing.T) {
	td := buildTypeDef("NestedVecs")
	if td == nil {
		t.Skip("NestedVecs type not defined")
	}
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindVec, Vec: []Value{
			{Kind: KindVec, Vec: []Value{
				{Kind: KindU32, U32: 1},
				{Kind: KindU32, U32: 2},
			}},
			{Kind: KindVec, Vec: []Value{
				{Kind: KindU32, U32: 3},
			}},
		}},
	}}
	data := Pack(td, val)
	r := Validate(td, data)
	if r.Status != Valid {
		t.Fatalf("expected Valid, got %v: %s", r.Status, r.Message)
	}
}

// ── Variant payload size mismatch ──────────────────────────────────────

func TestValidateVariantPayloadSizeMismatch(t *testing.T) {
	// Variant with tag=0 (u32), data_size=8 but payload is only 4 bytes of u32
	data := []byte{
		0x04, 0x00, // fixed_size = 4
		0x04, 0x00, 0x00, 0x00, // offset = 4
		0x00,                   // tag = 0
		0x08, 0x00, 0x00, 0x00, // data_size = 8 (but u32 is only 4)
		0xFF, 0x00, 0x00, 0x00, // u32 value
		0x00, 0x00, 0x00, 0x00, // extra data
	}
	td := buildTypeDef("WithVariant")
	r := Validate(td, data)
	if r.Status != Invalid {
		t.Fatalf("expected Invalid for variant payload size mismatch, got %v", r.Status)
	}
}
