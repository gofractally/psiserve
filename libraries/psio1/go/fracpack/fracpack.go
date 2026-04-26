// Package fracpack implements the fracpack binary serialization format.
//
// Fracpack uses a two-region layout: a fixed region containing scalars and
// offsets, followed by a heap region with variable-length data (strings,
// vectors, nested objects). All integers are little-endian.
//
// Struct types come in two flavors:
//   - Struct (definitionWillNotChange): no header, fields concatenated
//   - Object (extensible): u16 fixed_size header, then fields, then heap
//
// Variable-size fields in the fixed region store a u32 offset relative to
// the offset's own position, pointing into the heap. Special offset values:
//   - 0: empty container (empty string, empty vec)
//   - 1: absent optional (None/null)
//
// # Typed Views (Code Generation)
//
// For performance-critical read paths, generate typed view structs that
// access fields at compile-time-known byte offsets instead of using the
// generic [FracView.Field] runtime dispatch. Generated views are 3–19x
// faster than generic views and match or beat native Go struct access for
// complex types (zero-copy []byte slices avoid GC string allocation).
//
// Define your types as Go structs and add one line:
//
//	//go:generate go run github.com/psibase/psio/fracpack-viewgen -type MyType
//
//	type MyType struct {
//	    ID   uint64
//	    Name string
//	}
//
// Run go generate, and use the generated views:
//
//	packed := fracpack.Pack(td, val)
//	v := NewMyTypeView(packed)
//	id := v.ID()           // uint64, direct read at known offset
//	name := v.NameBytes()  // []byte slice into packed buffer, zero-copy
//
// The tool maps Go types to fracpack kinds automatically (uint64→u64,
// string→string, *string→optional<string>, []string→vec<string>,
// StructName→object). Field names are converted from CamelCase to
// snake_case; override with a struct tag: `fracpack:"custom_name"`.
// By default all types are extensible; use -fixed to mark non-extensible types.
//
// See fracpack-viewgen for full documentation.
package fracpack

import (
	"encoding/binary"
	"fmt"
	"math"
)

// FieldKind describes how a field is encoded in the fixed region.
type FieldKind int

const (
	KindBool    FieldKind = iota
	KindU8
	KindI8
	KindU16
	KindI16
	KindU32
	KindI32
	KindU64
	KindI64
	KindF32
	KindF64
	KindString
	KindVec
	KindOptional
	KindObject
	KindVariant
	KindTuple
)

// FieldDef describes one field in a fracpack struct/object.
type FieldDef struct {
	Name         string
	Kind         FieldKind
	FixedSize    uint32    // size in the fixed region (4 for variable-size = offset slot)
	IsVar        bool      // true if variable-size (heap-allocated)
	ElemKind     FieldKind // for Vec/Optional: element kind
	ElemDef      *TypeDef  // for Vec: element type; for Optional: inner type
	InnerDef     *TypeDef  // for Object/Variant fields: the nested struct/variant definition
	OptInnerKind FieldKind // for Vec<Optional<T>>: the T kind when ElemKind is KindOptional
	// Test helpers (not part of the format)
	JSONArray string // if set, this field reads from a JSON array with this name
	JSONIndex int    // index within the JSON array
}

// TypeDef describes a fracpack-serializable type.
type TypeDef struct {
	Name       string
	Extensible bool      // true = Object (has u16 header), false = Struct
	Fields     []FieldDef
	// For variant types
	IsVariant  bool
	Cases      []FieldDef // variant cases (Name = case name, Kind/InnerDef = payload type)
	// For tuple types
	IsTuple    bool
	Elements   []FieldDef // tuple elements (heterogeneous, each has its own Kind)

	// Pre-computed field offsets (populated lazily by fieldOffsets()).
	// offsets[i] is the byte offset of field i within the fixed region
	// (excluding the u16 header for extensible types).
	offsets []uint32
	nameIdx map[string]int // field name → index
}

// initOffsets pre-computes field offsets and name index.
// Safe to call multiple times; only computes once.
func (td *TypeDef) initOffsets() {
	if td.offsets != nil {
		return
	}
	td.offsets = make([]uint32, len(td.Fields))
	td.nameIdx = make(map[string]int, len(td.Fields))
	var pos uint32
	for i, f := range td.Fields {
		td.offsets[i] = pos
		td.nameIdx[f.Name] = i
		pos += f.FixedSize
	}
}

// fixedSize returns the total fixed region size for this type (excluding header).
func (td *TypeDef) fixedSize() uint32 {
	var total uint32
	for _, f := range td.Fields {
		total += f.FixedSize
	}
	return total
}

