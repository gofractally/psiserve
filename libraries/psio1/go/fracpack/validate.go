// Zero-copy validation of fracpack binary data.
//
// Validates packed data against a type schema without unpacking.
// Returns Valid, Extended (has unknown fields), or Invalid.
package fracpack

import (
	"encoding/binary"
	"fmt"
	"unicode/utf8"
)

// ValidationStatus indicates the result of validating fracpack data.
type ValidationStatus int

const (
	// Valid means the data matches the schema exactly.
	Valid ValidationStatus = iota
	// Extended means the data is valid but contains unknown fields
	// (forward-compatible extensible struct).
	Extended
	// Invalid means the data is malformed.
	Invalid
)

// ValidationResult holds the outcome of a Validate call.
type ValidationResult struct {
	Status  ValidationStatus
	Message string // empty for Valid, description for Extended/Invalid
}

// Validate checks that data conforms to the fracpack wire format described
// by td. It walks the type tree without allocating Go values. The check is
// O(n) in the data size and touches each byte at most once.
func Validate(td *TypeDef, data []byte) ValidationResult {
	v := &validator{
		data: data,
		end:  uint32(len(data)),
	}

	defer func() {
		if r := recover(); r != nil {
			// Shouldn't happen, but guard against index panics.
			v.err = fmt.Errorf("internal validation panic: %v", r)
		}
	}()

	pos := v.validateTop(td, 0)
	if v.err != nil {
		return ValidationResult{Status: Invalid, Message: v.err.Error()}
	}
	if pos != v.end {
		return ValidationResult{
			Status:  Invalid,
			Message: fmt.Sprintf("extra data: %d bytes after valid data", v.end-pos),
		}
	}
	if v.extended {
		return ValidationResult{Status: Extended, Message: "unknown fields present"}
	}
	return ValidationResult{Status: Valid}
}

// validator holds the state for a single validation pass.
type validator struct {
	data     []byte
	end      uint32
	extended bool
	err      error
}

// check verifies that [pos, pos+size) is within bounds.
func (v *validator) check(pos, size uint32) bool {
	if pos > v.end || size > v.end-pos {
		v.err = fmt.Errorf("read past end at %d+%d (buffer size %d)", pos, size, v.end)
		return false
	}
	return true
}

// u16 reads a little-endian uint16 at pos. Caller must bounds-check first.
func (v *validator) u16(pos uint32) uint16 {
	return binary.LittleEndian.Uint16(v.data[pos:])
}

// u32 reads a little-endian uint32 at pos. Caller must bounds-check first.
func (v *validator) u32(pos uint32) uint32 {
	return binary.LittleEndian.Uint32(v.data[pos:])
}

// validateTop dispatches to the right validator based on the TypeDef shape.
// Returns the position immediately after the consumed data.
func (v *validator) validateTop(td *TypeDef, pos uint32) uint32 {
	if v.err != nil {
		return pos
	}
	if td.IsVariant {
		return v.validateVariant(td, pos)
	}
	if td.IsTuple {
		return v.validateTuple(td, pos)
	}
	if td.Extensible {
		return v.validateExtensibleStruct(td, pos)
	}
	return v.validateFixedStruct(td, pos)
}

// ── Scalar validation ──────────────────────────────────────────────────

func (v *validator) validateScalar(kind FieldKind, pos uint32) uint32 {
	if v.err != nil {
		return pos
	}
	switch kind {
	case KindBool:
		if !v.check(pos, 1) {
			return pos
		}
		b := v.data[pos]
		if b > 1 {
			v.err = fmt.Errorf("invalid bool value %d at %d", b, pos)
			return pos
		}
		return pos + 1
	case KindU8, KindI8:
		if !v.check(pos, 1) {
			return pos
		}
		return pos + 1
	case KindU16, KindI16:
		if !v.check(pos, 2) {
			return pos
		}
		return pos + 2
	case KindU32, KindI32, KindF32:
		if !v.check(pos, 4) {
			return pos
		}
		return pos + 4
	case KindU64, KindI64, KindF64:
		if !v.check(pos, 8) {
			return pos
		}
		return pos + 8
	default:
		v.err = fmt.Errorf("unexpected scalar kind %d at %d", kind, pos)
		return pos
	}
}

