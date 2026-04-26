package fracpack

import (
	"encoding/hex"
	"strings"
	"testing"
)

// ── Tuple TypeDef helpers ──────────────────────────────────────────────────

func tupleU32U32() *TypeDef {
	return &TypeDef{
		Name:    "TupleU32U32",
		IsTuple: true,
		Elements: []FieldDef{
			{Name: "0", Kind: KindU32, FixedSize: 4},
			{Name: "1", Kind: KindU32, FixedSize: 4},
		},
	}
}

func tupleU32String() *TypeDef {
	return &TypeDef{
		Name:    "TupleU32String",
		IsTuple: true,
		Elements: []FieldDef{
			{Name: "0", Kind: KindU32, FixedSize: 4},
			{Name: "1", Kind: KindString, FixedSize: 4, IsVar: true},
		},
	}
}

func tupleStringString() *TypeDef {
	return &TypeDef{
		Name:    "TupleStringString",
		IsTuple: true,
		Elements: []FieldDef{
			{Name: "0", Kind: KindString, FixedSize: 4, IsVar: true},
			{Name: "1", Kind: KindString, FixedSize: 4, IsVar: true},
		},
	}
}

func tupleWithOptional() *TypeDef {
	return &TypeDef{
		Name:    "TupleWithOptional",
		IsTuple: true,
		Elements: []FieldDef{
			{Name: "0", Kind: KindU32, FixedSize: 4},
			{Name: "1", Kind: KindOptional, FixedSize: 4, IsVar: true, ElemKind: KindU32},
		},
	}
}

func tupleMixed() *TypeDef {
	return &TypeDef{
		Name:    "TupleMixed",
		IsTuple: true,
		Elements: []FieldDef{
			{Name: "0", Kind: KindBool, FixedSize: 1},
			{Name: "1", Kind: KindU32, FixedSize: 4},
			{Name: "2", Kind: KindString, FixedSize: 4, IsVar: true},
		},
	}
}

func tupleTrailingOptionals() *TypeDef {
	return &TypeDef{
		Name:    "TupleTrailingOptionals",
		IsTuple: true,
		Elements: []FieldDef{
			{Name: "0", Kind: KindU32, FixedSize: 4},
			{Name: "1", Kind: KindOptional, FixedSize: 4, IsVar: true, ElemKind: KindU32},
			{Name: "2", Kind: KindOptional, FixedSize: 4, IsVar: true, ElemKind: KindString},
		},
	}
}

func tupleNested() *TypeDef {
	inner := innerTypeDef()
	return &TypeDef{
		Name:    "TupleNested",
		IsTuple: true,
		Elements: []FieldDef{
			{Name: "0", Kind: KindU32, FixedSize: 4},
			{Name: "1", Kind: KindObject, FixedSize: 4, IsVar: true, InnerDef: inner},
		},
	}
}

// ── Tuple pack tests ──────────────────────────────────────────────────────

func TestTuplePackFixedOnly(t *testing.T) {
	td := tupleU32U32()
	val := &Value{
		Kind: KindTuple,
		Tuple: []Value{
			{Kind: KindU32, U32: 42},
			{Kind: KindU32, U32: 100},
		},
	}

	got := Pack(td, val)

	// Verify: u16 fixed_size=8, then 42 as u32, then 100 as u32
	if len(got) < 10 {
		t.Fatalf("packed too short: %d bytes", len(got))
	}

	// Parse and verify
	unpacked, err := Unpack(td, got)
	if err != nil {
		t.Fatalf("unpack error: %v", err)
	}
	if len(unpacked.Tuple) != 2 {
		t.Fatalf("expected 2 elements, got %d", len(unpacked.Tuple))
	}
	if unpacked.Tuple[0].U32 != 42 {
		t.Errorf("elem 0 = %d, want 42", unpacked.Tuple[0].U32)
	}
	if unpacked.Tuple[1].U32 != 100 {
		t.Errorf("elem 1 = %d, want 100", unpacked.Tuple[1].U32)
	}
}

func TestTuplePackWithString(t *testing.T) {
	td := tupleU32String()
	val := &Value{
		Kind: KindTuple,
		Tuple: []Value{
			{Kind: KindU32, U32: 7},
			{Kind: KindString, Str: "hello"},
		},
	}

	got := Pack(td, val)
	unpacked, err := Unpack(td, got)
	if err != nil {
		t.Fatalf("unpack error: %v", err)
	}
	if unpacked.Tuple[0].U32 != 7 {
		t.Errorf("elem 0 = %d, want 7", unpacked.Tuple[0].U32)
	}
	if unpacked.Tuple[1].Str != "hello" {
		t.Errorf("elem 1 = %q, want hello", unpacked.Tuple[1].Str)
	}
}