// fieldFixedSize returns the size a field occupies in the fixed region.
func fieldFixedSize(kind FieldKind) uint32 {
	switch kind {
	case KindBool, KindU8, KindI8:
		return 1
	case KindU16, KindI16:
		return 2
	case KindU32, KindI32, KindF32:
		return 4
	case KindU64, KindI64, KindF64:
		return 8
	case KindString, KindVec, KindOptional, KindObject, KindVariant, KindTuple:
		return 4 // offset slot
	default:
		return 0
	}
}

// isVariableKind returns true for types that use heap storage.
func isVariableKind(kind FieldKind) bool {
	switch kind {
	case KindString, KindVec, KindOptional, KindObject, KindVariant, KindTuple:
		return true
	default:
		return false
	}
}

// Value represents a fracpack value for packing/unpacking.
type Value struct {
	Kind FieldKind
	// Scalar values (exactly one is used based on Kind)
	Bool   bool
	U8     uint8
	I8     int8
	U16    uint16
	I16    int16
	U32    uint32
	I32    int32
	U64    uint64
	I64    int64
	F32    float32
	F64    float64
	Str    string
	Vec    []Value   // for KindVec
	Opt    *Value    // for KindOptional: nil = absent, non-nil = present
	Fields []Value   // for KindObject: field values
	Tuple  []Value   // for KindTuple: element values
	// For variant
	Tag    uint8
	Variant *Value   // variant payload (nil = no payload)
}

// Pack serializes a value according to its type definition.
func Pack(td *TypeDef, val *Value) []byte {
	if td.IsVariant {
		return packVariant(td, val)
	}
	if td.IsTuple {
		return packTuple(td, val)
	}
	return packObject(td, val)
}

func packObject(td *TypeDef, val *Value) []byte {
	fixedTotal := td.fixedSize()

	var headerSize uint32
	if td.Extensible {
		headerSize = 2
	}

	// For extensible objects, trim trailing absent optionals.
	actualFixed := fixedTotal
	if td.Extensible {
		actualFixed = computeActualFixed(td, val)
	}

	fixed := make([]byte, headerSize+actualFixed)
	if td.Extensible {
		binary.LittleEndian.PutUint16(fixed[0:2], uint16(actualFixed))
	}

	var heap []byte
	fixedPos := headerSize

	for i, fd := range td.Fields {
		if fixedPos-headerSize >= actualFixed {
			break
		}
		fval := &val.Fields[i]
		if !fd.IsVar {
			// Fixed-size field: write inline
			writeFixed(fixed, fixedPos, fd.Kind, fval)
		} else {
			// Variable-size field: write offset + heap data
			heapData := packHeapField(&fd, fval)
			if len(heapData) == 0 {
				// Empty container or absent optional
				if fd.Kind == KindOptional && fval.Opt == nil {
					binary.LittleEndian.PutUint32(fixed[fixedPos:], 1) // absent
				} else {
					binary.LittleEndian.PutUint32(fixed[fixedPos:], 0) // empty
				}
			} else {
				// Offset = distance from this position to start of heap data
				heapStart := uint32(len(fixed)) + uint32(len(heap)) - fixedPos
				binary.LittleEndian.PutUint32(fixed[fixedPos:], heapStart)
				heap = append(heap, heapData...)
			}
		}
		fixedPos += fd.FixedSize
	}

	return append(fixed, heap...)
}

func packTuple(td *TypeDef, val *Value) []byte {
	elems := td.Elements
	values := val.Tuple

	// Compute number of present elements (trailing optional elision)
	numPresent := computeTuplePresent(elems, values)

	// Compute fixed size for the present elements
	var membersFixed uint32
	for i := 0; i < numPresent; i++ {
		membersFixed += elemFixedSize(&elems[i])
	}

	// Header (u16 fixed_size) + fixed region
	fixed := make([]byte, 2+membersFixed)
	binary.LittleEndian.PutUint16(fixed[0:2], uint16(membersFixed))

	var heap []byte
	fixedPos := uint32(2)

	for i := 0; i < numPresent; i++ {
		ed := &elems[i]
		var ev *Value
		if i < len(values) {
			ev = &values[i]
		} else {
			z := zeroValue(ed.Kind)
			ev = &z
		}

		efsz := elemFixedSize(ed)
		if !elemIsVar(ed) {
			// Fixed-size element: write inline
			writeFixed(fixed, fixedPos, ed.Kind, ev)
		} else {
			// Variable-size element: write offset + heap data
			heapData := packHeapElem(ed, ev)
			if len(heapData) == 0 {
				// Empty container or absent optional
				if ed.Kind == KindOptional && ev.Opt == nil {
					binary.LittleEndian.PutUint32(fixed[fixedPos:], 1) // absent
				} else {
					binary.LittleEndian.PutUint32(fixed[fixedPos:], 0) // empty
				}
			} else {
				heapStart := uint32(len(fixed)) + uint32(len(heap)) - fixedPos
				binary.LittleEndian.PutUint32(fixed[fixedPos:], heapStart)
				heap = append(heap, heapData...)
			}
		}
		fixedPos += efsz
	}

	return append(fixed, heap...)
}