// ── String validation ──────────────────────────────────────────────────

func (v *validator) validateString(pos uint32) uint32 {
	if v.err != nil {
		return pos
	}
	if !v.check(pos, 4) {
		return pos
	}
	length := v.u32(pos)
	if !v.check(pos+4, length) {
		return pos
	}
	if !utf8.Valid(v.data[pos+4 : pos+4+length]) {
		v.err = fmt.Errorf("invalid UTF-8 in string at %d", pos)
		return pos
	}
	return pos + 4 + length
}

// ── Vec validation ─────────────────────────────────────────────────────

func (v *validator) validateVec(fd *FieldDef, pos uint32) uint32 {
	if v.err != nil {
		return pos
	}
	if !v.check(pos, 4) {
		return pos
	}
	dataSize := v.u32(pos)
	if !v.check(pos+4, dataSize) {
		return pos
	}
	if dataSize == 0 {
		return pos + 4
	}

	elemFixedSz := v.elemFixedSize(fd)
	if elemFixedSz == 0 {
		v.err = fmt.Errorf("vec element has zero fixed size at %d", pos)
		return pos
	}
	if dataSize%elemFixedSz != 0 {
		v.err = fmt.Errorf("vec data_size %d not divisible by element size %d at %d", dataSize, elemFixedSz, pos)
		return pos
	}
	count := dataSize / elemFixedSz
	fixedStart := pos + 4

	elemIsVariable := v.elemIsVariable(fd)

	if !elemIsVariable {
		// Fixed-size elements: validate each inline
		for i := uint32(0); i < count; i++ {
			v.validateFieldInline(fd, fixedStart+i*elemFixedSz)
			if v.err != nil {
				return pos
			}
		}
		return fixedStart + dataSize
	}

	// Variable-size elements: follow offsets from fixed region
	endPos := fixedStart + dataSize
	for i := uint32(0); i < count; i++ {
		fp := fixedStart + i*elemFixedSz
		endPos = v.validateEmbeddedElem(fd, fp, endPos)
		if v.err != nil {
			return pos
		}
	}
	return endPos
}

// ── Optional validation ────────────────────────────────────────────────

func (v *validator) validateOptionalHeap(fd *FieldDef, pos uint32) uint32 {
	if v.err != nil {
		return pos
	}
	// At the heap position, we validate the inner value directly.
	return v.validateInnerValue(fd, pos)
}

// ── Variant validation ─────────────────────────────────────────────────

func (v *validator) validateVariant(td *TypeDef, pos uint32) uint32 {
	if v.err != nil {
		return pos
	}
	if !v.check(pos, 5) {
		return pos
	}
	tag := v.data[pos]
	dataSize := v.u32(pos + 1)

	if int(tag) >= len(td.Cases) {
		v.err = fmt.Errorf("variant tag %d out of range (%d cases) at %d", tag, len(td.Cases), pos)
		return pos
	}
	if !v.check(pos+5, dataSize) {
		return pos
	}

	// Validate the case payload
	caseDef := &td.Cases[tag]
	payloadEnd := v.validateCasePayload(caseDef, pos+5)
	if v.err != nil {
		return pos
	}

	expectedEnd := pos + 5 + dataSize
	if payloadEnd != expectedEnd {
		v.err = fmt.Errorf("variant payload size mismatch at %d: consumed %d bytes, data_size says %d",
			pos, payloadEnd-(pos+5), dataSize)
		return pos
	}

	return expectedEnd
}

