package fracpack

import (
	"encoding/hex"
	"encoding/json"
	"os"
	"strings"
	"testing"
)

// ── Test vector types ────────────────────────────────────────────────────

type testVectors struct {
	FormatVersion int        `json:"format_version"`
	Types         []testType `json:"types"`
}

type testType struct {
	Name     string     `json:"name"`
	RootType string     `json:"root_type"`
	Cases    []testCase `json:"cases"`
}

type testCase struct {
	Name      string          `json:"name"`
	PackedHex string          `json:"packed_hex"`
	JSON      json.RawMessage `json:"json"`
}

// ── Type definition builders ─────────────────────────────────────────────

func buildTypeDef(name string) *TypeDef {
	switch name {
	case "FixedInts":
		return &TypeDef{Name: "FixedInts", Fields: []FieldDef{
			{Name: "x", Kind: KindI32, FixedSize: 4},
			{Name: "y", Kind: KindI32, FixedSize: 4},
		}}
	case "FixedMixed":
		return &TypeDef{Name: "FixedMixed", Fields: []FieldDef{
			{Name: "b", Kind: KindBool, FixedSize: 1},
			{Name: "u8", Kind: KindU8, FixedSize: 1},
			{Name: "u16", Kind: KindU16, FixedSize: 2},
			{Name: "u32", Kind: KindU32, FixedSize: 4},
			{Name: "u64", Kind: KindU64, FixedSize: 8},
		}}
	case "AllPrimitives":
		return &TypeDef{Name: "AllPrimitives", Extensible: true, Fields: []FieldDef{
			{Name: "b", Kind: KindBool, FixedSize: 1},
			{Name: "u8v", Kind: KindU8, FixedSize: 1},
			{Name: "i8v", Kind: KindI8, FixedSize: 1},
			{Name: "u16v", Kind: KindU16, FixedSize: 2},
			{Name: "i16v", Kind: KindI16, FixedSize: 2},
			{Name: "u32v", Kind: KindU32, FixedSize: 4},
			{Name: "i32v", Kind: KindI32, FixedSize: 4},
			{Name: "u64v", Kind: KindU64, FixedSize: 8},
			{Name: "i64v", Kind: KindI64, FixedSize: 8},
			{Name: "f32v", Kind: KindF32, FixedSize: 4},
			{Name: "f64v", Kind: KindF64, FixedSize: 8},
		}}
	case "SingleBool":
		return &TypeDef{Name: "SingleBool", Fields: []FieldDef{
			{Name: "value", Kind: KindBool, FixedSize: 1},
		}}
	case "SingleU32":
		return &TypeDef{Name: "SingleU32", Fields: []FieldDef{
			{Name: "value", Kind: KindU32, FixedSize: 4},
		}}
	case "SingleString":
		return &TypeDef{Name: "SingleString", Extensible: true, Fields: []FieldDef{
			{Name: "value", Kind: KindString, FixedSize: 4, IsVar: true},
		}}
	case "WithStrings":
		return &TypeDef{Name: "WithStrings", Extensible: true, Fields: []FieldDef{
			{Name: "empty_str", Kind: KindString, FixedSize: 4, IsVar: true},
			{Name: "hello", Kind: KindString, FixedSize: 4, IsVar: true},
			{Name: "unicode", Kind: KindString, FixedSize: 4, IsVar: true},
		}}
	case "WithVectors":
		return &TypeDef{Name: "WithVectors", Extensible: true, Fields: []FieldDef{
			{Name: "ints", Kind: KindVec, FixedSize: 4, IsVar: true, ElemKind: KindU32},
			{Name: "strings", Kind: KindVec, FixedSize: 4, IsVar: true, ElemKind: KindString},
		}}
	case "WithOptionals":
		return &TypeDef{Name: "WithOptionals", Extensible: true, Fields: []FieldDef{
			{Name: "opt_int", Kind: KindOptional, FixedSize: 4, IsVar: true, ElemKind: KindU32},
			{Name: "opt_str", Kind: KindOptional, FixedSize: 4, IsVar: true, ElemKind: KindString},
		}}
	case "Inner":
		return innerTypeDef()
	case "Outer":
		inner := innerTypeDef()
		return &TypeDef{Name: "Outer", Extensible: true, Fields: []FieldDef{
			{Name: "inner", Kind: KindObject, FixedSize: 4, IsVar: true, InnerDef: inner},
			{Name: "name", Kind: KindString, FixedSize: 4, IsVar: true},
		}}
	case "FixedArray":
		return &TypeDef{Name: "FixedArray", Fields: []FieldDef{
			{Name: "arr[0]", Kind: KindU32, FixedSize: 4, JSONArray: "arr", JSONIndex: 0},
			{Name: "arr[1]", Kind: KindU32, FixedSize: 4, JSONArray: "arr", JSONIndex: 1},
			{Name: "arr[2]", Kind: KindU32, FixedSize: 4, JSONArray: "arr", JSONIndex: 2},
		}}
	case "WithVariant":
		inner := innerTypeDef()
		return &TypeDef{Name: "WithVariant", Extensible: true, Fields: []FieldDef{
			{Name: "data", Kind: KindVariant, FixedSize: 4, IsVar: true,
				InnerDef: &TypeDef{Name: "DataVariant", IsVariant: true, Cases: []FieldDef{
					{Name: "uint32", Kind: KindU32, FixedSize: 4},
					{Name: "string", Kind: KindString, FixedSize: 4, IsVar: true},
					{Name: "Inner", Kind: KindObject, FixedSize: 4, IsVar: true, InnerDef: inner},
				}}},
		}}
	case "VecOfStructs":
		inner := innerTypeDef()
		return &TypeDef{Name: "VecOfStructs", Extensible: true, Fields: []FieldDef{
			{Name: "items", Kind: KindVec, FixedSize: 4, IsVar: true, ElemKind: KindObject, ElemDef: inner},
		}}
	case "OptionalStruct":
		inner := innerTypeDef()
		return &TypeDef{Name: "OptionalStruct", Extensible: true, Fields: []FieldDef{
			{Name: "item", Kind: KindOptional, FixedSize: 4, IsVar: true, ElemKind: KindObject, ElemDef: inner},
		}}
	case "VecOfOptionals":
		return &TypeDef{Name: "VecOfOptionals", Extensible: true, Fields: []FieldDef{
			{Name: "items", Kind: KindVec, FixedSize: 4, IsVar: true, ElemKind: KindOptional},
		}}
	case "OptionalVec":
		return &TypeDef{Name: "OptionalVec", Extensible: true, Fields: []FieldDef{
			{Name: "items", Kind: KindOptional, FixedSize: 4, IsVar: true, ElemKind: KindVec},
		}}
	case "NestedVecs":
		return &TypeDef{Name: "NestedVecs", Extensible: true, Fields: []FieldDef{
			{Name: "matrix", Kind: KindVec, FixedSize: 4, IsVar: true, ElemKind: KindVec},
		}}
	case "Complex":
		inner := innerTypeDef()
		return &TypeDef{Name: "Complex", Extensible: true, Fields: []FieldDef{
			{Name: "items", Kind: KindVec, FixedSize: 4, IsVar: true, ElemKind: KindObject, ElemDef: inner},
			{Name: "opt_vec", Kind: KindOptional, FixedSize: 4, IsVar: true, ElemKind: KindVec},
			{Name: "vec_opt", Kind: KindVec, FixedSize: 4, IsVar: true, ElemKind: KindOptional, OptInnerKind: KindString},
			{Name: "opt_struct", Kind: KindOptional, FixedSize: 4, IsVar: true, ElemKind: KindObject, ElemDef: inner},
		}}
	case "EmptyExtensible":
		return &TypeDef{Name: "EmptyExtensible", Extensible: true, Fields: []FieldDef{
			{Name: "dummy", Kind: KindU32, FixedSize: 4},
		}}
	default:
		return nil
	}
}