// elemFixedSize returns the fixed region size for a tuple element.
func elemFixedSize(ed *FieldDef) uint32 {
	if ed.FixedSize > 0 {
		return ed.FixedSize
	}
	return fieldFixedSize(ed.Kind)
}

// elemIsVar returns true if a tuple element uses heap storage.
func elemIsVar(ed *FieldDef) bool {
	if ed.IsVar {
		return true
	}
	return isVariableKind(ed.Kind)
}

// packHeapElem packs the heap data for a tuple element (reuses packHeapField logic).
func packHeapElem(ed *FieldDef, val *Value) []byte {
	return packHeapField(ed, val)
}

// computeTuplePresent returns the number of elements to include,
// applying trailing optional elision.
func computeTuplePresent(elems []FieldDef, values []Value) int {
	lastPresent := -1
	for i := range elems {
		if i >= len(values) {
			break
		}
		if elems[i].Kind == KindOptional {
			if values[i].Opt != nil {
				lastPresent = i
			}
		} else {
			lastPresent = i
		}
	}
	if lastPresent < 0 {
		return 0
	}
	return lastPresent + 1
}

// computeActualFixed finds the trimmed fixed size for extensible objects.
// Only trailing optional-absent fields are trimmed. Non-optional fields
// (even if zero/empty) are always included.
func computeActualFixed(td *TypeDef, val *Value) uint32 {
	lastPresent := -1
	for i, fd := range td.Fields {
		if i >= len(val.Fields) {
			break
		}
		if fd.Kind == KindOptional {
			// Only trim if absent
			if val.Fields[i].Opt != nil {
				lastPresent = i
			}
		} else {
			// Non-optional fields are always present
			lastPresent = i
		}
	}
	if lastPresent < 0 {
		return 0
	}
	var size uint32
	for i := 0; i <= lastPresent; i++ {
		size += td.Fields[i].FixedSize
	}
	return size
}

func writeFixed(buf []byte, pos uint32, kind FieldKind, val *Value) {
	switch kind {
	case KindBool:
		if val.Bool {
			buf[pos] = 1
		} else {
			buf[pos] = 0
		}
	case KindU8:
		buf[pos] = val.U8
	case KindI8:
		buf[pos] = byte(val.I8)
	case KindU16:
		binary.LittleEndian.PutUint16(buf[pos:], val.U16)
	case KindI16:
		binary.LittleEndian.PutUint16(buf[pos:], uint16(val.I16))
	case KindU32:
		binary.LittleEndian.PutUint32(buf[pos:], val.U32)
	case KindI32:
		binary.LittleEndian.PutUint32(buf[pos:], uint32(val.I32))
	case KindU64:
		binary.LittleEndian.PutUint64(buf[pos:], val.U64)
	case KindI64:
		binary.LittleEndian.PutUint64(buf[pos:], uint64(val.I64))
	case KindF32:
		binary.LittleEndian.PutUint32(buf[pos:], math.Float32bits(val.F32))
	case KindF64:
		binary.LittleEndian.PutUint64(buf[pos:], math.Float64bits(val.F64))
	}
}

// writeArray writes a fixed-size array of elements inline.
func writeArray(buf []byte, pos uint32, elemKind FieldKind, vals []Value) {
	elemSize := fieldFixedSize(elemKind)
	for i := range vals {
		writeFixed(buf, pos+uint32(i)*elemSize, elemKind, &vals[i])
	}
}

func packHeapField(fd *FieldDef, val *Value) []byte {
	switch fd.Kind {
	case KindString:
		if len(val.Str) == 0 {
			return nil // offset = 0
		}
		b := make([]byte, 4+len(val.Str))
		binary.LittleEndian.PutUint32(b[0:4], uint32(len(val.Str)))
		copy(b[4:], val.Str)
		return b

	case KindVec:
		if len(val.Vec) == 0 {
			return nil // offset = 0
		}
		return packVec(fd, val.Vec)

	case KindOptional:
		if val.Opt == nil {
			return nil // offset = 1 (handled by caller)
		}
		return packOptionalPayload(fd, val.Opt)

	case KindObject:
		return packObject(fd.InnerDef, val)

	case KindVariant:
		return packVariant(fd.InnerDef, val)

	case KindTuple:
		return packTuple(fd.InnerDef, val)

	default:
		return nil
	}
}

