package fracpack

import (
	"encoding/binary"
	"math"
)

// FracView provides zero-copy read access to fracpack-encoded data.
//
// After validating data once, a FracView reads fields directly from the
// underlying byte slice without deserialization or allocation. String fields
// return []byte slices into the original buffer for true zero-copy access.
//
// Usage:
//
//	view := fracpack.NewView(td, data)
//	name := view.Field("name").String()     // zero-copy []byte → string
//	age  := view.Field("age").U32()         // read u32 directly
//	inner := view.Field("inner").Object()   // nested view
//	items := view.Field("items").Vec()      // vector view
type FracView struct {
	td   *TypeDef
	data []byte
	base uint32 // absolute position in data where this object starts
}

// NewView creates a zero-copy view over fracpack-encoded data.
// The data must have been validated before creating the view.
func NewView(td *TypeDef, data []byte) *FracView {
	return &FracView{td: td, data: data, base: 0}
}

// viewAt creates a view starting at a specific position.
func viewAt(td *TypeDef, data []byte, pos uint32) *FracView {
	return &FracView{td: td, data: data, base: pos}
}

// fieldInfo resolves the position of a named field using pre-computed offsets.
func (v *FracView) fieldInfo(name string) (fd *FieldDef, fixedPos uint32, found bool) {
	v.td.initOffsets()
	idx, ok := v.td.nameIdx[name]
	if !ok {
		return nil, 0, false
	}
	pos := v.base
	if v.td.Extensible {
		pos += 2 // skip u16 header
	}
	return &v.td.Fields[idx], pos + v.td.offsets[idx], true
}

// FieldView is a handle to a single field within a FracView.
// It provides typed accessors that read directly from the buffer.
type FieldView struct {
	fd       *FieldDef
	data     []byte
	fixedPos uint32
}

// Field returns a FieldView for the named field.
// Panics if the field doesn't exist.
func (v *FracView) Field(name string) FieldView {
	fd, pos, ok := v.fieldInfo(name)
	if !ok {
		panic("fracpack: no field " + name + " in " + v.td.Name)
	}
	return FieldView{fd: fd, data: v.data, fixedPos: pos}
}

// FieldAt returns a FieldView for the field at the given index.
// This avoids string lookup entirely — use when the field index is known.
func (v *FracView) FieldAt(i int) FieldView {
	v.td.initOffsets()
	pos := v.base
	if v.td.Extensible {
		pos += 2
	}
	return FieldView{fd: &v.td.Fields[i], data: v.data, fixedPos: pos + v.td.offsets[i]}
}

// ── Scalar reads ─────────────────────────────────────────────────────────

func (fv FieldView) Bool() bool   { return fv.data[fv.fixedPos] != 0 }
func (fv FieldView) U8() uint8    { return fv.data[fv.fixedPos] }
func (fv FieldView) I8() int8     { return int8(fv.data[fv.fixedPos]) }
func (fv FieldView) U16() uint16  { return binary.LittleEndian.Uint16(fv.data[fv.fixedPos:]) }
func (fv FieldView) I16() int16   { return int16(binary.LittleEndian.Uint16(fv.data[fv.fixedPos:])) }
func (fv FieldView) U32() uint32  { return binary.LittleEndian.Uint32(fv.data[fv.fixedPos:]) }
func (fv FieldView) I32() int32   { return int32(binary.LittleEndian.Uint32(fv.data[fv.fixedPos:])) }
func (fv FieldView) U64() uint64  { return binary.LittleEndian.Uint64(fv.data[fv.fixedPos:]) }
func (fv FieldView) I64() int64   { return int64(binary.LittleEndian.Uint64(fv.data[fv.fixedPos:])) }
func (fv FieldView) F32() float32 { return math.Float32frombits(binary.LittleEndian.Uint32(fv.data[fv.fixedPos:])) }
func (fv FieldView) F64() float64 { return math.Float64frombits(binary.LittleEndian.Uint64(fv.data[fv.fixedPos:])) }

// ── Variable-size reads ──────────────────────────────────────────────────

// offset reads the u32 relative offset at fixedPos.
func (fv FieldView) offset() uint32 {
	return binary.LittleEndian.Uint32(fv.data[fv.fixedPos:])
}

// heapPos returns the absolute heap position (fixedPos + offset).
func (fv FieldView) heapPos() uint32 {
	return fv.fixedPos + fv.offset()
}

// Bytes returns the raw bytes of a string field as a zero-copy slice.
// Returns nil for empty strings.
func (fv FieldView) Bytes() []byte {
	off := fv.offset()
	if off == 0 {
		return nil
	}
	hp := fv.fixedPos + off
	strLen := binary.LittleEndian.Uint32(fv.data[hp:])
	if strLen == 0 {
		return nil
	}
	return fv.data[hp+4 : hp+4+strLen]
}

// String returns the string value. For true zero-copy, use Bytes() instead.
func (fv FieldView) String() string {
	b := fv.Bytes()
	if b == nil {
		return ""
	}
	return string(b)
}

// Object returns a nested FracView for an object field.
func (fv FieldView) Object() *FracView {
	off := fv.offset()
	if off == 0 {
		return nil
	}
	return viewAt(fv.fd.InnerDef, fv.data, fv.fixedPos+off)
}

// ── Optional access ──────────────────────────────────────────────────────

// IsPresent returns true if an optional field has a value.
func (fv FieldView) IsPresent() bool {
	off := fv.offset()
	return off != 1
}

// OptU32 returns the value of an optional u32 field.
func (fv FieldView) OptU32() (uint32, bool) {
	off := fv.offset()
	if off == 1 {
		return 0, false
	}
	if off == 0 {
		return 0, true // present but zero/empty
	}
	hp := fv.fixedPos + off
	return binary.LittleEndian.Uint32(fv.data[hp:]), true
}

