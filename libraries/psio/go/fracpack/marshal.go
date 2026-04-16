// Reflection-based typed fracpack marshal/unmarshal.
//
// Usage:
//
//	type Inner struct {
//	    Value uint32 `fracpack:"value"`
//	    Label string `fracpack:"label"`
//	}
//
//	data, err := fracpack.Marshal(inner)
//	var out Inner
//	err = fracpack.Unmarshal(data, &out)
//
// By default, structs are extensible (Object with u16 header). Embed
// fracpack.Fixed to mark a struct as non-extensible (Struct).
//
// Optional fields use Go pointers: *uint32 maps to Optional<u32>.
// Variant types embed fracpack.Variant; each case is a pointer field
// and exactly one must be non-nil.
package fracpack

import (
	"encoding/binary"
	"fmt"
	"math"
	"reflect"
)

// Fixed is a zero-size marker. Embed it to mark a struct as non-extensible.
type Fixed struct{}

// Variant is a zero-size marker. Embed it to mark a struct as a variant.
//
//	type DataVariant struct {
//	    fracpack.Variant
//	    Uint32 *uint32 `fracpack:"uint32"`
//	    String *string `fracpack:"string"`
//	    Inner  *Inner  `fracpack:"Inner"`
//	}
type Variant struct{}

var fixedType = reflect.TypeOf(Fixed{})
var variantType = reflect.TypeOf(Variant{})

// ── Public API ──────────────────────────────────────────────────────────

// Marshal serializes v into fracpack bytes.
func Marshal(v any) ([]byte, error) {
	rv := reflect.ValueOf(v)
	if rv.Kind() == reflect.Ptr {
		rv = rv.Elem()
	}
	e := &encoder{}
	if isVariantStruct(rv.Type()) {
		if err := e.variant(rv); err != nil {
			return nil, err
		}
	} else {
		if err := e.object(rv); err != nil {
			return nil, err
		}
	}
	return e.buf, nil
}

// Unmarshal decodes fracpack data into v, which must be a non-nil pointer.
func Unmarshal(data []byte, v any) error {
	rv := reflect.ValueOf(v)
	if rv.Kind() != reflect.Ptr || rv.IsNil() {
		return fmt.Errorf("fracpack: Unmarshal requires non-nil pointer")
	}
	return decodeObject(data, 0, rv.Elem())
}

// ── Type helpers ────────────────────────────────────────────────────────

func isFixedStruct(t reflect.Type) bool {
	if t.Kind() != reflect.Struct {
		return false
	}
	for i := 0; i < t.NumField(); i++ {
		if t.Field(i).Anonymous && t.Field(i).Type == fixedType {
			return true
		}
	}
	return false
}

func isVariantStruct(t reflect.Type) bool {
	if t.Kind() != reflect.Struct {
		return false
	}
	for i := 0; i < t.NumField(); i++ {
		if t.Field(i).Anonymous && t.Field(i).Type == variantType {
			return true
		}
	}
	return false
}

type fieldMeta struct {
	idx int // index in reflect struct
	sf  reflect.StructField
}

func getFields(t reflect.Type) []fieldMeta {
	var out []fieldMeta
	for i := 0; i < t.NumField(); i++ {
		f := t.Field(i)
		if f.Anonymous {
			continue
		}
		if f.Tag.Get("fracpack") == "-" {
			continue
		}
		out = append(out, fieldMeta{idx: i, sf: f})
	}
	return out
}

func fFixedSz(t reflect.Type) uint32 {
	switch t.Kind() {
	case reflect.Bool, reflect.Uint8, reflect.Int8:
		return 1
	case reflect.Uint16, reflect.Int16:
		return 2
	case reflect.Uint32, reflect.Int32, reflect.Float32:
		return 4
	case reflect.Uint64, reflect.Int64, reflect.Float64:
		return 8
	case reflect.String, reflect.Slice, reflect.Ptr, reflect.Struct:
		return 4
	case reflect.Array:
		return uint32(t.Len()) * fFixedSz(t.Elem())
	default:
		return 0
	}
}

func isVarType(t reflect.Type) bool {
	switch t.Kind() {
	case reflect.String, reflect.Slice, reflect.Ptr, reflect.Struct:
		return true
	default:
		return false
	}
}

func isScalarK(k reflect.Kind) bool {
	switch k {
	case reflect.Bool, reflect.Uint8, reflect.Int8,
		reflect.Uint16, reflect.Int16,
		reflect.Uint32, reflect.Int32, reflect.Float32,
		reflect.Uint64, reflect.Int64, reflect.Float64:
		return true
	default:
		return false
	}
}