func packVec(fd *FieldDef, elems []Value) []byte {
	if fd.ElemDef != nil && len(fd.ElemDef.Fields) > 0 {
		// Vector of variable-size objects: offset slots + heap data
		return packVarElemVec(fd.ElemDef, elems)
	}

	if fd.ElemKind == KindString {
		return packStringVec(elems)
	}

	if fd.ElemKind == KindOptional {
		return packOptionalVec(fd, elems)
	}

	if fd.ElemKind == KindVec {
		return packNestedVec(elems)
	}

	// Primitive vector (fixed-size elements)
	return packPrimitiveVec(elems)
}

func packVarElemVec(elemDef *TypeDef, elems []Value) []byte {
	// Variable-size element vec:
	// [u32 fixed_bytes = N*4] [offset_0]...[offset_N-1] [elem_0_packed]...[elem_N-1_packed]
	numElems := len(elems)
	fixedBytes := uint32(numElems * 4) // each element is a 4-byte offset slot

	// Pack each element
	var elemPacked [][]byte
	for i := range elems {
		packed := packObject(elemDef, &elems[i])
		elemPacked = append(elemPacked, packed)
	}

	heapTotal := uint32(0)
	for _, p := range elemPacked {
		heapTotal += uint32(len(p))
	}

	buf := make([]byte, 4+fixedBytes+heapTotal)
	binary.LittleEndian.PutUint32(buf[0:4], fixedBytes)

	heapStart := uint32(4) + fixedBytes
	heapOff := uint32(0)
	for i := 0; i < numElems; i++ {
		offPos := uint32(4 + i*4)
		if len(elemPacked[i]) == 0 {
			binary.LittleEndian.PutUint32(buf[offPos:], 0)
		} else {
			relOff := heapStart + heapOff - offPos
			binary.LittleEndian.PutUint32(buf[offPos:], relOff)
			copy(buf[heapStart+heapOff:], elemPacked[i])
			heapOff += uint32(len(elemPacked[i]))
		}
	}
	return buf
}

func packNestedVec(elems []Value) []byte {
	// Vec<Vec<T>> — each inner vec is variable-size, needs offset indirection
	numElems := len(elems)
	fixedBytes := uint32(numElems * 4)

	var innerPacked [][]byte
	for i := range elems {
		innerFd := FieldDef{Kind: KindVec, ElemKind: KindU32}
		if len(elems[i].Vec) == 0 {
			innerPacked = append(innerPacked, nil)
		} else {
			packed := packVec(&innerFd, elems[i].Vec)
			innerPacked = append(innerPacked, packed)
		}
	}

	heapTotal := uint32(0)
	for _, p := range innerPacked {
		heapTotal += uint32(len(p))
	}

	buf := make([]byte, 4+fixedBytes+heapTotal)
	binary.LittleEndian.PutUint32(buf[0:4], fixedBytes)

	heapStart := uint32(4) + fixedBytes
	heapOff := uint32(0)
	for i := 0; i < numElems; i++ {
		offPos := uint32(4 + i*4)
		if innerPacked[i] == nil {
			binary.LittleEndian.PutUint32(buf[offPos:], 0)
		} else {
			relOff := heapStart + heapOff - offPos
			binary.LittleEndian.PutUint32(buf[offPos:], relOff)
			copy(buf[heapStart+heapOff:], innerPacked[i])
			heapOff += uint32(len(innerPacked[i]))
		}
	}
	return buf
}

func packStringVec(elems []Value) []byte {
	// String vec uses the standard container layout:
	// [u32 fixed_bytes] [offset_0] [offset_1] ... [heap string_0] [heap string_1] ...
	// fixed_bytes = N * 4 (just the offset slots, not heap)
	numElems := len(elems)
	fixedBytes := uint32(numElems * 4)

	// Build heap data for each string
	var heapParts [][]byte
	for _, e := range elems {
		if len(e.Str) == 0 {
			heapParts = append(heapParts, nil)
		} else {
			part := make([]byte, 4+len(e.Str))
			binary.LittleEndian.PutUint32(part[0:4], uint32(len(e.Str)))
			copy(part[4:], e.Str)
			heapParts = append(heapParts, part)
		}
	}

	heapTotal := uint32(0)
	for _, p := range heapParts {
		heapTotal += uint32(len(p))
	}

	buf := make([]byte, 4+fixedBytes+heapTotal)
	binary.LittleEndian.PutUint32(buf[0:4], fixedBytes)

	// Write offsets and heap data
	heapStart := uint32(4) + fixedBytes // absolute position in buf where heap begins
	heapOff := uint32(0)
	for i := 0; i < numElems; i++ {
		offPos := uint32(4 + i*4)
		if heapParts[i] == nil {
			binary.LittleEndian.PutUint32(buf[offPos:], 0) // empty string
		} else {
			relOff := heapStart + heapOff - offPos
			binary.LittleEndian.PutUint32(buf[offPos:], relOff)
			copy(buf[heapStart+heapOff:], heapParts[i])
			heapOff += uint32(len(heapParts[i]))
		}
	}
	return buf
}