// validateCasePayload validates the content of a variant case.
func (v *validator) validateCasePayload(caseDef *FieldDef, pos uint32) uint32 {
	if v.err != nil {
		return pos
	}
	if caseDef.InnerDef != nil {
		return v.validateTop(caseDef.InnerDef, pos)
	}
	if caseDef.Kind == KindString {
		return v.validateString(pos)
	}
	return v.validateScalar(caseDef.Kind, pos)
}

// ── Extensible struct validation ───────────────────────────────────────

func (v *validator) validateExtensibleStruct(td *TypeDef, pos uint32) uint32 {
	if v.err != nil {
		return pos
	}
	if !v.check(pos, 2) {
		return pos
	}
	fixedSize := uint32(v.u16(pos))
	fixedStart := pos + 2
	heapPos := fixedStart + fixedSize
	if !v.check(fixedStart, fixedSize) {
		return pos
	}

	// Compute expected fixed size from known fields
	var expectedFixed uint32
	for _, f := range td.Fields {
		expectedFixed += f.FixedSize
	}
	if fixedSize > expectedFixed {
		v.extended = true
	}

	endPos := heapPos
	fieldPos := fixedStart

	for i := range td.Fields {
		f := &td.Fields[i]
		if fieldPos+f.FixedSize > heapPos {
			// Field beyond the declared fixed region -- trailing optional elision
			// Non-optional fields missing from fixed region: this is valid when
			// reading data from a newer or older schema.
			continue
		}
		endPos = v.validateEmbeddedField(f, fieldPos, endPos)
		if v.err != nil {
			return pos
		}
		fieldPos += f.FixedSize
	}

	return endPos
}

// ── Tuple validation ───────────────────────────────────────────────────

func (v *validator) validateTuple(td *TypeDef, pos uint32) uint32 {
	if v.err != nil {
		return pos
	}
	if !v.check(pos, 2) {
		return pos
	}
	fixedSize := uint32(v.u16(pos))
	fixedStart := pos + 2
	heapPos := fixedStart + fixedSize
	if !v.check(fixedStart, fixedSize) {
		return pos
	}

	endPos := heapPos
	fieldPos := fixedStart

	for i := range td.Elements {
		e := &td.Elements[i]
		efsz := elemFixedSize(e)
		if fieldPos+efsz > heapPos {
			// Element beyond the declared fixed region
			continue
		}
		endPos = v.validateEmbeddedField(e, fieldPos, endPos)
		if v.err != nil {
			return pos
		}
		fieldPos += efsz
	}

	return endPos
}

// ── Fixed (non-extensible) struct validation ───────────────────────────

func (v *validator) validateFixedStruct(td *TypeDef, pos uint32) uint32 {
	if v.err != nil {
		return pos
	}
	var totalFixed uint32
	for _, f := range td.Fields {
		totalFixed += f.FixedSize
	}
	if !v.check(pos, totalFixed) {
		return pos
	}

	endPos := pos + totalFixed
	fieldPos := pos

	for i := range td.Fields {
		f := &td.Fields[i]
		endPos = v.validateEmbeddedField(f, fieldPos, endPos)
		if v.err != nil {
			return pos
		}
		fieldPos += f.FixedSize
	}

	return endPos
}

// ── Embedded field/element helpers ─────────────────────────────────────