func innerTypeDef() *TypeDef {
	return &TypeDef{Name: "Inner", Extensible: true, Fields: []FieldDef{
		{Name: "value", Kind: KindU32, FixedSize: 4},
		{Name: "label", Kind: KindString, FixedSize: 4, IsVar: true},
	}}
}

// ── JSON → Value conversion ──────────────────────────────────────────────

func jsonToValue(td *TypeDef, raw json.RawMessage) (*Value, error) {
	var obj map[string]json.RawMessage
	if err := json.Unmarshal(raw, &obj); err != nil {
		return nil, err
	}

	// Pre-parse any arrays referenced by JSONArray fields
	arrays := map[string][]json.RawMessage{}
	for _, fd := range td.Fields {
		if fd.JSONArray != "" {
			if _, ok := arrays[fd.JSONArray]; !ok {
				if arrJSON, ok := obj[fd.JSONArray]; ok {
					var arr []json.RawMessage
					json.Unmarshal(arrJSON, &arr)
					arrays[fd.JSONArray] = arr
				}
			}
		}
	}

	val := &Value{Kind: KindObject}
	for _, fd := range td.Fields {
		// Handle array-mapped fields
		if fd.JSONArray != "" {
			arr := arrays[fd.JSONArray]
			if fd.JSONIndex < len(arr) {
				fv, err := jsonToFieldValue(&fd, arr[fd.JSONIndex])
				if err != nil {
					return nil, err
				}
				val.Fields = append(val.Fields, *fv)
			} else {
				val.Fields = append(val.Fields, zeroValue(fd.Kind))
			}
			continue
		}

		fieldJSON, ok := obj[fd.Name]
		if !ok {
			val.Fields = append(val.Fields, zeroValue(fd.Kind))
			continue
		}
		fv, err := jsonToFieldValue(&fd, fieldJSON)
		if err != nil {
			return nil, err
		}
		val.Fields = append(val.Fields, *fv)
	}
	return val, nil
}