func packOptionalVec(fd *FieldDef, elems []Value) []byte {
	// Optional vec elements: each is a 4-byte offset slot
	numElems := len(elems)
	fixedSize := numElems * 4

	var heapParts [][]byte
	for _, e := range elems {
		if e.Opt == nil {
			heapParts = append(heapParts, nil)
		} else if e.Opt.Kind == KindString {
			// String optional payload: [u32 len][bytes]
			s := e.Opt.Str
			part := make([]byte, 4+len(s))
			binary.LittleEndian.PutUint32(part[0:4], uint32(len(s)))
			copy(part[4:], s)
			heapParts = append(heapParts, part)
		} else if e.Opt.Kind == KindObject && fd.ElemDef != nil {
			part := packObject(fd.ElemDef, e.Opt)
			heapParts = append(heapParts, part)
		} else {
			// Fixed-size optional payload
			sz := fieldFixedSize(e.Opt.Kind)
			part := make([]byte, sz)
			writeFixed(part, 0, e.Opt.Kind, e.Opt)
			heapParts = append(heapParts, part)
		}
	}

	heapTotal := 0
	for _, p := range heapParts {
		heapTotal += len(p)
	}

	totalBytes := fixedSize + heapTotal
	buf := make([]byte, 4+totalBytes)
	binary.LittleEndian.PutUint32(buf[0:4], uint32(fixedSize)) // only fixed slots count

	heapPos := uint32(4 + fixedSize)
	heapOff := uint32(0)
	for i := 0; i < numElems; i++ {
		offPos := uint32(4 + i*4)
		if heapParts[i] == nil {
			binary.LittleEndian.PutUint32(buf[offPos:], 1) // absent
		} else {
			relOff := heapPos + heapOff - offPos
			binary.LittleEndian.PutUint32(buf[offPos:], relOff)
			copy(buf[heapPos+heapOff:], heapParts[i])
			heapOff += uint32(len(heapParts[i]))
		}
	}
	return buf
}

func packPrimitiveVec(elems []Value) []byte {
	if len(elems) == 0 {
		return nil
	}
	// Determine element size from the first element's kind
	kind := elems[0].Kind
	elemSize := fieldFixedSize(kind)
	totalBytes := uint32(len(elems)) * elemSize
	buf := make([]byte, 4+totalBytes)
	binary.LittleEndian.PutUint32(buf[0:4], totalBytes)
	pos := uint32(4)
	for i := range elems {
		writeFixed(buf, pos, kind, &elems[i])
		pos += elemSize
	}
	return buf
}

func packOptionalPayload(fd *FieldDef, val *Value) []byte {
	if fd.ElemKind == KindObject && fd.ElemDef != nil {
		return packObject(fd.ElemDef, val)
	}
	if fd.ElemKind == KindVec {
		// Optional<Vec<T>> — pack the inner vec
		if len(val.Vec) == 0 {
			return nil // empty vec = offset 0
		}
		innerFd := FieldDef{Kind: KindVec, ElemKind: KindU32}
		return packVec(&innerFd, val.Vec)
	}
	// Primitive or string optional
	switch val.Kind {
	case KindString:
		b := make([]byte, 4+len(val.Str))
		binary.LittleEndian.PutUint32(b[0:4], uint32(len(val.Str)))
		copy(b[4:], val.Str)
		return b
	default:
		sz := fieldFixedSize(val.Kind)
		b := make([]byte, sz)
		writeFixed(b, 0, val.Kind, val)
		return b
	}
}

func packVariant(td *TypeDef, val *Value) []byte {
	// Variant: [u8 tag] [u32 content_size] [content]
	var content []byte
	if val.Variant != nil {
		caseDef := &td.Cases[val.Tag]
		if caseDef.InnerDef != nil {
			content = packObject(caseDef.InnerDef, val.Variant)
		} else if caseDef.Kind == KindString {
			s := val.Variant.Str
			content = make([]byte, 4+len(s))
			binary.LittleEndian.PutUint32(content[0:4], uint32(len(s)))
			copy(content[4:], s)
		} else {
			sz := fieldFixedSize(caseDef.Kind)
			content = make([]byte, sz)
			writeFixed(content, 0, caseDef.Kind, val.Variant)
		}
	}
	// [u8 tag][u32 content_size][content_bytes]
	buf := make([]byte, 1+4+len(content))
	buf[0] = val.Tag
	binary.LittleEndian.PutUint32(buf[1:5], uint32(len(content)))
	if len(content) > 0 {
		copy(buf[5:], content)
	}
	return buf
}

