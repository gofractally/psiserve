// In-place mutation system for fracpack packed data.
//
// Provides MutView that allows modifying individual fields of packed
// fracpack data without a full repack. Two modes are available:
//
// Canonical mode (default):
//
//	Mutations splice the buffer in place, shifting all subsequent bytes and
//	patching self-relative offsets of sibling variable-size fields. The
//	resulting buffer is always valid canonical fracpack.
//
// Fast mode:
//
//	Mutations that fit in the old slot overwrite in place. Mutations that
//	grow append to the end of the buffer and update the offset. Setting a
//	field to None sets the offset marker to 1 without reclaiming space.
//	Call Compact() to produce a canonical buffer.
package fracpack

import (
	"encoding/binary"
	"fmt"
)

// SpliceBuffer replaces data[pos:pos+oldLen] with newData, shifting the tail.
// Returns the new slice and the delta (positive = grew, negative = shrank).
func SpliceBuffer(data []byte, pos, oldLen int, newData []byte) ([]byte, int) {
	newLen := len(newData)
	delta := newLen - oldLen

	if delta == 0 {
		copy(data[pos:], newData)
		return data, 0
	}

	// Build the result: prefix + newData + tail
	tail := data[pos+oldLen:]
	result := make([]byte, 0, len(data)+delta)
	result = append(result, data[:pos]...)
	result = append(result, newData...)
	result = append(result, tail...)

	return result, delta
}

// PatchOffset adjusts a self-relative u32 offset at offsetPos after a splice.
// Only adjusts if the offset is a valid pointer (>= 4) and the target
// it points to is at or past splicePos.
func PatchOffset(data []byte, offsetPos int, splicePos int, delta int) {
	if offsetPos+4 > len(data) {
		return
	}
	offsetVal := binary.LittleEndian.Uint32(data[offsetPos:])
	if offsetVal <= 1 {
		// 0 = empty container, 1 = None marker -- never patch
		return
	}
	target := offsetPos + int(offsetVal)
	if target >= splicePos {
		newVal := int(offsetVal) + delta
		if newVal < 0 {
			newVal = 0
		}
		binary.LittleEndian.PutUint32(data[offsetPos:], uint32(newVal))
	}
}

// MutView wraps a []byte buffer and a *TypeDef, providing in-place
// mutation of fracpack-encoded data.
type MutView struct {
	data *[]byte // pointer to slice so we can grow/shrink it
	td   *TypeDef
	base int
	fast bool
}

// NewMutView creates a mutable view over fracpack-encoded data.
// The data slice may be modified in place. If fast is true, fast mode
// is used; otherwise canonical mode is used.
func NewMutView(data *[]byte, td *TypeDef, fast bool) *MutView {
	return &MutView{data: data, td: td, base: 0, fast: fast}
}

// NewMutViewAt creates a mutable view starting at a specific position.
func NewMutViewAt(data *[]byte, td *TypeDef, base int, fast bool) *MutView {
	return &MutView{data: data, td: td, base: base, fast: fast}
}

// Bytes returns the current buffer contents.
func (mv *MutView) Bytes() []byte {
	return *mv.data
}

// Get reads a field by name.
func (mv *MutView) Get(field string) (interface{}, error) {
	fd, fixedPos, err := mv.findField(field)
	if err != nil {
		return nil, err
	}

	data := *mv.data

	if !fd.IsVar && !isVariableKind(fd.Kind) {
		return readFixedAsInterface(data, uint32(fixedPos), fd.Kind), nil
	}

	val, err := readVarField(fd, data, uint32(fixedPos))
	if err != nil {
		return nil, err
	}
	return valueToInterface(val), nil
}

// Set writes a field (canonical mode).
func (mv *MutView) Set(field string, value interface{}) error {
	if mv.fast {
		return mv.setFast(field, value)
	}
	return mv.setCanonical(field, value)
}

// SetFast writes a field using fast mode regardless of MutView mode.
func (mv *MutView) SetFast(field string, value interface{}) error {
	return mv.setFast(field, value)
}

// Compact repacks the buffer to canonical form (removes dead bytes).
func (mv *MutView) Compact() []byte {
	val, err := Unpack(mv.td, *mv.data)
	if err != nil {
		return *mv.data
	}
	result := Pack(mv.td, val)
	*mv.data = result
	mv.base = 0
	return result
}