func jsonToFieldValue(fd *FieldDef, raw json.RawMessage) (*Value, error) {
	s := strings.TrimSpace(string(raw))

	switch fd.Kind {
	case KindBool:
		v := &Value{Kind: KindBool}
		v.Bool = s == "true"
		return v, nil

	case KindU8:
		v := &Value{Kind: KindU8}
		var n float64
		json.Unmarshal(raw, &n)
		v.U8 = uint8(n)
		return v, nil

	case KindI8:
		v := &Value{Kind: KindI8}
		var n float64
		json.Unmarshal(raw, &n)
		v.I8 = int8(n)
		return v, nil

	case KindU16:
		v := &Value{Kind: KindU16}
		var n float64
		json.Unmarshal(raw, &n)
		v.U16 = uint16(n)
		return v, nil

	case KindI16:
		v := &Value{Kind: KindI16}
		var n float64
		json.Unmarshal(raw, &n)
		v.I16 = int16(n)
		return v, nil

	case KindU32:
		v := &Value{Kind: KindU32}
		var n float64
		json.Unmarshal(raw, &n)
		v.U32 = uint32(n)
		return v, nil

	case KindI32:
		v := &Value{Kind: KindI32}
		var n float64
		json.Unmarshal(raw, &n)
		v.I32 = int32(n)
		return v, nil

	case KindU64:
		v := &Value{Kind: KindU64}
		// u64 might be a string in JSON
		if s[0] == '"' {
			var str string
			json.Unmarshal(raw, &str)
			var n uint64
			for _, c := range str {
				n = n*10 + uint64(c-'0')
			}
			v.U64 = n
		} else {
			var n float64
			json.Unmarshal(raw, &n)
			v.U64 = uint64(n)
		}
		return v, nil

	case KindI64:
		v := &Value{Kind: KindI64}
		if s[0] == '"' {
			var str string
			json.Unmarshal(raw, &str)
			negative := false
			if str[0] == '-' {
				negative = true
				str = str[1:]
			}
			var n uint64
			for _, c := range str {
				n = n*10 + uint64(c-'0')
			}
			if negative {
				v.I64 = -int64(n)
			} else {
				v.I64 = int64(n)
			}
		} else {
			var n float64
			json.Unmarshal(raw, &n)
			v.I64 = int64(n)
		}
		return v, nil

	case KindF32:
		v := &Value{Kind: KindF32}
		var n float64
		json.Unmarshal(raw, &n)
		v.F32 = float32(n)
		return v, nil

	case KindF64:
		v := &Value{Kind: KindF64}
		json.Unmarshal(raw, &v.F64)
		return v, nil

	case KindString:
		v := &Value{Kind: KindString}
		json.Unmarshal(raw, &v.Str)
		return v, nil

	case KindOptional:
		if s == "null" {
			return &Value{Kind: KindOptional, Opt: nil}, nil
		}
		inner := &Value{}
		if fd.ElemKind == KindString {
			inner.Kind = KindString
			json.Unmarshal(raw, &inner.Str)
		} else if fd.ElemKind == KindObject && fd.ElemDef != nil {
			obj, err := jsonToValue(fd.ElemDef, raw)
			if err != nil {
				return nil, err
			}
			inner = obj
		} else if fd.ElemKind == KindVec {
			// optional vec
			innerFd := FieldDef{Kind: KindVec, ElemKind: KindU32}
			ev, err := jsonToFieldValue(&innerFd, raw)
			if err != nil {
				return nil, err
			}
			inner = ev
		} else {
			inner.Kind = KindU32
			var n float64
			json.Unmarshal(raw, &n)
			inner.U32 = uint32(n)
		}
		return &Value{Kind: KindOptional, Opt: inner}, nil

	case KindObject:
		return jsonToValue(fd.InnerDef, raw)

	case KindVec:
		v := &Value{Kind: KindVec}
		var arr []json.RawMessage
		json.Unmarshal(raw, &arr)
		for _, elem := range arr {
			if fd.ElemDef != nil && len(fd.ElemDef.Fields) > 0 {
				ev, err := jsonToValue(fd.ElemDef, elem)
				if err != nil {
					return nil, err
				}
				v.Vec = append(v.Vec, *ev)
			} else if fd.ElemKind == KindString {
				ev := &Value{Kind: KindString}
				json.Unmarshal(elem, &ev.Str)
				v.Vec = append(v.Vec, *ev)
			} else if fd.ElemKind == KindOptional {
				es := strings.TrimSpace(string(elem))
				if es == "null" {
					v.Vec = append(v.Vec, Value{Kind: KindOptional, Opt: nil})
				} else if es[0] == '"' {
					inner := &Value{Kind: KindString}
					json.Unmarshal(elem, &inner.Str)
					v.Vec = append(v.Vec, Value{Kind: KindOptional, Opt: inner})
				} else {
					inner := &Value{Kind: KindU32}
					var n float64
					json.Unmarshal(elem, &n)
					inner.U32 = uint32(n)
					v.Vec = append(v.Vec, Value{Kind: KindOptional, Opt: inner})
				}
			} else if fd.ElemKind == KindVec {
				// Nested vec (e.g., matrix)
				innerFd := FieldDef{Kind: KindVec, ElemKind: KindU32}
				ev, err := jsonToFieldValue(&innerFd, elem)
				if err != nil {
					return nil, err
				}
				v.Vec = append(v.Vec, *ev)
			} else {
				// Primitive element
				ev := &Value{Kind: KindU32}
				var n float64
				json.Unmarshal(elem, &n)
				ev.U32 = uint32(n)
				v.Vec = append(v.Vec, *ev)
			}
		}
		return v, nil

	case KindVariant:
		// Variant JSON: {"caseName": payload}
		var obj map[string]json.RawMessage
		json.Unmarshal(raw, &obj)
		for caseName, payload := range obj {
			// Find matching case
			for i, c := range fd.InnerDef.Cases {
				if c.Name == caseName {
					val := &Value{Kind: KindVariant, Tag: uint8(i)}
					if c.InnerDef != nil {
						inner, err := jsonToValue(c.InnerDef, payload)
						if err != nil {
							return nil, err
						}
						val.Variant = inner
					} else if c.Kind == KindString {
						inner := &Value{Kind: KindString}
						json.Unmarshal(payload, &inner.Str)
						val.Variant = inner
					} else {
						inner := &Value{Kind: c.Kind}
						var n float64
						json.Unmarshal(payload, &n)
						inner.U32 = uint32(n)
						val.Variant = inner
					}
					return val, nil
				}
			}
		}
		return &Value{Kind: KindVariant}, nil

	default:
		return &Value{Kind: fd.Kind}, nil
	}
}