// Unpack deserializes fracpack bytes into a Value according to the type definition.
func Unpack(td *TypeDef, data []byte) (*Value, error) {
	if td.IsVariant {
		return unpackVariantTop(td, data)
	}
	if td.IsTuple {
		return unpackTuple(td, data, 0)
	}
	return unpackObject(td, data, 0)
}

func unpackObject(td *TypeDef, data []byte, start uint32) (*Value, error) {
	val := &Value{Kind: KindObject, Fields: make([]Value, 0, len(td.Fields))}
	pos := start

	var fixedTotal uint32
	if td.Extensible {
		if uint32(len(data))-start < 2 {
			return nil, fmt.Errorf("fracpack: buffer too short for object header")
		}
		fixedTotal = uint32(binary.LittleEndian.Uint16(data[pos : pos+2]))
		pos += 2
	} else {
		fixedTotal = td.fixedSize()
	}

	fixedStart := pos
	_ = fixedTotal // used for bounds checking

	for _, fd := range td.Fields {
		if pos-fixedStart >= fixedTotal {
			// Field beyond known fixed size (extensibility)
			val.Fields = append(val.Fields, zeroValue(fd.Kind))
			continue
		}

		if !fd.IsVar {
			fval := readFixed(data, pos, fd.Kind)
			val.Fields = append(val.Fields, fval)
		} else {
			fval, err := readVarField(&fd, data, pos)
			if err != nil {
				return nil, err
			}
			val.Fields = append(val.Fields, *fval)
		}
		pos += fd.FixedSize
	}

	return val, nil
}

func unpackTuple(td *TypeDef, data []byte, start uint32) (*Value, error) {
	val := &Value{Kind: KindTuple, Tuple: make([]Value, 0, len(td.Elements))}
	pos := start

	if uint32(len(data))-start < 2 {
		return nil, fmt.Errorf("fracpack: buffer too short for tuple header")
	}
	fixedTotal := uint32(binary.LittleEndian.Uint16(data[pos : pos+2]))
	pos += 2

	fixedStart := pos

	for _, ed := range td.Elements {
		if pos-fixedStart >= fixedTotal {
			// Element beyond known fixed size — default value
			if ed.Kind == KindOptional {
				val.Tuple = append(val.Tuple, Value{Kind: KindOptional, Opt: nil})
			} else {
				val.Tuple = append(val.Tuple, zeroValue(ed.Kind))
			}
			continue
		}

		efsz := elemFixedSize(&ed)
		if !elemIsVar(&ed) {
			fval := readFixed(data, pos, ed.Kind)
			val.Tuple = append(val.Tuple, fval)
		} else {
			fval, err := readVarField(&ed, data, pos)
			if err != nil {
				return nil, err
			}
			val.Tuple = append(val.Tuple, *fval)
		}
		pos += efsz
	}

	return val, nil
}

func readFixed(data []byte, pos uint32, kind FieldKind) Value {
	switch kind {
	case KindBool:
		return Value{Kind: KindBool, Bool: data[pos] != 0}
	case KindU8:
		return Value{Kind: KindU8, U8: data[pos]}
	case KindI8:
		return Value{Kind: KindI8, I8: int8(data[pos])}
	case KindU16:
		return Value{Kind: KindU16, U16: binary.LittleEndian.Uint16(data[pos:])}
	case KindI16:
		return Value{Kind: KindI16, I16: int16(binary.LittleEndian.Uint16(data[pos:]))}
	case KindU32:
		return Value{Kind: KindU32, U32: binary.LittleEndian.Uint32(data[pos:])}
	case KindI32:
		return Value{Kind: KindI32, I32: int32(binary.LittleEndian.Uint32(data[pos:]))}
	case KindU64:
		return Value{Kind: KindU64, U64: binary.LittleEndian.Uint64(data[pos:])}
	case KindI64:
		return Value{Kind: KindI64, I64: int64(binary.LittleEndian.Uint64(data[pos:]))}
	case KindF32:
		return Value{Kind: KindF32, F32: math.Float32frombits(binary.LittleEndian.Uint32(data[pos:]))}
	case KindF64:
		return Value{Kind: KindF64, F64: math.Float64frombits(binary.LittleEndian.Uint64(data[pos:]))}
	default:
		return Value{}
	}
}