// findField locates a field by name and returns its definition and
// absolute fixed-region position.
func (mv *MutView) findField(name string) (*FieldDef, int, error) {
	data := *mv.data

	var headerSize int
	if mv.td.Extensible {
		headerSize = 2
	}

	pos := mv.base + headerSize
	for i := range mv.td.Fields {
		if mv.td.Fields[i].Name == name {
			// Check if the field is within the declared fixed region
			if mv.td.Extensible {
				declaredFixed := int(binary.LittleEndian.Uint16(data[mv.base:]))
				fieldOffset := pos - mv.base - headerSize
				if fieldOffset >= declaredFixed {
					// Field is elided
					return &mv.td.Fields[i], pos, nil
				}
			}
			return &mv.td.Fields[i], pos, nil
		}
		pos += int(mv.td.Fields[i].FixedSize)
	}
	return nil, 0, fmt.Errorf("fracpack: no field %q in %s", name, mv.td.Name)
}

// isFieldElided checks if a field is beyond the declared fixed region.
func (mv *MutView) isFieldElided(fieldFixedOffset int) bool {
	if !mv.td.Extensible {
		return false
	}
	data := *mv.data
	declaredFixed := int(binary.LittleEndian.Uint16(data[mv.base:]))
	return fieldFixedOffset >= declaredFixed
}

// fieldFixedOffset returns the offset of a field relative to the start
// of the fixed region (after the header).
func (mv *MutView) fieldFixedOffset(name string) int {
	offset := 0
	for i := range mv.td.Fields {
		if mv.td.Fields[i].Name == name {
			return offset
		}
		offset += int(mv.td.Fields[i].FixedSize)
	}
	return -1
}

// setCanonical implements canonical mode field mutation.
func (mv *MutView) setCanonical(field string, value interface{}) error {
	fd, fixedPos, err := mv.findField(field)
	if err != nil {
		return err
	}

	data := *mv.data

	// Fixed-size field: overwrite in place
	if !fd.IsVar && !isVariableKind(fd.Kind) {
		val := interfaceToValue(fd.Kind, value)
		writeFixed(data, uint32(fixedPos), fd.Kind, val)
		return nil
	}

	fieldOffset := mv.fieldFixedOffset(field)
	settingNone := value == nil && fd.Kind == KindOptional

	// Handle trailing optional elision
	if mv.isFieldElided(fieldOffset) {
		if settingNone {
			return nil // already elided = None
		}
		// Expand fixed region
		mv.expandFixedRegion(fd, fieldOffset)
		data = *mv.data
		// Recalculate fixedPos
		headerSize := 0
		if mv.td.Extensible {
			headerSize = 2
		}
		fixedPos = mv.base + headerSize + fieldOffset
	}

	oldOffset := binary.LittleEndian.Uint32(data[fixedPos:])

	if settingNone {
		if oldOffset <= 1 {
			binary.LittleEndian.PutUint32(data[fixedPos:], 1)
			return nil
		}
		// Remove existing heap data
		oldTarget := fixedPos + int(oldOffset)
		oldSize := mv.measureHeapData(fd, oldTarget)
		splicePos := oldTarget
		newData, delta := SpliceBuffer(data, splicePos, oldSize, nil)
		*mv.data = newData
		binary.LittleEndian.PutUint32(newData[fixedPos:], 1)
		mv.patchSiblings(field, splicePos, delta)
		return nil
	}

	// Check for empty container
	settingEmpty := isEmptyValue(fd, value)
	if settingEmpty {
		if oldOffset <= 1 {
			binary.LittleEndian.PutUint32(data[fixedPos:], 0)
			return nil
		}
		oldTarget := fixedPos + int(oldOffset)
		oldSize := mv.measureHeapData(fd, oldTarget)
		splicePos := oldTarget
		newData, delta := SpliceBuffer(data, splicePos, oldSize, nil)
		*mv.data = newData
		binary.LittleEndian.PutUint32(newData[fixedPos:], 0)
		mv.patchSiblings(field, splicePos, delta)
		return nil
	}

	// Pack new value
	newBytes := packFieldValue(fd, value)

	if oldOffset <= 1 {
		// No existing heap data -- append at end
		splicePos := len(data)
		newData, _ := SpliceBuffer(data, splicePos, 0, newBytes)
		*mv.data = newData
		binary.LittleEndian.PutUint32(newData[fixedPos:], uint32(splicePos-fixedPos))
		return nil
	}

	// Replace existing heap data
	oldTarget := fixedPos + int(oldOffset)
	oldSize := mv.measureHeapData(fd, oldTarget)
	splicePos := oldTarget
	newData, delta := SpliceBuffer(data, splicePos, oldSize, newBytes)
	*mv.data = newData
	// Offset stays the same because we spliced at the target position
	mv.patchSiblings(field, splicePos, delta)
	return nil
}