// ── Encoder ─────────────────────────────────────────────────────────────

type encoder struct {
	buf []byte
}

func (e *encoder) pos() uint32    { return uint32(len(e.buf)) }
func (e *encoder) zeros(n uint32) { e.buf = append(e.buf, make([]byte, n)...) }
func (e *encoder) raw(d []byte)   { e.buf = append(e.buf, d...) }

func (e *encoder) setU16(p uint32, v uint16) { binary.LittleEndian.PutUint16(e.buf[p:], v) }
func (e *encoder) setU32(p uint32, v uint32) { binary.LittleEndian.PutUint32(e.buf[p:], v) }

func (e *encoder) object(v reflect.Value) error {
	t := v.Type()
	ext := !isFixedStruct(t)
	fields := getFields(t)

	var headerSz uint32
	if ext {
		headerSz = 2
	}

	fullFixed := uint32(0)
	for _, fi := range fields {
		fullFixed += fFixedSz(fi.sf.Type)
	}

	actualFixed := fullFixed
	if ext {
		actualFixed = computeActualFixedRefl(v, fields)
	}

	start := e.pos()
	e.zeros(headerSz + actualFixed)
	if ext {
		e.setU16(start, uint16(actualFixed))
	}

	// Pass 1: scalars & arrays
	off := uint32(0)
	for _, fi := range fields {
		fsz := fFixedSz(fi.sf.Type)
		if off >= actualFixed {
			break
		}
		fv := v.Field(fi.idx)
		if !isVarType(fi.sf.Type) {
			if fi.sf.Type.Kind() == reflect.Array {
				writeArr(e.buf, start+headerSz+off, fv)
			} else {
				writeScalar(e.buf, start+headerSz+off, fv)
			}
		}
		off += fsz
	}

	// Pass 2: var fields
	off = 0
	for _, fi := range fields {
		fsz := fFixedSz(fi.sf.Type)
		if off >= actualFixed {
			break
		}
		if isVarType(fi.sf.Type) {
			slot := start + headerSz + off
			fv := v.Field(fi.idx)
			before := e.pos()
			if err := e.varField(fv); err != nil {
				return err
			}
			after := e.pos()
			hLen := after - before
			if hLen == 0 {
				if fi.sf.Type.Kind() == reflect.Ptr && fv.IsNil() {
					e.setU32(slot, 1) // absent
				} else {
					e.setU32(slot, 0) // empty
				}
			} else {
				e.setU32(slot, before-slot)
			}
		}
		off += fsz
	}

	return nil
}

func computeActualFixedRefl(v reflect.Value, fields []fieldMeta) uint32 {
	last := -1
	for i, fi := range fields {
		fv := v.Field(fi.idx)
		if fi.sf.Type.Kind() == reflect.Ptr {
			if !fv.IsNil() {
				last = i
			}
		} else {
			last = i
		}
	}
	if last < 0 {
		return 0
	}
	var sz uint32
	for i := 0; i <= last; i++ {
		sz += fFixedSz(fields[i].sf.Type)
	}
	return sz
}

func (e *encoder) varField(fv reflect.Value) error {
	t := fv.Type()
	switch t.Kind() {
	case reflect.String:
		s := fv.String()
		if len(s) == 0 {
			return nil
		}
		start := e.pos()
		e.zeros(4)
		e.setU32(start, uint32(len(s)))
		e.raw([]byte(s))

	case reflect.Slice:
		if fv.Len() == 0 {
			return nil
		}
		return e.vec(t.Elem(), fv)

	case reflect.Ptr:
		if fv.IsNil() {
			return nil
		}
		return e.optPayload(fv.Elem())

	case reflect.Struct:
		if isVariantStruct(t) {
			return e.variant(fv)
		}
		return e.object(fv)
	}
	return nil
}

func (e *encoder) optPayload(v reflect.Value) error {
	t := v.Type()
	if isScalarK(t.Kind()) {
		start := e.pos()
		e.zeros(fFixedSz(t))
		writeScalar(e.buf, start, v)
		return nil
	}
	switch t.Kind() {
	case reflect.String:
		s := v.String()
		start := e.pos()
		e.zeros(4)
		e.setU32(start, uint32(len(s)))
		e.raw([]byte(s))
	case reflect.Slice:
		if v.Len() == 0 {
			return nil
		}
		return e.vec(t.Elem(), v)
	case reflect.Struct:
		return e.object(v)
	}
	return nil
}