func readVarField(fd *FieldDef, data []byte, pos uint32) (*Value, error) {
	offset := binary.LittleEndian.Uint32(data[pos:])

	switch fd.Kind {
	case KindString:
		if offset == 0 {
			return &Value{Kind: KindString, Str: ""}, nil
		}
		dataPos := pos + offset
		strLen := binary.LittleEndian.Uint32(data[dataPos:])
		s := string(data[dataPos+4 : dataPos+4+strLen])
		return &Value{Kind: KindString, Str: s}, nil

	case KindVec:
		if offset == 0 {
			return &Value{Kind: KindVec, Vec: nil}, nil
		}
		dataPos := pos + offset
		return unpackVec(fd, data, dataPos)

	case KindOptional:
		if offset == 1 {
			// Absent
			return &Value{Kind: KindOptional, Opt: nil}, nil
		}
		if offset == 0 {
			// Empty container wrapped in optional
			inner := zeroOptionalValue(fd)
			return &Value{Kind: KindOptional, Opt: &inner}, nil
		}
		dataPos := pos + offset
		inner, err := unpackOptionalPayload(fd, data, dataPos)
		if err != nil {
			return nil, err
		}
		return &Value{Kind: KindOptional, Opt: inner}, nil

	case KindObject:
		if offset == 0 {
			return &Value{Kind: KindObject}, nil
		}
		dataPos := pos + offset
		return unpackObject(fd.InnerDef, data, dataPos)

	case KindTuple:
		if offset == 0 {
			return &Value{Kind: KindTuple}, nil
		}
		dataPos := pos + offset
		return unpackTuple(fd.InnerDef, data, dataPos)

	case KindVariant:
		if offset == 0 {
			return &Value{Kind: KindVariant, Tag: 0}, nil
		}
		dataPos := pos + offset
		return unpackVariantPayload(fd.InnerDef, data, dataPos)

	default:
		return nil, fmt.Errorf("fracpack: unknown variable kind %d", fd.Kind)
	}
}

func unpackVec(fd *FieldDef, data []byte, pos uint32) (*Value, error) {
	fixedBytes := binary.LittleEndian.Uint32(data[pos:])
	if fixedBytes == 0 {
		return &Value{Kind: KindVec, Vec: nil}, nil
	}

	// Variable-size elements: each element is a 4-byte offset slot in the fixed region
	if fd.ElemDef != nil && len(fd.ElemDef.Fields) > 0 {
		// Vector of objects (variable-size): offset indirection
		count := fixedBytes / 4
		fixedStart := pos + 4
		elems := make([]Value, 0, count)
		for i := uint32(0); i < count; i++ {
			offPos := fixedStart + i*4
			offset := binary.LittleEndian.Uint32(data[offPos:])
			if offset == 0 {
				elems = append(elems, Value{Kind: KindObject})
				continue
			}
			target := offPos + offset
			elem, err := unpackObject(fd.ElemDef, data, target)
			if err != nil {
				return nil, err
			}
			elems = append(elems, *elem)
		}
		return &Value{Kind: KindVec, Vec: elems}, nil
	}

	elemKind := fd.ElemKind

	// String vector: each element is a 4-byte offset slot
	if elemKind == KindString {
		count := fixedBytes / 4
		fixedStart := pos + 4
		elems := make([]Value, count)
		for i := uint32(0); i < count; i++ {
			offPos := fixedStart + i*4
			offset := binary.LittleEndian.Uint32(data[offPos:])
			if offset == 0 {
				elems[i] = Value{Kind: KindString, Str: ""}
			} else {
				target := offPos + offset
				strLen := binary.LittleEndian.Uint32(data[target:])
				elems[i] = Value{Kind: KindString, Str: string(data[target+4 : target+4+strLen])}
			}
		}
		return &Value{Kind: KindVec, Vec: elems}, nil
	}

	// Optional vector: each element is a 4-byte offset slot
	if elemKind == KindOptional {
		count := fixedBytes / 4
		fixedStart := pos + 4
		elems := make([]Value, count)
		// Create a FieldDef for the optional element with the correct inner kind
		optFd := &FieldDef{
			Kind:     KindOptional,
			ElemKind: fd.OptInnerKind, // the T in Optional<T>; 0 defaults to KindU32 in unpackOptionalPayload
			ElemDef:  fd.ElemDef,
		}
		for i := uint32(0); i < count; i++ {
			offPos := fixedStart + i*4
			offset := binary.LittleEndian.Uint32(data[offPos:])
			if offset == 1 {
				elems[i] = Value{Kind: KindOptional, Opt: nil}
			} else if offset == 0 {
				inner := zeroOptionalValue(optFd)
				elems[i] = Value{Kind: KindOptional, Opt: &inner}
			} else {
				target := offPos + offset
				inner, err := unpackOptionalPayload(optFd, data, target)
				if err != nil {
					return nil, err
				}
				elems[i] = Value{Kind: KindOptional, Opt: inner}
			}
		}
		return &Value{Kind: KindVec, Vec: elems}, nil
	}

	// Nested vec: each element is a 4-byte offset slot
	if elemKind == KindVec {
		count := fixedBytes / 4
		fixedStart := pos + 4
		elems := make([]Value, count)
		innerFd := &FieldDef{Kind: KindVec, ElemKind: KindU32}
		for i := uint32(0); i < count; i++ {
			offPos := fixedStart + i*4
			offset := binary.LittleEndian.Uint32(data[offPos:])
			if offset == 0 {
				elems[i] = Value{Kind: KindVec}
			} else {
				target := offPos + offset
				ev, err := unpackVec(innerFd, data, target)
				if err != nil {
					return nil, err
				}
				elems[i] = *ev
			}
		}
		return &Value{Kind: KindVec, Vec: elems}, nil
	}

	// Fixed-size element vector (primitives)
	if elemKind == 0 {
		elemKind = KindU32 // default
	}
	elemSize := fieldFixedSize(elemKind)
	if elemSize == 0 {
		return &Value{Kind: KindVec}, nil
	}
	count := fixedBytes / elemSize
	elems := make([]Value, count)
	epos := pos + 4
	for i := uint32(0); i < count; i++ {
		elems[i] = readFixed(data, epos, elemKind)
		epos += elemSize
	}
	return &Value{Kind: KindVec, Vec: elems}, nil
}