// setFast implements fast mode field mutation.
func (mv *MutView) setFast(field string, value interface{}) error {
	fd, fixedPos, err := mv.findField(field)
	if err != nil {
		return err
	}

	data := *mv.data

	// Fixed-size field: overwrite in place
	if !fd.IsVar && !isVariableKind(fd.Kind) {
		val := interfaceToValue(fd.Kind, value)
		writeFixed(data, uint32(fixedPos), fd.Kind, val)
		return nil
	}

	fieldOffset := mv.fieldFixedOffset(field)
	settingNone := value == nil && fd.Kind == KindOptional

	if settingNone {
		if mv.isFieldElided(fieldOffset) {
			return nil // already elided = None
		}
		binary.LittleEndian.PutUint32(data[fixedPos:], 1)
		return nil
	}

	// Handle trailing optional elision
	if mv.isFieldElided(fieldOffset) {
		mv.expandFixedRegion(fd, fieldOffset)
		data = *mv.data
		headerSize := 0
		if mv.td.Extensible {
			headerSize = 2
		}
		fixedPos = mv.base + headerSize + fieldOffset
	}

	oldOffset := binary.LittleEndian.Uint32(data[fixedPos:])

	// Check for empty container
	if isEmptyValue(fd, value) {
		binary.LittleEndian.PutUint32(data[fixedPos:], 0)
		return nil
	}

	// Pack new value
	newBytes := packFieldValue(fd, value)

	if oldOffset <= 1 {
		// No existing data -- append
		appendPos := len(data)
		*mv.data = append(data, newBytes...)
		binary.LittleEndian.PutUint32((*mv.data)[fixedPos:], uint32(appendPos-fixedPos))
		return nil
	}

	// Measure old data
	oldTarget := fixedPos + int(oldOffset)
	oldSize := mv.measureHeapData(fd, oldTarget)

	if len(newBytes) <= oldSize {
		// Fits -- overwrite in place
		copy(data[oldTarget:], newBytes)
		return nil
	}

	// Doesn't fit -- append to end, update offset
	appendPos := len(data)
	*mv.data = append(data, newBytes...)
	binary.LittleEndian.PutUint32((*mv.data)[fixedPos:], uint32(appendPos-fixedPos))
	return nil
}

// expandFixedRegion expands the extensible struct's fixed region to include
// the given field. Inserts zero-initialized offset slots.
func (mv *MutView) expandFixedRegion(fd *FieldDef, fieldOffset int) {
	data := *mv.data
	headerSize := 2 // only called for extensible structs

	declaredFixed := int(binary.LittleEndian.Uint16(data[mv.base:]))
	neededFixed := fieldOffset + int(fd.FixedSize)

	if neededFixed <= declaredFixed {
		return
	}

	insertPos := mv.base + headerSize + declaredFixed
	insertSize := neededFixed - declaredFixed

	// Build inserted bytes: set optional fields to offset 1 (None)
	insertBytes := make([]byte, insertSize)
	for i := range mv.td.Fields {
		foff := 0
		for j := 0; j < i; j++ {
			foff += int(mv.td.Fields[j].FixedSize)
		}
		if foff >= declaredFixed && foff < neededFixed {
			slotStart := foff - declaredFixed
			if mv.td.Fields[i].Kind == KindOptional {
				binary.LittleEndian.PutUint32(insertBytes[slotStart:], 1)
			}
		}
	}

	newData, delta := SpliceBuffer(data, insertPos, 0, insertBytes)
	*mv.data = newData

	// Update header
	binary.LittleEndian.PutUint16(newData[mv.base:], uint16(neededFixed))

	// Patch existing sibling offsets that point past the insert point
	for i := range mv.td.Fields {
		foff := 0
		for j := 0; j < i; j++ {
			foff += int(mv.td.Fields[j].FixedSize)
		}
		if foff >= declaredFixed {
			continue // just inserted
		}
		if !mv.td.Fields[i].IsVar && !isVariableKind(mv.td.Fields[i].Kind) {
			continue
		}
		siblingPos := mv.base + headerSize + foff
		PatchOffset(newData, siblingPos, insertPos, delta)
	}
}