// ── Pack tests ───────────────────────────────────────────────────────────

func TestPackGolden(t *testing.T) {
	vectors := loadVectors(t)

	for _, tt := range vectors.Types {
		td := buildTypeDef(tt.Name)
		if td == nil {
			t.Logf("Skipping %s (no type definition)", tt.Name)
			continue
		}

		for _, tc := range tt.Cases {
			t.Run(tt.Name+"/"+tc.Name, func(t *testing.T) {
				expected, err := hex.DecodeString(tc.PackedHex)
				if err != nil {
					t.Fatalf("bad hex: %v", err)
				}

				val, err := jsonToValue(td, tc.JSON)
				if err != nil {
					t.Fatalf("json parse: %v", err)
				}

				got := Pack(td, val)
				if !bytesEqual(got, expected) {
					t.Errorf("pack mismatch\n  got:  %s\n  want: %s",
						hex.EncodeToString(got), tc.PackedHex)
				}
			})
		}
	}
}

// ── View tests ───────────────────────────────────────────────────────────

func TestViewScalarFields(t *testing.T) {
	// FixedInts: x=42, y=100
	data, _ := hex.DecodeString("2A00000064000000")
	td := buildTypeDef("FixedInts")
	view := NewView(td, data)

	if got := view.Field("x").I32(); got != 42 {
		t.Errorf("x = %d, want 42", got)
	}
	if got := view.Field("y").I32(); got != 100 {
		t.Errorf("y = %d, want 100", got)
	}
}