func (e *encoder) vec(elemT reflect.Type, v reflect.Value) error {
	n := v.Len()
	elemFix := fFixedSz(elemT)
	fixBytes := uint32(n) * elemFix

	// Inline elements (scalars, arrays)
	if isScalarK(elemT.Kind()) || elemT.Kind() == reflect.Array {
		start := e.pos()
		e.zeros(4 + fixBytes)
		e.setU32(start, fixBytes)
		for i := 0; i < n; i++ {
			if elemT.Kind() == reflect.Array {
				writeArr(e.buf, start+4+uint32(i)*elemFix, v.Index(i))
			} else {
				writeScalar(e.buf, start+4+uint32(i)*elemFix, v.Index(i))
			}
		}
		return nil
	}

	// Variable-size elements: pack each to temp, then assemble
	packed := make([][]byte, n)
	heapTotal := uint32(0)
	for i := 0; i < n; i++ {
		inner := &encoder{}
		if err := inner.vecElem(v.Index(i)); err != nil {
			return err
		}
		packed[i] = inner.buf
		heapTotal += uint32(len(inner.buf))
	}

	start := e.pos()
	e.zeros(4 + fixBytes + heapTotal)
	e.setU32(start, fixBytes)

	heapStart := start + 4 + fixBytes
	hoff := uint32(0)
	for i := 0; i < n; i++ {
		op := start + 4 + uint32(i)*4
		if len(packed[i]) == 0 {
			if elemT.Kind() == reflect.Ptr {
				e.setU32(op, 1) // absent optional
			} else {
				e.setU32(op, 0)
			}
		} else {
			e.setU32(op, heapStart+hoff-op)
			copy(e.buf[heapStart+hoff:], packed[i])
			hoff += uint32(len(packed[i]))
		}
	}
	return nil
}

func (e *encoder) vecElem(fv reflect.Value) error {
	t := fv.Type()
	switch t.Kind() {
	case reflect.String:
		s := fv.String()
		if len(s) == 0 {
			return nil
		}
		e.zeros(4)
		e.setU32(0, uint32(len(s)))
		e.raw([]byte(s))
	case reflect.Slice:
		if fv.Len() == 0 {
			return nil
		}
		return e.vec(t.Elem(), fv)
	case reflect.Ptr:
		if fv.IsNil() {
			return nil
		}
		return e.optPayload(fv.Elem())
	case reflect.Struct:
		if isVariantStruct(t) {
			return e.variant(fv)
		}
		return e.object(fv)
	}
	return nil
}

func (e *encoder) variant(v reflect.Value) error {
	fields := getFields(v.Type())
	tag := uint8(0)
	var payload reflect.Value
	found := false
	for i, fi := range fields {
		fv := v.Field(fi.idx)
		if fv.Kind() == reflect.Ptr && !fv.IsNil() {
			tag = uint8(i)
			payload = fv.Elem()
			found = true
			break
		}
	}

	var content []byte
	if found {
		inner := &encoder{}
		if err := inner.variantContent(payload); err != nil {
			return nil
		}
		content = inner.buf
	}

	start := e.pos()
	e.zeros(1 + 4 + uint32(len(content)))
	e.buf[start] = tag
	e.setU32(start+1, uint32(len(content)))
	if len(content) > 0 {
		copy(e.buf[start+5:], content)
	}
	return nil
}

func (e *encoder) variantContent(v reflect.Value) error {
	t := v.Type()
	if isScalarK(t.Kind()) {
		start := e.pos()
		e.zeros(fFixedSz(t))
		writeScalar(e.buf, start, v)
		return nil
	}
	switch t.Kind() {
	case reflect.String:
		s := v.String()
		start := e.pos()
		e.zeros(4)
		e.setU32(start, uint32(len(s)))
		e.raw([]byte(s))
	case reflect.Struct:
		return e.object(v)
	}
	return nil
}

// ── Scalar read/write ───────────────────────────────────────────────────