// OptString returns the value of an optional string field.
func (fv FieldView) OptString() (string, bool) {
	off := fv.offset()
	if off == 1 {
		return "", false
	}
	if off == 0 {
		return "", true
	}
	hp := fv.fixedPos + off
	strLen := binary.LittleEndian.Uint32(fv.data[hp:])
	return string(fv.data[hp+4 : hp+4+strLen]), true
}

// OptBytes returns the bytes of an optional string field (zero-copy).
func (fv FieldView) OptBytes() ([]byte, bool) {
	off := fv.offset()
	if off == 1 {
		return nil, false
	}
	if off == 0 {
		return nil, true
	}
	hp := fv.fixedPos + off
	strLen := binary.LittleEndian.Uint32(fv.data[hp:])
	return fv.data[hp+4 : hp+4+strLen], true
}

// OptObject returns a nested view for an optional object field.
func (fv FieldView) OptObject() (*FracView, bool) {
	off := fv.offset()
	if off == 1 {
		return nil, false
	}
	if off == 0 {
		return nil, true
	}
	return viewAt(fv.fd.ElemDef, fv.data, fv.fixedPos+off), true
}

// ── Vector access ────────────────────────────────────────────────────────

// VecView provides zero-copy iteration over a fracpack vector.
type VecView struct {
	fd      *FieldDef
	data    []byte
	start   uint32 // absolute position of first element
	numBytes uint32
}

// Vec returns a VecView for a vector field.
func (fv FieldView) Vec() VecView {
	off := fv.offset()
	if off == 0 {
		return VecView{fd: fv.fd, data: fv.data}
	}
	hp := fv.fixedPos + off
	byteCount := binary.LittleEndian.Uint32(fv.data[hp:])
	return VecView{
		fd:       fv.fd,
		data:     fv.data,
		start:    hp + 4,
		numBytes: byteCount,
	}
}

// Len returns the number of elements (for fixed-size element types).
func (vv VecView) Len() int {
	if vv.numBytes == 0 {
		return 0
	}
	elemSize := vv.elemFixedSize()
	if elemSize == 0 {
		return 0
	}
	return int(vv.numBytes / elemSize)
}

// IsEmpty returns true if the vector has no elements.
func (vv VecView) IsEmpty() bool {
	return vv.numBytes == 0
}

func (vv VecView) elemFixedSize() uint32 {
	if vv.fd.ElemDef != nil && len(vv.fd.ElemDef.Fields) > 0 {
		// Object elements: header + fixed
		total := vv.fd.ElemDef.fixedSize()
		if vv.fd.ElemDef.Extensible {
			return total + 2
		}
		return total
	}
	// Primitive elements — infer from the element kind stored in ElemDef
	return 4 // default u32
}

// U32At returns the i-th u32 element.
func (vv VecView) U32At(i int) uint32 {
	pos := vv.start + uint32(i)*4
	return binary.LittleEndian.Uint32(vv.data[pos:])
}

// ObjectAt returns a FracView for the i-th object element.
func (vv VecView) ObjectAt(i int) *FracView {
	// Walk elements to find position (elements may be variable-size)
	pos := vv.start
	for j := 0; j < i; j++ {
		sz := packedSizeAt(vv.fd.ElemDef, vv.data, pos)
		pos += sz
	}
	return viewAt(vv.fd.ElemDef, vv.data, pos)
}

// StringAt returns the i-th string from a Vec<string>.
// Each string element occupies a 4-byte offset slot in the fixed region.
func (vv VecView) StringAt(i int) string {
	pos := vv.start + uint32(i)*4
	off := binary.LittleEndian.Uint32(vv.data[pos:])
	if off == 0 {
		return ""
	}
	hp := pos + off
	strLen := binary.LittleEndian.Uint32(vv.data[hp:])
	return string(vv.data[hp+4 : hp+4+strLen])
}

// BytesAt returns the i-th string from a Vec<string> as zero-copy bytes.
func (vv VecView) BytesAt(i int) []byte {
	pos := vv.start + uint32(i)*4
	off := binary.LittleEndian.Uint32(vv.data[pos:])
	if off == 0 {
		return nil
	}
	hp := pos + off
	strLen := binary.LittleEndian.Uint32(vv.data[hp:])
	return vv.data[hp+4 : hp+4+strLen]
}

// ── Variant view ─────────────────────────────────────────────────────────

// VariantView provides zero-copy access to variant data.
type VariantView struct {
	td   *TypeDef
	data []byte
	pos  uint32 // absolute position of the variant container
}

// Variant returns a VariantView for a variant field.
func (fv FieldView) Variant() VariantView {
	off := fv.offset()
	return VariantView{
		td:   fv.fd.InnerDef,
		data: fv.data,
		pos:  fv.fixedPos + off,
	}
}

// Tag returns the variant's discriminant tag.
func (vv VariantView) Tag() uint8 {
	return vv.data[vv.pos+4]
}

// PayloadU32 reads the payload as a u32.
func (vv VariantView) PayloadU32() uint32 {
	return binary.LittleEndian.Uint32(vv.data[vv.pos+5:])
}

// PayloadString reads the payload as a string.
func (vv VariantView) PayloadString() string {
	hp := vv.pos + 5
	strLen := binary.LittleEndian.Uint32(vv.data[hp:])
	return string(vv.data[hp+4 : hp+4+strLen])
}

// PayloadObject reads the payload as a nested object view.
func (vv VariantView) PayloadObject() *FracView {
	caseDef := &vv.td.Cases[vv.Tag()]
	return viewAt(caseDef.InnerDef, vv.data, vv.pos+5)
}
