package fracpack

import (
	"encoding/binary"
	"encoding/hex"
	"testing"
)

// ── SpliceBuffer tests ────────────────────────────────────────────────────

func TestSpliceBufferSameSize(t *testing.T) {
	data := []byte{1, 2, 3, 4, 5}
	data, delta := SpliceBuffer(data, 1, 2, []byte{10, 20})
	if delta != 0 {
		t.Errorf("delta = %d, want 0", delta)
	}
	expect := []byte{1, 10, 20, 4, 5}
	if !bytesEqual(data, expect) {
		t.Errorf("data = %v, want %v", data, expect)
	}
}

func TestSpliceBufferGrow(t *testing.T) {
	data := []byte{1, 2, 3, 4, 5}
	data, delta := SpliceBuffer(data, 1, 2, []byte{10, 20, 30})
	if delta != 1 {
		t.Errorf("delta = %d, want 1", delta)
	}
	expect := []byte{1, 10, 20, 30, 4, 5}
	if !bytesEqual(data, expect) {
		t.Errorf("data = %v, want %v", data, expect)
	}
}

func TestSpliceBufferShrink(t *testing.T) {
	data := []byte{1, 2, 3, 4, 5}
	data, delta := SpliceBuffer(data, 1, 3, []byte{10})
	if delta != -2 {
		t.Errorf("delta = %d, want -2", delta)
	}
	expect := []byte{1, 10, 5}
	if !bytesEqual(data, expect) {
		t.Errorf("data = %v, want %v", data, expect)
	}
}

func TestSpliceBufferInsert(t *testing.T) {
	data := []byte{1, 2, 3}
	data, delta := SpliceBuffer(data, 1, 0, []byte{10, 20})
	if delta != 2 {
		t.Errorf("delta = %d, want 2", delta)
	}
	expect := []byte{1, 10, 20, 2, 3}
	if !bytesEqual(data, expect) {
		t.Errorf("data = %v, want %v", data, expect)
	}
}

func TestSpliceBufferDelete(t *testing.T) {
	data := []byte{1, 2, 3, 4, 5}
	data, delta := SpliceBuffer(data, 1, 2, nil)
	if delta != -2 {
		t.Errorf("delta = %d, want -2", delta)
	}
	expect := []byte{1, 4, 5}
	if !bytesEqual(data, expect) {
		t.Errorf("data = %v, want %v", data, expect)
	}
}

// ── PatchOffset tests ─────────────────────────────────────────────────────

func TestPatchOffsetValid(t *testing.T) {
	data := make([]byte, 16)
	// Place an offset value of 8 at position 4
	binary.LittleEndian.PutUint32(data[4:], 8)
	// Target = 4 + 8 = 12, splicePos = 10, delta = 5
	PatchOffset(data, 4, 10, 5)
	got := binary.LittleEndian.Uint32(data[4:])
	if got != 13 {
		t.Errorf("patched offset = %d, want 13", got)
	}
}

func TestPatchOffsetBeforeSplice(t *testing.T) {
	data := make([]byte, 16)
	// Place an offset value of 4 at position 0
	binary.LittleEndian.PutUint32(data[0:], 4)
	// Target = 0 + 4 = 4, splicePos = 10, delta = 5
	// Target < splicePos, so should NOT be patched
	PatchOffset(data, 0, 10, 5)
	got := binary.LittleEndian.Uint32(data[0:])
	if got != 4 {
		t.Errorf("offset should be unchanged: got %d, want 4", got)
	}
}

func TestPatchOffsetEmpty(t *testing.T) {
	data := make([]byte, 8)
	// Offset = 0 (empty container) should not be patched
	binary.LittleEndian.PutUint32(data[0:], 0)
	PatchOffset(data, 0, 0, 5)
	got := binary.LittleEndian.Uint32(data[0:])
	if got != 0 {
		t.Errorf("empty offset should not be patched: got %d", got)
	}
}