// patchSiblings patches self-relative offsets of sibling variable-size fields
// after a splice operation.
func (mv *MutView) patchSiblings(modifiedField string, splicePos int, delta int) {
	if delta == 0 {
		return
	}
	data := *mv.data
	headerSize := 0
	if mv.td.Extensible {
		headerSize = 2
	}

	for i := range mv.td.Fields {
		if mv.td.Fields[i].Name == modifiedField {
			continue
		}
		if !mv.td.Fields[i].IsVar && !isVariableKind(mv.td.Fields[i].Kind) {
			continue
		}
		foff := 0
		for j := 0; j < i; j++ {
			foff += int(mv.td.Fields[j].FixedSize)
		}
		// Skip elided fields
		if mv.isFieldElided(foff) {
			continue
		}
		siblingPos := mv.base + headerSize + foff
		PatchOffset(data, siblingPos, splicePos, delta)
	}
}

// measureHeapData returns the byte size of heap data at the given position.
func (mv *MutView) measureHeapData(fd *FieldDef, pos int) int {
	data := *mv.data
	effKind := fd.Kind
	if fd.Kind == KindOptional {
		effKind = fd.ElemKind
	}

	switch effKind {
	case KindString:
		if pos+4 > len(data) {
			return 0
		}
		strLen := int(binary.LittleEndian.Uint32(data[pos:]))
		return 4 + strLen

	case KindVec:
		if pos+4 > len(data) {
			return 0
		}
		dataSize := int(binary.LittleEndian.Uint32(data[pos:]))
		// For simplicity, measure the vec as length prefix + fixed data
		// This works for primitive vecs. For variable-size element vecs,
		// we need to walk the heap data.
		if fd.ElemDef == nil || len(fd.ElemDef.Fields) == 0 {
			return 4 + dataSize
		}
		// Variable-size element vec: walk to find extent
		total := 4 + dataSize
		elemFix := 4 // offset slot
		count := dataSize / elemFix
		for i := 0; i < count; i++ {
			offPos := pos + 4 + i*4
			off := int(binary.LittleEndian.Uint32(data[offPos:]))
			if off > 1 {
				target := offPos + off
				innerSize := int(packedSizeAt(fd.ElemDef, data, uint32(target)))
				end := target + innerSize - pos
				if end > total {
					total = end
				}
			}
		}
		return total

	case KindObject:
		if fd.InnerDef != nil || fd.ElemDef != nil {
			def := fd.InnerDef
			if def == nil {
				def = fd.ElemDef
			}
			return int(packedSizeAt(def, data, uint32(pos)))
		}
		return 0

	case KindVariant:
		if pos+5 > len(data) {
			return 0
		}
		dataSize := int(binary.LittleEndian.Uint32(data[pos+1:]))
		return 5 + dataSize

	case KindTuple:
		if fd.InnerDef != nil {
			return int(packedSizeAt(fd.InnerDef, data, uint32(pos)))
		}
		return 0

	default:
		// Fixed-size
		return int(fieldFixedSize(effKind))
	}
}

// ── Value conversion helpers ────────────────────────────────────────────────

// readFixedAsInterface reads a fixed-size value at pos and returns it
// as a Go interface{}.
func readFixedAsInterface(data []byte, pos uint32, kind FieldKind) interface{} {
	v := readFixed(data, pos, kind)
	return valueToInterface(&v)
}

// valueToInterface converts a Value to a Go interface{}.
func valueToInterface(v *Value) interface{} {
	switch v.Kind {
	case KindBool:
		return v.Bool
	case KindU8:
		return v.U8
	case KindI8:
		return v.I8
	case KindU16:
		return v.U16
	case KindI16:
		return v.I16
	case KindU32:
		return v.U32
	case KindI32:
		return v.I32
	case KindU64:
		return v.U64
	case KindI64:
		return v.I64
	case KindF32:
		return v.F32
	case KindF64:
		return v.F64
	case KindString:
		return v.Str
	case KindOptional:
		if v.Opt == nil {
			return nil
		}
		return valueToInterface(v.Opt)
	case KindVec:
		result := make([]interface{}, len(v.Vec))
		for i := range v.Vec {
			result[i] = valueToInterface(&v.Vec[i])
		}
		return result
	case KindObject:
		result := make(map[string]interface{})
		// Cannot populate field names without TypeDef, return Fields
		for i := range v.Fields {
			result[fmt.Sprintf("field_%d", i)] = valueToInterface(&v.Fields[i])
		}
		return result
	case KindTuple:
		result := make([]interface{}, len(v.Tuple))
		for i := range v.Tuple {
			result[i] = valueToInterface(&v.Tuple[i])
		}
		return result
	default:
		return nil
	}
}