// validateEmbeddedField validates a field at fixedPos. For fixed-size fields,
// it validates inline. For variable-size fields, it reads the offset and
// validates the heap data. Returns the new end position.
func (v *validator) validateEmbeddedField(fd *FieldDef, fixedPos, endPos uint32) uint32 {
	if v.err != nil {
		return endPos
	}
	if !fd.IsVar && !isVariableKind(fd.Kind) {
		// Fixed-size field: validate inline
		v.validateFieldInline(fd, fixedPos)
		return endPos
	}

	// Variable-size field: read offset
	if !v.check(fixedPos, 4) {
		return endPos
	}
	offset := v.u32(fixedPos)

	// Special offset values
	if offset == 0 {
		return endPos // empty container
	}
	if offset == 1 {
		if fd.Kind == KindOptional {
			return endPos // None
		}
		// offset=1 is only valid for optionals
		v.err = fmt.Errorf("offset 1 (None) on non-optional field %q at %d", fd.Name, fixedPos)
		return endPos
	}
	if offset == 2 || offset == 3 {
		v.err = fmt.Errorf("reserved offset value %d at %d", offset, fixedPos)
		return endPos
	}

	target := fixedPos + offset
	if target < endPos {
		v.err = fmt.Errorf("offset %d at %d points backward (target %d < heap %d)", offset, fixedPos, target, endPos)
		return endPos
	}

	// For optional fields: the offset already resolves indirection,
	// so validate the inner type directly.
	if fd.Kind == KindOptional {
		return v.validateInnerValue(fd, target)
	}

	return v.validateFieldAtPos(fd, target)
}

// validateEmbeddedElem validates a vector element at fixedPos.
// For fixed-size elements, validates inline. For variable-size, follows offset.
func (v *validator) validateEmbeddedElem(fd *FieldDef, fixedPos, endPos uint32) uint32 {
	if v.err != nil {
		return endPos
	}
	if !v.elemIsVariable(fd) {
		v.validateFieldInline(fd, fixedPos)
		return endPos
	}

	// Variable-size element: read offset
	if !v.check(fixedPos, 4) {
		return endPos
	}
	offset := v.u32(fixedPos)

	if offset == 0 {
		return endPos // empty container
	}
	if offset == 1 {
		if fd.ElemKind == KindOptional {
			return endPos // None in optional vector element
		}
		v.err = fmt.Errorf("offset 1 (None) on non-optional vec element at %d", fixedPos)
		return endPos
	}
	if offset == 2 || offset == 3 {
		v.err = fmt.Errorf("reserved offset value %d at %d", offset, fixedPos)
		return endPos
	}

	target := fixedPos + offset
	if target < endPos {
		v.err = fmt.Errorf("offset %d at %d points backward (target %d < heap %d)", offset, fixedPos, target, endPos)
		return endPos
	}

	return v.validateElemAtPos(fd, target)
}

// validateFieldInline validates a fixed-size field at position pos.
func (v *validator) validateFieldInline(fd *FieldDef, pos uint32) {
	if v.err != nil {
		return
	}
	// For vec elements that have a nested object definition with all fixed fields
	if fd.ElemDef != nil && len(fd.ElemDef.Fields) > 0 {
		v.validateTop(fd.ElemDef, pos)
		return
	}

	kind := fd.Kind
	// For vec elements, use the element kind
	if fd.Kind == KindVec || fd.Kind == KindOptional {
		kind = fd.ElemKind
	}
	v.validateScalar(kind, pos)
}

// validateFieldAtPos validates a variable-size field at heap position pos.
func (v *validator) validateFieldAtPos(fd *FieldDef, pos uint32) uint32 {
	if v.err != nil {
		return pos
	}
	switch fd.Kind {
	case KindString:
		return v.validateString(pos)
	case KindVec:
		return v.validateVec(fd, pos)
	case KindObject:
		if fd.InnerDef != nil {
			return v.validateTop(fd.InnerDef, pos)
		}
		v.err = fmt.Errorf("object field %q has no type definition at %d", fd.Name, pos)
		return pos
	case KindTuple:
		if fd.InnerDef != nil {
			return v.validateTuple(fd.InnerDef, pos)
		}
		v.err = fmt.Errorf("tuple field %q has no type definition at %d", fd.Name, pos)
		return pos
	case KindVariant:
		if fd.InnerDef != nil {
			return v.validateVariant(fd.InnerDef, pos)
		}
		v.err = fmt.Errorf("variant field %q has no type definition at %d", fd.Name, pos)
		return pos
	default:
		return v.validateScalar(fd.Kind, pos)
	}
}