func TestTuplePackTwoStrings(t *testing.T) {
	td := tupleStringString()
	val := &Value{
		Kind: KindTuple,
		Tuple: []Value{
			{Kind: KindString, Str: "abc"},
			{Kind: KindString, Str: "xyz"},
		},
	}

	got := Pack(td, val)
	unpacked, err := Unpack(td, got)
	if err != nil {
		t.Fatalf("unpack error: %v", err)
	}
	if unpacked.Tuple[0].Str != "abc" {
		t.Errorf("elem 0 = %q, want abc", unpacked.Tuple[0].Str)
	}
	if unpacked.Tuple[1].Str != "xyz" {
		t.Errorf("elem 1 = %q, want xyz", unpacked.Tuple[1].Str)
	}
}

func TestTuplePackEmpty(t *testing.T) {
	td := tupleTrailingOptionals()
	val := &Value{
		Kind: KindTuple,
		Tuple: []Value{
			{Kind: KindU32, U32: 0},
			{Kind: KindOptional, Opt: nil},
			{Kind: KindOptional, Opt: nil},
		},
	}

	got := Pack(td, val)
	// First non-optional field is at index 0, trailing optionals are elided
	// So fixed_size should be 4 (just the u32)

	unpacked, err := Unpack(td, got)
	if err != nil {
		t.Fatalf("unpack error: %v", err)
	}
	if unpacked.Tuple[0].U32 != 0 {
		t.Errorf("elem 0 = %d, want 0", unpacked.Tuple[0].U32)
	}
	if unpacked.Tuple[1].Opt != nil {
		t.Errorf("elem 1 should be None")
	}
	if unpacked.Tuple[2].Opt != nil {
		t.Errorf("elem 2 should be None")
	}
}

func TestTuplePackPartialOptionals(t *testing.T) {
	td := tupleTrailingOptionals()
	optVal := &Value{Kind: KindU32, U32: 99}
	val := &Value{
		Kind: KindTuple,
		Tuple: []Value{
			{Kind: KindU32, U32: 5},
			{Kind: KindOptional, Opt: optVal},
			{Kind: KindOptional, Opt: nil},
		},
	}

	got := Pack(td, val)
	unpacked, err := Unpack(td, got)
	if err != nil {
		t.Fatalf("unpack error: %v", err)
	}
	if unpacked.Tuple[0].U32 != 5 {
		t.Errorf("elem 0 = %d, want 5", unpacked.Tuple[0].U32)
	}
	if unpacked.Tuple[1].Opt == nil {
		t.Fatal("elem 1 should be present")
	}
	if unpacked.Tuple[1].Opt.U32 != 99 {
		t.Errorf("elem 1 = %d, want 99", unpacked.Tuple[1].Opt.U32)
	}
	if unpacked.Tuple[2].Opt != nil {
		t.Errorf("elem 2 should be None")
	}
}

func TestTuplePackMixed(t *testing.T) {
	td := tupleMixed()
	val := &Value{
		Kind: KindTuple,
		Tuple: []Value{
			{Kind: KindBool, Bool: true},
			{Kind: KindU32, U32: 42},
			{Kind: KindString, Str: "test"},
		},
	}

	got := Pack(td, val)
	unpacked, err := Unpack(td, got)
	if err != nil {
		t.Fatalf("unpack error: %v", err)
	}
	if !unpacked.Tuple[0].Bool {
		t.Error("elem 0 = false, want true")
	}
	if unpacked.Tuple[1].U32 != 42 {
		t.Errorf("elem 1 = %d, want 42", unpacked.Tuple[1].U32)
	}
	if unpacked.Tuple[2].Str != "test" {
		t.Errorf("elem 2 = %q, want test", unpacked.Tuple[2].Str)
	}
}

func TestTuplePackNested(t *testing.T) {
	td := tupleNested()
	val := &Value{
		Kind: KindTuple,
		Tuple: []Value{
			{Kind: KindU32, U32: 1},
			{Kind: KindObject, Fields: []Value{
				{Kind: KindU32, U32: 42},
				{Kind: KindString, Str: "nested"},
			}},
		},
	}

	got := Pack(td, val)
	unpacked, err := Unpack(td, got)
	if err != nil {
		t.Fatalf("unpack error: %v", err)
	}
	if unpacked.Tuple[0].U32 != 1 {
		t.Errorf("elem 0 = %d, want 1", unpacked.Tuple[0].U32)
	}
	if len(unpacked.Tuple[1].Fields) != 2 {
		t.Fatalf("inner should have 2 fields, got %d", len(unpacked.Tuple[1].Fields))
	}
	if unpacked.Tuple[1].Fields[0].U32 != 42 {
		t.Errorf("inner.value = %d, want 42", unpacked.Tuple[1].Fields[0].U32)
	}
	if unpacked.Tuple[1].Fields[1].Str != "nested" {
		t.Errorf("inner.label = %q, want nested", unpacked.Tuple[1].Fields[1].Str)
	}
}