// interfaceToValue converts a Go interface{} to a *Value for a given kind.
func interfaceToValue(kind FieldKind, v interface{}) *Value {
	val := &Value{Kind: kind}
	if v == nil {
		return val
	}
	switch kind {
	case KindBool:
		val.Bool = v.(bool)
	case KindU8:
		val.U8 = toU8(v)
	case KindI8:
		val.I8 = toI8(v)
	case KindU16:
		val.U16 = toU16(v)
	case KindI16:
		val.I16 = toI16(v)
	case KindU32:
		val.U32 = toU32(v)
	case KindI32:
		val.I32 = toI32(v)
	case KindU64:
		val.U64 = toU64(v)
	case KindI64:
		val.I64 = toI64(v)
	case KindF32:
		val.F32 = toF32(v)
	case KindF64:
		val.F64 = toF64(v)
	case KindString:
		val.Str = v.(string)
	}
	return val
}

// packFieldValue packs a field value into standalone bytes suitable for
// heap storage. For optional fields, it packs the inner value.
func packFieldValue(fd *FieldDef, value interface{}) []byte {
	val := interfaceToValue(fd.Kind, value)
	if fd.Kind == KindOptional && value != nil {
		// Pack the inner value
		innerVal := interfaceToValue(fd.ElemKind, value)
		innerFd := &FieldDef{Kind: fd.ElemKind, InnerDef: fd.ElemDef, ElemDef: fd.ElemDef}
		return packHeapField(innerFd, innerVal)
	}
	if fd.Kind == KindString {
		if val.Str == "" {
			return nil
		}
		b := make([]byte, 4+len(val.Str))
		binary.LittleEndian.PutUint32(b[0:4], uint32(len(val.Str)))
		copy(b[4:], val.Str)
		return b
	}
	return packHeapField(fd, val)
}

// isEmptyValue checks if a value represents an empty container.
func isEmptyValue(fd *FieldDef, value interface{}) bool {
	if value == nil {
		return false
	}
	switch fd.Kind {
	case KindString:
		s, ok := value.(string)
		return ok && s == ""
	case KindVec:
		// Check for empty slice
		if arr, ok := value.([]interface{}); ok {
			return len(arr) == 0
		}
	case KindOptional:
		// For optional, check the inner type
		return isEmptyValue(&FieldDef{Kind: fd.ElemKind}, value)
	}
	return false
}

// ── Numeric conversion helpers ──────────────────────────────────────────────

func toU8(v interface{}) uint8 {
	switch x := v.(type) {
	case uint8:
		return x
	case int:
		return uint8(x)
	case uint32:
		return uint8(x)
	case uint64:
		return uint8(x)
	case float64:
		return uint8(x)
	default:
		return 0
	}
}

func toI8(v interface{}) int8 {
	switch x := v.(type) {
	case int8:
		return x
	case int:
		return int8(x)
	case float64:
		return int8(x)
	default:
		return 0
	}
}

func toU16(v interface{}) uint16 {
	switch x := v.(type) {
	case uint16:
		return x
	case int:
		return uint16(x)
	case uint32:
		return uint16(x)
	case float64:
		return uint16(x)
	default:
		return 0
	}
}

func toI16(v interface{}) int16 {
	switch x := v.(type) {
	case int16:
		return x
	case int:
		return int16(x)
	case float64:
		return int16(x)
	default:
		return 0
	}
}

func toU32(v interface{}) uint32 {
	switch x := v.(type) {
	case uint32:
		return x
	case int:
		return uint32(x)
	case uint64:
		return uint32(x)
	case float64:
		return uint32(x)
	default:
		return 0
	}
}

func toI32(v interface{}) int32 {
	switch x := v.(type) {
	case int32:
		return x
	case int:
		return int32(x)
	case float64:
		return int32(x)
	default:
		return 0
	}
}

func toU64(v interface{}) uint64 {
	switch x := v.(type) {
	case uint64:
		return x
	case int:
		return uint64(x)
	case uint32:
		return uint64(x)
	case float64:
		return uint64(x)
	default:
		return 0
	}
}

func toI64(v interface{}) int64 {
	switch x := v.(type) {
	case int64:
		return x
	case int:
		return int64(x)
	case float64:
		return int64(x)
	default:
		return 0
	}
}

func toF32(v interface{}) float32 {
	switch x := v.(type) {
	case float32:
		return x
	case float64:
		return float32(x)
	case int:
		return float32(x)
	default:
		return 0
	}
}

func toF64(v interface{}) float64 {
	switch x := v.(type) {
	case float64:
		return x
	case float32:
		return float64(x)
	case int:
		return float64(x)
	default:
		return 0
	}
}