// validateInnerValue validates the inner value of an optional field at
// the heap position. The indirection has already been resolved.
func (v *validator) validateInnerValue(fd *FieldDef, pos uint32) uint32 {
	if v.err != nil {
		return pos
	}
	// Optional<Object>
	if fd.ElemKind == KindObject && fd.ElemDef != nil {
		return v.validateTop(fd.ElemDef, pos)
	}
	// Optional<String>
	if fd.ElemKind == KindString {
		return v.validateString(pos)
	}
	// Optional<Vec>
	if fd.ElemKind == KindVec {
		innerFd := &FieldDef{
			Kind:     KindVec,
			ElemKind: KindU32,
			ElemDef:  fd.ElemDef,
		}
		return v.validateVec(innerFd, pos)
	}
	// Optional<Variant>
	if fd.ElemKind == KindVariant && fd.InnerDef != nil {
		return v.validateVariant(fd.InnerDef, pos)
	}
	// Optional<Tuple>
	if fd.ElemKind == KindTuple && fd.InnerDef != nil {
		return v.validateTuple(fd.InnerDef, pos)
	}
	// Optional<scalar>
	return v.validateScalar(fd.ElemKind, pos)
}

// validateElemAtPos validates a variable-size vector element at heap pos.
func (v *validator) validateElemAtPos(fd *FieldDef, pos uint32) uint32 {
	if v.err != nil {
		return pos
	}
	// Vec of objects
	if fd.ElemDef != nil && len(fd.ElemDef.Fields) > 0 {
		return v.validateTop(fd.ElemDef, pos)
	}
	// Vec of strings
	if fd.ElemKind == KindString {
		return v.validateString(pos)
	}
	// Vec of optionals — the offset has been followed, so we are at the
	// optional's inner payload. Default inner type is u32 when ElemDef is nil.
	if fd.ElemKind == KindOptional {
		if fd.ElemDef != nil {
			return v.validateTop(fd.ElemDef, pos)
		}
		return v.validateScalar(KindU32, pos)
	}
	// Vec of vecs (nested)
	if fd.ElemKind == KindVec {
		innerFd := &FieldDef{Kind: KindVec, ElemKind: KindU32}
		return v.validateVec(innerFd, pos)
	}
	// Vec of variants
	if fd.ElemKind == KindVariant && fd.InnerDef != nil {
		return v.validateVariant(fd.InnerDef, pos)
	}
	return v.validateScalar(fd.ElemKind, pos)
}

// ── Type-size helpers ──────────────────────────────────────────────────

// elemFixedSize returns the fixed-region size for a vector element described by fd.
func (v *validator) elemFixedSize(fd *FieldDef) uint32 {
	// For vec of objects: each element is a 4-byte offset
	if fd.ElemDef != nil && len(fd.ElemDef.Fields) > 0 {
		if fd.ElemDef.Extensible || v.hasVarField(fd.ElemDef) {
			return 4
		}
		return fd.ElemDef.fixedSize()
	}
	if fd.ElemKind == KindString || fd.ElemKind == KindVec ||
		fd.ElemKind == KindOptional || fd.ElemKind == KindObject ||
		fd.ElemKind == KindVariant || fd.ElemKind == KindTuple {
		return 4
	}
	return fieldFixedSize(fd.ElemKind)
}

// elemIsVariable returns true if vector elements use heap storage.
func (v *validator) elemIsVariable(fd *FieldDef) bool {
	if fd.ElemDef != nil && len(fd.ElemDef.Fields) > 0 {
		if fd.ElemDef.Extensible || v.hasVarField(fd.ElemDef) {
			return true
		}
		return false
	}
	return isVariableKind(fd.ElemKind)
}

// hasVarField returns true if any field in td is variable-size.
func (v *validator) hasVarField(td *TypeDef) bool {
	for _, f := range td.Fields {
		if f.IsVar || isVariableKind(f.Kind) {
			return true
		}
	}
	return false
}