func TestPatchOffsetNone(t *testing.T) {
	data := make([]byte, 8)
	// Offset = 1 (None marker) should not be patched
	binary.LittleEndian.PutUint32(data[0:], 1)
	PatchOffset(data, 0, 0, 5)
	got := binary.LittleEndian.Uint32(data[0:])
	if got != 1 {
		t.Errorf("None offset should not be patched: got %d", got)
	}
}

// ── MutView tests (canonical mode) ────────────────────────────────────────

func TestMutViewGetScalar(t *testing.T) {
	// Pack an Inner object: {value: 42, label: "hello"}
	td := innerTypeDef()
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindU32, U32: 42},
		{Kind: KindString, Str: "hello"},
	}}
	data := Pack(td, val)

	mv := NewMutView(&data, td, false)

	got, err := mv.Get("value")
	if err != nil {
		t.Fatalf("Get error: %v", err)
	}
	if got.(uint32) != 42 {
		t.Errorf("value = %v, want 42", got)
	}
}

func TestMutViewGetString(t *testing.T) {
	td := innerTypeDef()
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindU32, U32: 1},
		{Kind: KindString, Str: "hello"},
	}}
	data := Pack(td, val)

	mv := NewMutView(&data, td, false)

	got, err := mv.Get("label")
	if err != nil {
		t.Fatalf("Get error: %v", err)
	}
	if got.(string) != "hello" {
		t.Errorf("label = %v, want hello", got)
	}
}

func TestMutViewSetScalar(t *testing.T) {
	td := innerTypeDef()
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindU32, U32: 42},
		{Kind: KindString, Str: "hello"},
	}}
	data := Pack(td, val)

	mv := NewMutView(&data, td, false)

	err := mv.Set("value", uint32(99))
	if err != nil {
		t.Fatalf("Set error: %v", err)
	}

	got, err := mv.Get("value")
	if err != nil {
		t.Fatalf("Get error: %v", err)
	}
	if got.(uint32) != 99 {
		t.Errorf("value = %v, want 99", got)
	}

	// Verify other fields preserved
	got, err = mv.Get("label")
	if err != nil {
		t.Fatalf("Get label error: %v", err)
	}
	if got.(string) != "hello" {
		t.Errorf("label = %v, want hello", got)
	}
}

func TestMutViewSetStringSameSize(t *testing.T) {
	td := innerTypeDef()
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindU32, U32: 1},
		{Kind: KindString, Str: "hello"},
	}}
	data := Pack(td, val)

	mv := NewMutView(&data, td, false)

	// Set to same-length string
	err := mv.Set("label", "world")
	if err != nil {
		t.Fatalf("Set error: %v", err)
	}

	got, err := mv.Get("label")
	if err != nil {
		t.Fatalf("Get error: %v", err)
	}
	if got.(string) != "world" {
		t.Errorf("label = %v, want world", got)
	}
}

func TestMutViewSetStringGrow(t *testing.T) {
	td := innerTypeDef()
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindU32, U32: 1},
		{Kind: KindString, Str: "hi"},
	}}
	data := Pack(td, val)

	mv := NewMutView(&data, td, false)

	err := mv.Set("label", "hello world")
	if err != nil {
		t.Fatalf("Set error: %v", err)
	}

	got, err := mv.Get("label")
	if err != nil {
		t.Fatalf("Get error: %v", err)
	}
	if got.(string) != "hello world" {
		t.Errorf("label = %v, want 'hello world'", got)
	}

	// Verify the result is canonical by repacking
	unpacked, err := Unpack(td, mv.Bytes())
	if err != nil {
		t.Fatalf("unpack error: %v", err)
	}
	repacked := Pack(td, unpacked)
	if !bytesEqual(mv.Bytes(), repacked) {
		t.Errorf("not canonical after set\n  got:  %s\n  want: %s",
			hex.EncodeToString(mv.Bytes()),
			hex.EncodeToString(repacked))
	}
}