func writeScalar(buf []byte, pos uint32, v reflect.Value) {
	switch v.Kind() {
	case reflect.Bool:
		if v.Bool() {
			buf[pos] = 1
		} else {
			buf[pos] = 0
		}
	case reflect.Uint8:
		buf[pos] = uint8(v.Uint())
	case reflect.Int8:
		buf[pos] = byte(int8(v.Int()))
	case reflect.Uint16:
		binary.LittleEndian.PutUint16(buf[pos:], uint16(v.Uint()))
	case reflect.Int16:
		binary.LittleEndian.PutUint16(buf[pos:], uint16(int16(v.Int())))
	case reflect.Uint32:
		binary.LittleEndian.PutUint32(buf[pos:], uint32(v.Uint()))
	case reflect.Int32:
		binary.LittleEndian.PutUint32(buf[pos:], uint32(int32(v.Int())))
	case reflect.Uint64:
		binary.LittleEndian.PutUint64(buf[pos:], v.Uint())
	case reflect.Int64:
		binary.LittleEndian.PutUint64(buf[pos:], uint64(v.Int()))
	case reflect.Float32:
		binary.LittleEndian.PutUint32(buf[pos:], math.Float32bits(float32(v.Float())))
	case reflect.Float64:
		binary.LittleEndian.PutUint64(buf[pos:], math.Float64bits(v.Float()))
	}
}

func writeArr(buf []byte, pos uint32, v reflect.Value) {
	et := v.Type().Elem()
	esz := fFixedSz(et)
	for i := 0; i < v.Len(); i++ {
		writeScalar(buf, pos+uint32(i)*esz, v.Index(i))
	}
}

func readScalarInto(data []byte, pos uint32, fv reflect.Value) {
	switch fv.Kind() {
	case reflect.Bool:
		fv.SetBool(data[pos] != 0)
	case reflect.Uint8:
		fv.SetUint(uint64(data[pos]))
	case reflect.Int8:
		fv.SetInt(int64(int8(data[pos])))
	case reflect.Uint16:
		fv.SetUint(uint64(binary.LittleEndian.Uint16(data[pos:])))
	case reflect.Int16:
		fv.SetInt(int64(int16(binary.LittleEndian.Uint16(data[pos:]))))
	case reflect.Uint32:
		fv.SetUint(uint64(binary.LittleEndian.Uint32(data[pos:])))
	case reflect.Int32:
		fv.SetInt(int64(int32(binary.LittleEndian.Uint32(data[pos:]))))
	case reflect.Uint64:
		fv.SetUint(binary.LittleEndian.Uint64(data[pos:]))
	case reflect.Int64:
		fv.SetInt(int64(binary.LittleEndian.Uint64(data[pos:])))
	case reflect.Float32:
		fv.SetFloat(float64(math.Float32frombits(binary.LittleEndian.Uint32(data[pos:]))))
	case reflect.Float64:
		fv.SetFloat(math.Float64frombits(binary.LittleEndian.Uint64(data[pos:])))
	}
}

// ── Decoder ─────────────────────────────────────────────────────────────

func decodeObject(data []byte, pos uint32, v reflect.Value) error {
	t := v.Type()
	ext := !isFixedStruct(t)
	fields := getFields(t)

	var headerSz uint32
	if ext {
		headerSz = 2
	}

	fullFixed := uint32(0)
	for _, fi := range fields {
		fullFixed += fFixedSz(fi.sf.Type)
	}

	actualFixed := fullFixed
	if ext {
		if uint32(len(data)) < pos+2 {
			return fmt.Errorf("fracpack: buffer too short for header")
		}
		actualFixed = uint32(binary.LittleEndian.Uint16(data[pos:]))
	}

	off := uint32(0)
	for _, fi := range fields {
		fsz := fFixedSz(fi.sf.Type)
		if off >= actualFixed {
			off += fsz
			continue
		}
		fp := pos + headerSz + off
		fv := v.Field(fi.idx)
		if err := decodeField(data, fp, fv); err != nil {
			return err
		}
		off += fsz
	}
	return nil
}

func decodeField(data []byte, pos uint32, fv reflect.Value) error {
	t := fv.Type()

	if isScalarK(t.Kind()) {
		readScalarInto(data, pos, fv)
		return nil
	}
	if t.Kind() == reflect.Array {
		et := t.Elem()
		esz := fFixedSz(et)
		for i := 0; i < t.Len(); i++ {
			readScalarInto(data, pos+uint32(i)*esz, fv.Index(i))
		}
		return nil
	}

	off := binary.LittleEndian.Uint32(data[pos:])

	switch t.Kind() {
	case reflect.String:
		if off == 0 {
			fv.SetString("")
			return nil
		}
		hp := pos + off
		slen := binary.LittleEndian.Uint32(data[hp:])
		fv.SetString(string(data[hp+4 : hp+4+slen]))

	case reflect.Slice:
		if off == 0 {
			fv.Set(reflect.MakeSlice(t, 0, 0))
			return nil
		}
		return decodeVec(data, pos+off, fv)

	case reflect.Ptr:
		if off == 1 {
			return nil // absent
		}
		elem := reflect.New(t.Elem())
		if off == 0 {
			// present but empty
			fv.Set(elem)
			return nil
		}
		if err := decodeOptPayload(data, pos+off, elem.Elem()); err != nil {
			return err
		}
		fv.Set(elem)

	case reflect.Struct:
		if isVariantStruct(t) {
			if off == 0 {
				return nil
			}
			return decodeVariantInto(data, pos+off, fv)
		}
		if off == 0 {
			return nil
		}
		return decodeObject(data, pos+off, fv)
	}
	return nil
}