func unpackOptionalPayload(fd *FieldDef, data []byte, pos uint32) (*Value, error) {
	// Optional<Object>
	if fd.ElemKind == KindObject && fd.ElemDef != nil {
		return unpackObject(fd.ElemDef, data, pos)
	}
	// Optional<String>
	if fd.ElemKind == KindString {
		strLen := binary.LittleEndian.Uint32(data[pos:])
		s := string(data[pos+4 : pos+4+strLen])
		return &Value{Kind: KindString, Str: s}, nil
	}
	// Optional<Vec>
	if fd.ElemKind == KindVec {
		innerFd := &FieldDef{Kind: KindVec, ElemKind: KindU32, ElemDef: fd.ElemDef}
		return unpackVec(innerFd, data, pos)
	}
	// Primitive optional — use ElemKind if set, else default to U32
	innerKind := fd.ElemKind
	if innerKind == 0 || innerKind == KindOptional {
		innerKind = KindU32
	}
	v := readFixed(data, pos, innerKind)
	return &v, nil
}

func unpackVariantPayload(td *TypeDef, data []byte, pos uint32) (*Value, error) {
	// Wire format: [u8 tag][u32 content_size][content_bytes]
	tag := data[pos]
	contentSize := binary.LittleEndian.Uint32(data[pos+1:])
	_ = contentSize
	val := &Value{Kind: KindVariant, Tag: tag}
	if int(tag) < len(td.Cases) {
		caseDef := &td.Cases[tag]
		if caseDef.InnerDef != nil {
			payload, err := unpackObject(caseDef.InnerDef, data, pos+5)
			if err != nil {
				return nil, err
			}
			val.Variant = payload
		} else if caseDef.Kind == KindString {
			strLen := binary.LittleEndian.Uint32(data[pos+5:])
			s := string(data[pos+9 : pos+9+strLen])
			val.Variant = &Value{Kind: KindString, Str: s}
		} else {
			payload := readFixed(data, pos+5, caseDef.Kind)
			val.Variant = &payload
		}
	}
	return val, nil
}

func unpackVariantTop(td *TypeDef, data []byte) (*Value, error) {
	return unpackVariantPayload(td, data, 0)
}

func zeroValue(kind FieldKind) Value {
	return Value{Kind: kind}
}

func zeroOptionalValue(fd *FieldDef) Value {
	if fd.ElemDef != nil {
		return Value{Kind: fd.ElemDef.Fields[0].Kind}
	}
	return Value{Kind: KindU32, U32: 0}
}

// packedSizeAt computes the packed byte size of an object at position pos.
func packedSizeAt(td *TypeDef, data []byte, pos uint32) uint32 {
	var headerSize uint32
	var fixedTotal uint32
	if td.Extensible {
		fixedTotal = uint32(binary.LittleEndian.Uint16(data[pos:]))
		headerSize = 2
	} else {
		fixedTotal = td.fixedSize()
	}

	heapEnd := pos + headerSize + fixedTotal
	fpos := pos + headerSize
	for _, fd := range td.Fields {
		if fpos-pos-headerSize >= fixedTotal {
			break
		}
		if fd.IsVar {
			offset := binary.LittleEndian.Uint32(data[fpos:])
			if offset > 1 {
				dataStart := fpos + offset
				// Approximate: for strings/vecs, add 4 + byte_count
				if dataStart+4 <= uint32(len(data)) {
					innerLen := binary.LittleEndian.Uint32(data[dataStart:])
					dataEnd := dataStart + 4 + innerLen
					if dataEnd > heapEnd {
						heapEnd = dataEnd
					}
				}
			}
		}
		fpos += fd.FixedSize
	}
	return heapEnd - pos
}