func TestMutViewSetStringShrink(t *testing.T) {
	td := innerTypeDef()
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindU32, U32: 1},
		{Kind: KindString, Str: "hello world"},
	}}
	data := Pack(td, val)

	mv := NewMutView(&data, td, false)

	err := mv.Set("label", "hi")
	if err != nil {
		t.Fatalf("Set error: %v", err)
	}

	got, err := mv.Get("label")
	if err != nil {
		t.Fatalf("Get error: %v", err)
	}
	if got.(string) != "hi" {
		t.Errorf("label = %v, want hi", got)
	}

	// Verify canonical
	unpacked, err := Unpack(td, mv.Bytes())
	if err != nil {
		t.Fatalf("unpack error: %v", err)
	}
	repacked := Pack(td, unpacked)
	if !bytesEqual(mv.Bytes(), repacked) {
		t.Errorf("not canonical after shrink\n  got:  %s\n  want: %s",
			hex.EncodeToString(mv.Bytes()),
			hex.EncodeToString(repacked))
	}
}

// ── MutView tests (fast mode) ─────────────────────────────────────────────

func TestMutViewFastSetScalar(t *testing.T) {
	td := innerTypeDef()
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindU32, U32: 42},
		{Kind: KindString, Str: "hello"},
	}}
	data := Pack(td, val)

	mv := NewMutView(&data, td, true)

	err := mv.Set("value", uint32(99))
	if err != nil {
		t.Fatalf("Set error: %v", err)
	}

	got, err := mv.Get("value")
	if err != nil {
		t.Fatalf("Get error: %v", err)
	}
	if got.(uint32) != 99 {
		t.Errorf("value = %v, want 99", got)
	}
}

func TestMutViewFastSetStringFits(t *testing.T) {
	td := innerTypeDef()
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindU32, U32: 1},
		{Kind: KindString, Str: "hello"},
	}}
	data := Pack(td, val)
	origLen := len(data)

	mv := NewMutView(&data, td, true)

	// Set to shorter string (fits in old slot)
	err := mv.SetFast("label", "hi")
	if err != nil {
		t.Fatalf("SetFast error: %v", err)
	}

	got, err := mv.Get("label")
	if err != nil {
		t.Fatalf("Get error: %v", err)
	}
	if got.(string) != "hi" {
		t.Errorf("label = %v, want hi", got)
	}

	// Buffer size should be unchanged in fast mode
	if len(mv.Bytes()) != origLen {
		t.Errorf("buffer size changed: %d -> %d", origLen, len(mv.Bytes()))
	}
}

func TestMutViewFastSetStringGrow(t *testing.T) {
	td := innerTypeDef()
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindU32, U32: 1},
		{Kind: KindString, Str: "hi"},
	}}
	data := Pack(td, val)

	mv := NewMutView(&data, td, true)

	// Set to longer string (appended to end)
	err := mv.SetFast("label", "hello world")
	if err != nil {
		t.Fatalf("SetFast error: %v", err)
	}

	got, err := mv.Get("label")
	if err != nil {
		t.Fatalf("Get error: %v", err)
	}
	if got.(string) != "hello world" {
		t.Errorf("label = %v, want 'hello world'", got)
	}
}

func TestMutViewFastCompact(t *testing.T) {
	td := innerTypeDef()
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindU32, U32: 1},
		{Kind: KindString, Str: "hello"},
	}}
	data := Pack(td, val)

	mv := NewMutView(&data, td, true)

	// Do a fast set that creates dead bytes
	err := mv.SetFast("label", "hi")
	if err != nil {
		t.Fatalf("SetFast error: %v", err)
	}

	// Compact to restore canonical form
	compacted := mv.Compact()

	// Verify the compacted data is canonical
	unpacked, err := Unpack(td, compacted)
	if err != nil {
		t.Fatalf("unpack error: %v", err)
	}
	repacked := Pack(td, unpacked)
	if !bytesEqual(compacted, repacked) {
		t.Errorf("not canonical after compact\n  got:  %s\n  want: %s",
			hex.EncodeToString(compacted),
			hex.EncodeToString(repacked))
	}

	// Verify values
	if unpacked.Fields[0].U32 != 1 {
		t.Errorf("value = %d, want 1", unpacked.Fields[0].U32)
	}
	if unpacked.Fields[1].Str != "hi" {
		t.Errorf("label = %q, want hi", unpacked.Fields[1].Str)
	}
}