func decodeVec(data []byte, pos uint32, fv reflect.Value) error {
	fixBytes := binary.LittleEndian.Uint32(data[pos:])
	elemT := fv.Type().Elem()
	elemFix := fFixedSz(elemT)
	if elemFix == 0 {
		return nil
	}
	n := int(fixBytes / elemFix)
	slice := reflect.MakeSlice(fv.Type(), n, n)

	for i := 0; i < n; i++ {
		ep := pos + 4 + uint32(i)*elemFix
		if err := decodeVecElem(data[pos:], 4+uint32(i)*elemFix, slice.Index(i)); err != nil {
			return err
		}
		_ = ep
	}
	fv.Set(slice)
	return nil
}

func decodeVecElem(vecData []byte, epos uint32, fv reflect.Value) error {
	t := fv.Type()
	if isScalarK(t.Kind()) {
		readScalarInto(vecData, epos, fv)
		return nil
	}
	switch t.Kind() {
	case reflect.String:
		off := binary.LittleEndian.Uint32(vecData[epos:])
		if off == 0 {
			fv.SetString("")
			return nil
		}
		hp := epos + off
		slen := binary.LittleEndian.Uint32(vecData[hp:])
		fv.SetString(string(vecData[hp+4 : hp+4+slen]))

	case reflect.Slice:
		off := binary.LittleEndian.Uint32(vecData[epos:])
		if off == 0 {
			fv.Set(reflect.MakeSlice(t, 0, 0))
			return nil
		}
		return decodeVec(vecData, epos+off, fv)

	case reflect.Ptr:
		off := binary.LittleEndian.Uint32(vecData[epos:])
		if off == 1 {
			return nil // absent
		}
		elem := reflect.New(t.Elem())
		if off == 0 {
			fv.Set(elem)
			return nil
		}
		if err := decodeOptPayload(vecData, epos+off, elem.Elem()); err != nil {
			return err
		}
		fv.Set(elem)

	case reflect.Struct:
		off := binary.LittleEndian.Uint32(vecData[epos:])
		if off == 0 {
			return nil
		}
		return decodeObject(vecData, epos+off, fv)
	}
	return nil
}

func decodeOptPayload(data []byte, pos uint32, fv reflect.Value) error {
	t := fv.Type()
	if isScalarK(t.Kind()) {
		readScalarInto(data, pos, fv)
		return nil
	}
	switch t.Kind() {
	case reflect.String:
		slen := binary.LittleEndian.Uint32(data[pos:])
		fv.SetString(string(data[pos+4 : pos+4+slen]))
	case reflect.Slice:
		return decodeVec(data, pos, fv)
	case reflect.Struct:
		return decodeObject(data, pos, fv)
	}
	return nil
}

func decodeVariantInto(data []byte, pos uint32, fv reflect.Value) error {
	if uint32(len(data)) < pos+5 {
		return fmt.Errorf("fracpack: buffer too short for variant")
	}
	tag := data[pos]
	// contentSize := binary.LittleEndian.Uint32(data[pos+1:])
	contentStart := pos + 5

	fields := getFields(fv.Type())
	if int(tag) >= len(fields) {
		return fmt.Errorf("fracpack: variant tag %d out of range", tag)
	}

	fi := fields[tag]
	field := fv.Field(fi.idx)
	// field is a *T; allocate and decode
	elem := reflect.New(fi.sf.Type.Elem())

	innerV := elem.Elem()
	innerT := innerV.Type()
	if isScalarK(innerT.Kind()) {
		readScalarInto(data, contentStart, innerV)
	} else if innerT.Kind() == reflect.String {
		slen := binary.LittleEndian.Uint32(data[contentStart:])
		innerV.SetString(string(data[contentStart+4 : contentStart+4+slen]))
	} else if innerT.Kind() == reflect.Struct {
		if err := decodeObject(data, contentStart, innerV); err != nil {
			return err
		}
	}

	field.Set(elem)
	return nil
}