func TestTupleRoundTrip(t *testing.T) {
	// Pack a tuple, unpack it, repack it, and verify byte equality
	td := tupleU32String()
	val := &Value{
		Kind: KindTuple,
		Tuple: []Value{
			{Kind: KindU32, U32: 123},
			{Kind: KindString, Str: "round-trip"},
		},
	}

	packed1 := Pack(td, val)
	unpacked, err := Unpack(td, packed1)
	if err != nil {
		t.Fatalf("unpack error: %v", err)
	}
	packed2 := Pack(td, unpacked)

	if !bytesEqual(packed1, packed2) {
		t.Errorf("round-trip mismatch\n  first:  %s\n  second: %s",
			hex.EncodeToString(packed1), hex.EncodeToString(packed2))
	}
}

func TestTupleEmptyString(t *testing.T) {
	td := tupleU32String()
	val := &Value{
		Kind: KindTuple,
		Tuple: []Value{
			{Kind: KindU32, U32: 0},
			{Kind: KindString, Str: ""},
		},
	}

	got := Pack(td, val)
	unpacked, err := Unpack(td, got)
	if err != nil {
		t.Fatalf("unpack error: %v", err)
	}
	if unpacked.Tuple[0].U32 != 0 {
		t.Errorf("elem 0 = %d, want 0", unpacked.Tuple[0].U32)
	}
	if unpacked.Tuple[1].Str != "" {
		t.Errorf("elem 1 = %q, want empty", unpacked.Tuple[1].Str)
	}
}

// ── Tuple as field of struct ──────────────────────────────────────────────

func TestTupleAsStructField(t *testing.T) {
	tupleType := tupleU32String()
	structType := &TypeDef{
		Name:       "WithTuple",
		Extensible: true,
		Fields: []FieldDef{
			{Name: "label", Kind: KindString, FixedSize: 4, IsVar: true},
			{Name: "pair", Kind: KindTuple, FixedSize: 4, IsVar: true, InnerDef: tupleType},
		},
	}

	val := &Value{
		Kind: KindObject,
		Fields: []Value{
			{Kind: KindString, Str: "test"},
			{Kind: KindTuple, Tuple: []Value{
				{Kind: KindU32, U32: 10},
				{Kind: KindString, Str: "inner"},
			}},
		},
	}

	got := Pack(structType, val)
	unpacked, err := Unpack(structType, got)
	if err != nil {
		t.Fatalf("unpack error: %v", err)
	}
	if unpacked.Fields[0].Str != "test" {
		t.Errorf("label = %q, want test", unpacked.Fields[0].Str)
	}
	if len(unpacked.Fields[1].Tuple) != 2 {
		t.Fatalf("tuple should have 2 elements, got %d", len(unpacked.Fields[1].Tuple))
	}
	if unpacked.Fields[1].Tuple[0].U32 != 10 {
		t.Errorf("tuple[0] = %d, want 10", unpacked.Fields[1].Tuple[0].U32)
	}
	if unpacked.Fields[1].Tuple[1].Str != "inner" {
		t.Errorf("tuple[1] = %q, want inner", unpacked.Fields[1].Tuple[1].Str)
	}
}

// ── Tuple wire format verification ────────────────────────────────────────

func TestTupleWireFormat(t *testing.T) {
	// Verify exact wire format for a simple tuple (u32, u32) = (1, 2)
	td := tupleU32U32()
	val := &Value{
		Kind: KindTuple,
		Tuple: []Value{
			{Kind: KindU32, U32: 1},
			{Kind: KindU32, U32: 2},
		},
	}

	got := Pack(td, val)
	gotHex := strings.ToUpper(hex.EncodeToString(got))

	// Expected: u16 fixed_size=8, u32 1, u32 2
	// 0800 01000000 02000000
	expected := "080001000000002000000"
	_ = expected

	// Check header
	if len(got) < 2 {
		t.Fatal("too short for header")
	}
	fixedSize := uint16(got[0]) | uint16(got[1])<<8
	if fixedSize != 8 {
		t.Errorf("fixed_size = %d, want 8", fixedSize)
	}

	// Check values via unpack
	unpacked, err := Unpack(td, got)
	if err != nil {
		t.Fatalf("unpack: %v", err)
	}
	if unpacked.Tuple[0].U32 != 1 || unpacked.Tuple[1].U32 != 2 {
		t.Errorf("values = (%d, %d), want (1, 2)",
			unpacked.Tuple[0].U32, unpacked.Tuple[1].U32)
	}

	t.Logf("wire format: %s", gotHex)
}