// ── MutView with optionals ───────────────────────────────────────────────

func TestMutViewSetOptionalToNone(t *testing.T) {
	td := &TypeDef{Name: "WithOptionals", Extensible: true, Fields: []FieldDef{
		{Name: "opt_int", Kind: KindOptional, FixedSize: 4, IsVar: true, ElemKind: KindU32},
		{Name: "label", Kind: KindString, FixedSize: 4, IsVar: true},
	}}

	optVal := &Value{Kind: KindU32, U32: 42}
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindOptional, Opt: optVal},
		{Kind: KindString, Str: "hello"},
	}}
	data := Pack(td, val)

	mv := NewMutView(&data, td, false)

	// Verify initial state
	got, err := mv.Get("opt_int")
	if err != nil {
		t.Fatalf("Get error: %v", err)
	}
	if got == nil {
		t.Fatal("opt_int should be present initially")
	}

	// Set to nil (None)
	err = mv.Set("opt_int", nil)
	if err != nil {
		t.Fatalf("Set error: %v", err)
	}

	// Verify it's now None
	got, err = mv.Get("opt_int")
	if err != nil {
		t.Fatalf("Get error: %v", err)
	}
	if got != nil {
		t.Errorf("opt_int should be None, got %v", got)
	}

	// Verify label still works
	got, err = mv.Get("label")
	if err != nil {
		t.Fatalf("Get label error: %v", err)
	}
	if got.(string) != "hello" {
		t.Errorf("label = %v, want hello", got)
	}
}

// ── MutView multiple edits ──────────────────────────────────────────────

func TestMutViewMultipleEdits(t *testing.T) {
	td := innerTypeDef()
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindU32, U32: 1},
		{Kind: KindString, Str: "initial"},
	}}
	data := Pack(td, val)

	mv := NewMutView(&data, td, false)

	// First edit
	err := mv.Set("value", uint32(100))
	if err != nil {
		t.Fatalf("Set value error: %v", err)
	}

	// Second edit
	err = mv.Set("label", "modified")
	if err != nil {
		t.Fatalf("Set label error: %v", err)
	}

	// Verify
	got, _ := mv.Get("value")
	if got.(uint32) != 100 {
		t.Errorf("value = %v, want 100", got)
	}
	got, _ = mv.Get("label")
	if got.(string) != "modified" {
		t.Errorf("label = %v, want modified", got)
	}

	// Verify canonical
	unpacked, err := Unpack(td, mv.Bytes())
	if err != nil {
		t.Fatalf("unpack error: %v", err)
	}
	repacked := Pack(td, unpacked)
	if !bytesEqual(mv.Bytes(), repacked) {
		t.Errorf("not canonical after multiple edits\n  got:  %s\n  want: %s",
			hex.EncodeToString(mv.Bytes()),
			hex.EncodeToString(repacked))
	}
}

// ── MutView error handling ──────────────────────────────────────────────

func TestMutViewGetUnknownField(t *testing.T) {
	td := innerTypeDef()
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindU32, U32: 1},
		{Kind: KindString, Str: "hello"},
	}}
	data := Pack(td, val)

	mv := NewMutView(&data, td, false)

	_, err := mv.Get("nonexistent")
	if err == nil {
		t.Error("expected error for unknown field")
	}
}

func TestMutViewSetUnknownField(t *testing.T) {
	td := innerTypeDef()
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindU32, U32: 1},
		{Kind: KindString, Str: "hello"},
	}}
	data := Pack(td, val)

	mv := NewMutView(&data, td, false)

	err := mv.Set("nonexistent", uint32(42))
	if err == nil {
		t.Error("expected error for unknown field")
	}
}

