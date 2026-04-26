// Schema-driven canonical JSON conversion for fracpack values.
//
// Go's encoding/json marshals uint64/int64 as JSON numbers, silently overflowing
// JavaScript's Number.MAX_SAFE_INTEGER (2^53-1). Byte slices become base64 strings
// (not hex). There is no built-in union/variant type or canonical enum encoding.
// Custom MarshalJSON/UnmarshalJSON per type is tedious and error-prone. This module
// provides schema-driven canonical JSON matching all fracpack implementations.
//
// Canonical format rules (matching JS/Rust):
//   - u64/i64 -> JSON string (e.g., "12345678901234")
//   - u8-u32, i8-i32 -> JSON number
//   - f32/f64 -> JSON number; NaN/Inf -> null
//   - bool -> true/false
//   - string -> JSON string
//   - []byte (bytes) -> lowercase hex string
//   - nil (optional None) -> null
//   - optional Some -> recurse on inner
//   - variant -> {"CaseName": value} (single-key object)
//   - []T (vec) -> JSON array
//   - [N]T (array) -> JSON array
//   - tuple -> JSON array (use td.Elements)
//   - struct -> JSON object with field names as keys
package fracpack

import (
	"bytes"
	"encoding/json"
	"fmt"
	"math"
	"strconv"
)