func TestViewStringFields(t *testing.T) {
	// WithStrings: mixed case
	data, _ := hex.DecodeString("0C0000000000080000000D0000000500000068656C6C6F11000000C3A96D6F6A69733A20F09F8E89F09F9A80")
	td := buildTypeDef("WithStrings")
	view := NewView(td, data)

	if got := view.Field("empty_str").String(); got != "" {
		t.Errorf("empty_str = %q, want empty", got)
	}
	if got := view.Field("hello").String(); got != "hello" {
		t.Errorf("hello = %q, want hello", got)
	}
	if got := view.Field("unicode").String(); got != "émojis: 🎉🚀" {
		t.Errorf("unicode = %q, want émojis: 🎉🚀", got)
	}
}

func TestViewNested(t *testing.T) {
	// Outer: simple case
	data, _ := hex.DecodeString("080008000000170000000800010000000400000005000000696E6E6572050000006F75746572")
	td := buildTypeDef("Outer")
	view := NewView(td, data)

	inner := view.Field("inner").Object()
	if inner == nil {
		t.Fatal("inner is nil")
	}
	if got := inner.Field("value").U32(); got != 1 {
		t.Errorf("inner.value = %d, want 1", got)
	}
	if got := inner.Field("label").String(); got != "inner" {
		t.Errorf("inner.label = %q, want inner", got)
	}
	if got := view.Field("name").String(); got != "outer" {
		t.Errorf("name = %q, want outer", got)
	}
}

func TestViewAllPrimitives(t *testing.T) {
	// AllPrimitives: ones case
	data, _ := hex.DecodeString("2B00010101010001000100000001000000010000000000000001000000000000000000803F000000000000F03F")
	td := buildTypeDef("AllPrimitives")
	view := NewView(td, data)

	if got := view.Field("b").Bool(); got != true {
		t.Errorf("b = %v, want true", got)
	}
	if got := view.Field("u8v").U8(); got != 1 {
		t.Errorf("u8v = %d, want 1", got)
	}
	if got := view.Field("i8v").I8(); got != 1 {
		t.Errorf("i8v = %d, want 1", got)
	}
	if got := view.Field("u16v").U16(); got != 1 {
		t.Errorf("u16v = %d, want 1", got)
	}
	if got := view.Field("u32v").U32(); got != 1 {
		t.Errorf("u32v = %d, want 1", got)
	}
	if got := view.Field("f32v").F32(); got != 1.0 {
		t.Errorf("f32v = %f, want 1.0", got)
	}
	if got := view.Field("f64v").F64(); got != 1.0 {
		t.Errorf("f64v = %f, want 1.0", got)
	}
}

// ── Helpers ──────────────────────────────────────────────────────────────

func loadVectors(t *testing.T) testVectors {
	t.Helper()
	data, err := os.ReadFile("../../test_vectors/vectors.json")
	if err != nil {
		t.Fatalf("cannot read vectors.json: %v", err)
	}
	var v testVectors
	if err := json.Unmarshal(data, &v); err != nil {
		t.Fatalf("cannot parse vectors.json: %v", err)
	}
	return v
}

func bytesEqual(a, b []byte) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}