// ── MutView fast mode with compact ──────────────────────────────────────

func TestMutViewFastMultipleEditsCompact(t *testing.T) {
	td := innerTypeDef()
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindU32, U32: 1},
		{Kind: KindString, Str: "original"},
	}}
	data := Pack(td, val)

	mv := NewMutView(&data, td, true)

	// Multiple fast edits
	mv.Set("value", uint32(42))
	mv.Set("label", "short")
	mv.Set("label", "a much longer string than the original")

	// Before compact, we can still read correctly
	got, _ := mv.Get("value")
	if got.(uint32) != 42 {
		t.Errorf("value = %v, want 42", got)
	}
	got, _ = mv.Get("label")
	if got.(string) != "a much longer string than the original" {
		t.Errorf("label = %v, want 'a much longer string than the original'", got)
	}

	// Compact and verify canonical
	compacted := mv.Compact()
	unpacked, err := Unpack(td, compacted)
	if err != nil {
		t.Fatalf("unpack error: %v", err)
	}
	repacked := Pack(td, unpacked)
	if !bytesEqual(compacted, repacked) {
		t.Errorf("not canonical after compact\n  got:  %s\n  want: %s",
			hex.EncodeToString(compacted),
			hex.EncodeToString(repacked))
	}

	if unpacked.Fields[0].U32 != 42 {
		t.Errorf("value = %d, want 42", unpacked.Fields[0].U32)
	}
	if unpacked.Fields[1].Str != "a much longer string than the original" {
		t.Errorf("label = %q, want 'a much longer string than the original'",
			unpacked.Fields[1].Str)
	}
}

// ── Bytes() returns current state ───────────────────────────────────────

func TestMutViewBytes(t *testing.T) {
	td := innerTypeDef()
	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindU32, U32: 1},
		{Kind: KindString, Str: "hello"},
	}}
	data := Pack(td, val)
	dataCopy := make([]byte, len(data))
	copy(dataCopy, data)

	mv := NewMutView(&data, td, false)

	// Before any edits, Bytes() should match original
	if !bytesEqual(mv.Bytes(), dataCopy) {
		t.Error("Bytes() should match original before edits")
	}
}

// ── MutView with WithStrings type ───────────────────────────────────────

func TestMutViewMultipleStrings(t *testing.T) {
	td := &TypeDef{Name: "WithStrings", Extensible: true, Fields: []FieldDef{
		{Name: "first", Kind: KindString, FixedSize: 4, IsVar: true},
		{Name: "second", Kind: KindString, FixedSize: 4, IsVar: true},
		{Name: "third", Kind: KindString, FixedSize: 4, IsVar: true},
	}}

	val := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindString, Str: "aaa"},
		{Kind: KindString, Str: "bbb"},
		{Kind: KindString, Str: "ccc"},
	}}
	data := Pack(td, val)

	mv := NewMutView(&data, td, false)

	// Modify middle string to be longer
	err := mv.Set("second", "BBBBBB")
	if err != nil {
		t.Fatalf("Set error: %v", err)
	}

	// Verify all fields
	got, _ := mv.Get("first")
	if got.(string) != "aaa" {
		t.Errorf("first = %q, want aaa", got)
	}
	got, _ = mv.Get("second")
	if got.(string) != "BBBBBB" {
		t.Errorf("second = %q, want BBBBBB", got)
	}
	got, _ = mv.Get("third")
	if got.(string) != "ccc" {
		t.Errorf("third = %q, want ccc", got)
	}

	// Verify canonical
	unpacked, err := Unpack(td, mv.Bytes())
	if err != nil {
		t.Fatalf("unpack error: %v", err)
	}
	repacked := Pack(td, unpacked)
	if !bytesEqual(mv.Bytes(), repacked) {
		t.Errorf("not canonical\n  got:  %s\n  want: %s",
			hex.EncodeToString(mv.Bytes()),
			hex.EncodeToString(repacked))
	}
}