// ToJSON converts a fracpack Value to canonical JSON bytes.
// The value should be the result of Unpack() or a manually constructed value tree.
func ToJSON(td *TypeDef, value *Value) ([]byte, error) {
	var buf bytes.Buffer
	if err := writeJSON(&buf, td, value); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

// FromJSON parses canonical JSON and returns a Value tree suitable for Pack().
func FromJSON(td *TypeDef, jsonData []byte) (*Value, error) {
	var raw interface{}
	d := json.NewDecoder(bytes.NewReader(jsonData))
	d.UseNumber()
	if err := d.Decode(&raw); err != nil {
		return nil, fmt.Errorf("fracpack: invalid JSON: %w", err)
	}
	return fromJSONValue(td, raw)
}

// ToJSONFromPacked converts packed fracpack bytes directly to canonical JSON.
func ToJSONFromPacked(td *TypeDef, packed []byte) ([]byte, error) {
	val, err := Unpack(td, packed)
	if err != nil {
		return nil, err
	}
	return ToJSON(td, val)
}

// ── ToJSON implementation ─────────────────────────────────────────────────

// writeJSON writes the canonical JSON representation of value to buf.
func writeJSON(buf *bytes.Buffer, td *TypeDef, value *Value) error {
	if td.IsVariant {
		return writeJSONVariant(buf, td, value)
	}
	if td.IsTuple {
		return writeJSONTuple(buf, td, value)
	}
	return writeJSONStruct(buf, td, value)
}

// writeJSONField writes the JSON for a single field value based on its FieldDef.
func writeJSONField(buf *bytes.Buffer, fd *FieldDef, value *Value) error {
	switch fd.Kind {
	case KindBool:
		return writeJSONScalar(buf, fd.Kind, value)
	case KindU8, KindI8, KindU16, KindI16, KindU32, KindI32:
		return writeJSONScalar(buf, fd.Kind, value)
	case KindU64, KindI64:
		return writeJSONScalar(buf, fd.Kind, value)
	case KindF32, KindF64:
		return writeJSONScalar(buf, fd.Kind, value)
	case KindString:
		return writeJSONString(buf, value.Str)
	case KindVec:
		return writeJSONVec(buf, fd, value)
	case KindOptional:
		return writeJSONOptional(buf, fd, value)
	case KindObject:
		if fd.InnerDef != nil {
			return writeJSON(buf, fd.InnerDef, value)
		}
		return fmt.Errorf("fracpack: object field %q has no type definition", fd.Name)
	case KindVariant:
		if fd.InnerDef != nil {
			return writeJSONVariant(buf, fd.InnerDef, value)
		}
		return fmt.Errorf("fracpack: variant field %q has no type definition", fd.Name)
	case KindTuple:
		if fd.InnerDef != nil {
			return writeJSONTuple(buf, fd.InnerDef, value)
		}
		return fmt.Errorf("fracpack: tuple field %q has no type definition", fd.Name)
	default:
		return fmt.Errorf("fracpack: unknown field kind %d", fd.Kind)
	}
}

// writeJSONScalar writes a scalar value as canonical JSON.
func writeJSONScalar(buf *bytes.Buffer, kind FieldKind, value *Value) error {
	switch kind {
	case KindBool:
		if value.Bool {
			buf.WriteString("true")
		} else {
			buf.WriteString("false")
		}
	case KindU8:
		buf.WriteString(strconv.FormatUint(uint64(value.U8), 10))
	case KindI8:
		buf.WriteString(strconv.FormatInt(int64(value.I8), 10))
	case KindU16:
		buf.WriteString(strconv.FormatUint(uint64(value.U16), 10))
	case KindI16:
		buf.WriteString(strconv.FormatInt(int64(value.I16), 10))
	case KindU32:
		buf.WriteString(strconv.FormatUint(uint64(value.U32), 10))
	case KindI32:
		buf.WriteString(strconv.FormatInt(int64(value.I32), 10))
	case KindU64:
		// u64 -> JSON string
		buf.WriteByte('"')
		buf.WriteString(strconv.FormatUint(value.U64, 10))
		buf.WriteByte('"')
	case KindI64:
		// i64 -> JSON string
		buf.WriteByte('"')
		buf.WriteString(strconv.FormatInt(value.I64, 10))
		buf.WriteByte('"')
	case KindF32:
		f := float64(value.F32)
		if math.IsNaN(f) || math.IsInf(f, 0) {
			buf.WriteString("null")
		} else {
			// Use 64-bit formatting to preserve the exact f32->f64 representation
			// (matching JS behavior where all numbers are f64).
			buf.WriteString(strconv.FormatFloat(f, 'f', -1, 64))
		}
	case KindF64:
		if math.IsNaN(value.F64) || math.IsInf(value.F64, 0) {
			buf.WriteString("null")
		} else {
			buf.WriteString(strconv.FormatFloat(value.F64, 'f', -1, 64))
		}
	default:
		return fmt.Errorf("fracpack: unknown scalar kind %d", kind)
	}
	return nil
}

// writeJSONString writes a properly escaped JSON string.
func writeJSONString(buf *bytes.Buffer, s string) error {
	// Use json.Marshal for correct escaping of special chars
	escaped, err := json.Marshal(s)
	if err != nil {
		return err
	}
	buf.Write(escaped)
	return nil
}

// writeJSONStruct writes a struct/object as a JSON object.
func writeJSONStruct(buf *bytes.Buffer, td *TypeDef, value *Value) error {
	buf.WriteByte('{')

	// Track which JSON arrays we've already written for JSONArray fields
	writtenArrays := map[string]bool{}
	first := true

	for i, fd := range td.Fields {
		// Handle JSONArray fields: collect all fields with the same JSONArray name
		// and emit them as a single JSON array
		if fd.JSONArray != "" {
			if writtenArrays[fd.JSONArray] {
				continue
			}
			writtenArrays[fd.JSONArray] = true

			if !first {
				buf.WriteByte(',')
			}
			first = false

			if err := writeJSONString(buf, fd.JSONArray); err != nil {
				return err
			}
			buf.WriteByte(':')
			buf.WriteByte('[')

			// Gather all fields for this array
			arrFirst := true
			for j := range td.Fields {
				if td.Fields[j].JSONArray == fd.JSONArray {
					if !arrFirst {
						buf.WriteByte(',')
					}
					arrFirst = false
					if j < len(value.Fields) {
						if err := writeJSONField(buf, &td.Fields[j], &value.Fields[j]); err != nil {
							return err
						}
					} else {
						buf.WriteString("0")
					}
				}
			}
			buf.WriteByte(']')
			continue
		}

		if !first {
			buf.WriteByte(',')
		}
		first = false

		if err := writeJSONString(buf, fd.Name); err != nil {
			return err
		}
		buf.WriteByte(':')

		if i < len(value.Fields) {
			if err := writeJSONField(buf, &fd, &value.Fields[i]); err != nil {
				return err
			}
		} else {
			// Default value for missing fields
			writeJSONDefault(buf, &fd)
		}
	}

	buf.WriteByte('}')
	return nil
}

// writeJSONDefault writes the default JSON value for a field kind.
func writeJSONDefault(buf *bytes.Buffer, fd *FieldDef) {
	switch fd.Kind {
	case KindBool:
		buf.WriteString("false")
	case KindU8, KindI8, KindU16, KindI16, KindU32, KindI32:
		buf.WriteString("0")
	case KindU64, KindI64:
		buf.WriteString("\"0\"")
	case KindF32, KindF64:
		buf.WriteString("0")
	case KindString:
		buf.WriteString("\"\"")
	case KindVec:
		buf.WriteString("[]")
	case KindOptional:
		buf.WriteString("null")
	case KindObject:
		buf.WriteString("{}")
	default:
		buf.WriteString("null")
	}
}

// writeJSONVec writes a vector as a JSON array.
func writeJSONVec(buf *bytes.Buffer, fd *FieldDef, value *Value) error {
	buf.WriteByte('[')
	for i, elem := range value.Vec {
		if i > 0 {
			buf.WriteByte(',')
		}
		elemFd := vecElemFieldDef(fd)
		if err := writeJSONField(buf, elemFd, &elem); err != nil {
			return err
		}
	}
	buf.WriteByte(']')
	return nil
}

// writeJSONOptional writes an optional as null or the inner value.
func writeJSONOptional(buf *bytes.Buffer, fd *FieldDef, value *Value) error {
	if value.Opt == nil {
		buf.WriteString("null")
		return nil
	}
	// Recurse on the inner value using the element kind
	innerFd := optionalInnerFieldDef(fd)
	return writeJSONField(buf, innerFd, value.Opt)
}

// writeJSONVariant writes a variant as {"CaseName": value}.
func writeJSONVariant(buf *bytes.Buffer, td *TypeDef, value *Value) error {
	if int(value.Tag) >= len(td.Cases) {
		return fmt.Errorf("fracpack: variant tag %d out of range (%d cases)", value.Tag, len(td.Cases))
	}
	caseDef := &td.Cases[value.Tag]

	buf.WriteByte('{')
	if err := writeJSONString(buf, caseDef.Name); err != nil {
		return err
	}
	buf.WriteByte(':')

	if value.Variant != nil {
		if err := writeJSONField(buf, caseDef, value.Variant); err != nil {
			return err
		}
	} else {
		writeJSONDefault(buf, caseDef)
	}

	buf.WriteByte('}')
	return nil
}

// writeJSONTuple writes a tuple as a JSON array.
func writeJSONTuple(buf *bytes.Buffer, td *TypeDef, value *Value) error {
	buf.WriteByte('[')
	for i, ed := range td.Elements {
		if i > 0 {
			buf.WriteByte(',')
		}
		if i < len(value.Tuple) {
			if err := writeJSONField(buf, &ed, &value.Tuple[i]); err != nil {
				return err
			}
		} else {
			writeJSONDefault(buf, &ed)
		}
	}
	buf.WriteByte(']')
	return nil
}

// ── FromJSON implementation ───────────────────────────────────────────────

// fromJSONValue converts a parsed JSON value to a fracpack Value using the TypeDef.
func fromJSONValue(td *TypeDef, jsonVal interface{}) (*Value, error) {
	if td.IsVariant {
		return fromJSONVariant(td, jsonVal)
	}
	if td.IsTuple {
		return fromJSONTuple(td, jsonVal)
	}
	return fromJSONStruct(td, jsonVal)
}

// fromJSONStruct converts a JSON object to a fracpack struct/object Value.
func fromJSONStruct(td *TypeDef, jsonVal interface{}) (*Value, error) {
	obj, ok := jsonVal.(map[string]interface{})
	if !ok {
		return nil, fmt.Errorf("fracpack: expected JSON object for %s, got %T", td.Name, jsonVal)
	}

	val := &Value{Kind: KindObject}

	// Pre-parse any JSON arrays referenced by JSONArray fields
	arrays := map[string][]interface{}{}
	for _, fd := range td.Fields {
		if fd.JSONArray != "" {
			if _, ok := arrays[fd.JSONArray]; !ok {
				if arr, ok := obj[fd.JSONArray]; ok {
					if a, ok := arr.([]interface{}); ok {
						arrays[fd.JSONArray] = a
					}
				}
			}
		}
	}

	for _, fd := range td.Fields {
		// Handle array-mapped fields
		if fd.JSONArray != "" {
			arr := arrays[fd.JSONArray]
			if fd.JSONIndex < len(arr) {
				fv, err := fromJSONFieldValue(&fd, arr[fd.JSONIndex])
				if err != nil {
					return nil, fmt.Errorf("fracpack: field %s[%d]: %w", fd.JSONArray, fd.JSONIndex, err)
				}
				val.Fields = append(val.Fields, *fv)
			} else {
				val.Fields = append(val.Fields, zeroValue(fd.Kind))
			}
			continue
		}

		fieldVal, ok := obj[fd.Name]
		if !ok || fieldVal == nil {
			if fd.Kind == KindOptional {
				val.Fields = append(val.Fields, Value{Kind: KindOptional, Opt: nil})
			} else {
				val.Fields = append(val.Fields, zeroValue(fd.Kind))
			}
			continue
		}
		fv, err := fromJSONFieldValue(&fd, fieldVal)
		if err != nil {
			return nil, fmt.Errorf("fracpack: field %s: %w", fd.Name, err)
		}
		val.Fields = append(val.Fields, *fv)
	}

	return val, nil
}

// fromJSONFieldValue converts a JSON value to a fracpack Value based on the FieldDef.
func fromJSONFieldValue(fd *FieldDef, jsonVal interface{}) (*Value, error) {
	switch fd.Kind {
	case KindBool:
		v := &Value{Kind: KindBool}
		switch b := jsonVal.(type) {
		case bool:
			v.Bool = b
		case json.Number:
			v.Bool = b.String() != "0"
		default:
			v.Bool = false
		}
		return v, nil

	case KindU8:
		n, err := toUint64(jsonVal)
		if err != nil {
			return nil, err
		}
		return &Value{Kind: KindU8, U8: uint8(n)}, nil

	case KindI8:
		n, err := toInt64(jsonVal)
		if err != nil {
			return nil, err
		}
		return &Value{Kind: KindI8, I8: int8(n)}, nil

	case KindU16:
		n, err := toUint64(jsonVal)
		if err != nil {
			return nil, err
		}
		return &Value{Kind: KindU16, U16: uint16(n)}, nil

	case KindI16:
		n, err := toInt64(jsonVal)
		if err != nil {
			return nil, err
		}
		return &Value{Kind: KindI16, I16: int16(n)}, nil

	case KindU32:
		n, err := toUint64(jsonVal)
		if err != nil {
			return nil, err
		}
		return &Value{Kind: KindU32, U32: uint32(n)}, nil

	case KindI32:
		n, err := toInt64(jsonVal)
		if err != nil {
			return nil, err
		}
		return &Value{Kind: KindI32, I32: int32(n)}, nil

	case KindU64:
		// u64 comes as a JSON string in canonical format
		n, err := toUint64(jsonVal)
		if err != nil {
			return nil, err
		}
		return &Value{Kind: KindU64, U64: n}, nil

	case KindI64:
		// i64 comes as a JSON string in canonical format
		n, err := toInt64(jsonVal)
		if err != nil {
			return nil, err
		}
		return &Value{Kind: KindI64, I64: n}, nil

	case KindF32:
		n, err := toFloat64(jsonVal)
		if err != nil {
			return nil, err
		}
		return &Value{Kind: KindF32, F32: float32(n)}, nil

	case KindF64:
		n, err := toFloat64(jsonVal)
		if err != nil {
			return nil, err
		}
		return &Value{Kind: KindF64, F64: n}, nil

	case KindString:
		s, ok := jsonVal.(string)
		if !ok {
			return nil, fmt.Errorf("expected string, got %T", jsonVal)
		}
		return &Value{Kind: KindString, Str: s}, nil

	case KindVec:
		return fromJSONVec(fd, jsonVal)

	case KindOptional:
		return fromJSONOptional(fd, jsonVal)

	case KindObject:
		if fd.InnerDef == nil {
			return nil, fmt.Errorf("object field %q has no type definition", fd.Name)
		}
		return fromJSONValue(fd.InnerDef, jsonVal)

	case KindVariant:
		if fd.InnerDef == nil {
			return nil, fmt.Errorf("variant field %q has no type definition", fd.Name)
		}
		return fromJSONVariant(fd.InnerDef, jsonVal)

	case KindTuple:
		if fd.InnerDef == nil {
			return nil, fmt.Errorf("tuple field %q has no type definition", fd.Name)
		}
		return fromJSONTuple(fd.InnerDef, jsonVal)

	default:
		return nil, fmt.Errorf("unknown field kind %d", fd.Kind)
	}
}

// fromJSONVec converts a JSON array to a fracpack Vec value.
func fromJSONVec(fd *FieldDef, jsonVal interface{}) (*Value, error) {
	arr, ok := jsonVal.([]interface{})
	if !ok {
		return nil, fmt.Errorf("expected JSON array for vec, got %T", jsonVal)
	}

	val := &Value{Kind: KindVec}
	elemFd := vecElemFieldDef(fd)

	for _, elem := range arr {
		ev, err := fromJSONFieldValue(elemFd, elem)
		if err != nil {
			return nil, err
		}
		val.Vec = append(val.Vec, *ev)
	}
	return val, nil
}

// fromJSONOptional converts a JSON value to an optional fracpack Value.
func fromJSONOptional(fd *FieldDef, jsonVal interface{}) (*Value, error) {
	if jsonVal == nil {
		return &Value{Kind: KindOptional, Opt: nil}, nil
	}
	innerFd := optionalInnerFieldDef(fd)
	// If the inner type is a scalar default (U32) but the JSON value is a string,
	// infer that it's actually a string optional. This handles cases where the
	// TypeDef doesn't fully specify the optional's inner type.
	if innerFd.Kind == KindU32 {
		if _, ok := jsonVal.(string); ok {
			innerFd = &FieldDef{Kind: KindString}
		}
	}
	inner, err := fromJSONFieldValue(innerFd, jsonVal)
	if err != nil {
		return nil, err
	}
	return &Value{Kind: KindOptional, Opt: inner}, nil
}

// fromJSONVariant converts a JSON single-key object to a fracpack Variant value.
func fromJSONVariant(td *TypeDef, jsonVal interface{}) (*Value, error) {
	obj, ok := jsonVal.(map[string]interface{})
	if !ok {
		return nil, fmt.Errorf("expected JSON object for variant, got %T", jsonVal)
	}
	if len(obj) != 1 {
		return nil, fmt.Errorf("variant JSON must have exactly one key, got %d", len(obj))
	}

	for caseName, payload := range obj {
		for i, c := range td.Cases {
			if c.Name == caseName {
				val := &Value{Kind: KindVariant, Tag: uint8(i)}
				inner, err := fromJSONFieldValue(&c, payload)
				if err != nil {
					return nil, fmt.Errorf("variant case %s: %w", caseName, err)
				}
				val.Variant = inner
				return val, nil
			}
		}
		return nil, fmt.Errorf("unknown variant case: %s", caseName)
	}
	return nil, fmt.Errorf("empty variant object")
}

// fromJSONTuple converts a JSON array to a fracpack Tuple value.
func fromJSONTuple(td *TypeDef, jsonVal interface{}) (*Value, error) {
	arr, ok := jsonVal.([]interface{})
	if !ok {
		return nil, fmt.Errorf("expected JSON array for tuple, got %T", jsonVal)
	}

	val := &Value{Kind: KindTuple}
	for i, ed := range td.Elements {
		if i < len(arr) {
			ev, err := fromJSONFieldValue(&ed, arr[i])
			if err != nil {
				return nil, err
			}
			val.Tuple = append(val.Tuple, *ev)
		} else {
			val.Tuple = append(val.Tuple, zeroValue(ed.Kind))
		}
	}
	return val, nil
}

// ── Helper functions ──────────────────────────────────────────────────────

// vecElemFieldDef constructs a FieldDef describing a vector's element type.
func vecElemFieldDef(fd *FieldDef) *FieldDef {
	if fd.ElemKind == KindOptional {
		// Vec<Optional<T>>: the optional's inner type is determined by
		// OptInnerKind, ElemDef, or defaults to KindU32.
		innerKind := fd.OptInnerKind
		if innerKind == 0 {
			if fd.ElemDef != nil {
				innerKind = KindObject
			} else {
				innerKind = KindU32
			}
		}
		return &FieldDef{
			Kind:     KindOptional,
			ElemKind: innerKind,
			ElemDef:  fd.ElemDef,
		}
	}
	if fd.ElemKind == KindVec {
		// Vec<Vec<T>>: nested vec, inner elements default to KindU32.
		return &FieldDef{
			Kind:     KindVec,
			ElemKind: KindU32,
			ElemDef:  fd.ElemDef,
		}
	}
	return &FieldDef{
		Kind:     fd.ElemKind,
		InnerDef: fd.ElemDef,
		ElemKind: fd.ElemKind,
		ElemDef:  fd.ElemDef,
	}
}

// optionalInnerFieldDef constructs a FieldDef describing an optional's inner type.
func optionalInnerFieldDef(fd *FieldDef) *FieldDef {
	innerKind := fd.ElemKind
	switch innerKind {
	case KindOptional, 0:
		// Avoid infinite recursion for Optional<Optional<...>> / missing kind
		innerKind = KindU32
		return &FieldDef{Kind: innerKind, ElemKind: innerKind, InnerDef: fd.ElemDef, ElemDef: fd.ElemDef}
	case KindVec:
		// Optional<Vec<T>>: the inner vec's element type defaults to KindU32
		return &FieldDef{Kind: KindVec, ElemKind: KindU32, InnerDef: fd.ElemDef, ElemDef: fd.ElemDef}
	case KindObject:
		return &FieldDef{Kind: KindObject, InnerDef: fd.ElemDef, ElemDef: fd.ElemDef}
	case KindString:
		return &FieldDef{Kind: KindString}
	default:
		// Scalar types
		return &FieldDef{Kind: innerKind}
	}
}

// toUint64 converts a JSON value to uint64.
func toUint64(v interface{}) (uint64, error) {
	switch n := v.(type) {
	case json.Number:
		s := n.String()
		return strconv.ParseUint(s, 10, 64)
	case string:
		return strconv.ParseUint(n, 10, 64)
	case float64:
		return uint64(n), nil
	case bool:
		if n {
			return 1, nil
		}
		return 0, nil
	default:
		return 0, fmt.Errorf("cannot convert %T to uint64", v)
	}
}

// toInt64 converts a JSON value to int64.
func toInt64(v interface{}) (int64, error) {
	switch n := v.(type) {
	case json.Number:
		s := n.String()
		return strconv.ParseInt(s, 10, 64)
	case string:
		return strconv.ParseInt(n, 10, 64)
	case float64:
		return int64(n), nil
	case bool:
		if n {
			return 1, nil
		}
		return 0, nil
	default:
		return 0, fmt.Errorf("cannot convert %T to int64", v)
	}
}

// toFloat64 converts a JSON value to float64.
func toFloat64(v interface{}) (float64, error) {
	switch n := v.(type) {
	case json.Number:
		return n.Float64()
	case float64:
		return n, nil
	case string:
		return strconv.ParseFloat(n, 64)
	case nil:
		return math.NaN(), nil
	default:
		return 0, fmt.Errorf("cannot convert %T to float64", v)
	}
}

