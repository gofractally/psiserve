package wit

import (
	"strings"
)

// Component Model binary opcodes.
const (
	sectionCustom = 0x00
	sectionType   = 0x07
	sectionExport = 0x0b

	typeComponent = 0x41
	typeInstance   = 0x42
	typeFunc       = 0x40

	defRecord  = 0x72
	defVariant = 0x71
	defList    = 0x70
	defTuple   = 0x6f
	defFlags   = 0x6e
	defEnum    = 0x6d
	defOption  = 0x6b
	defResult  = 0x6a

	primBoolByte   = 0x7f
	primS8Byte     = 0x7e
	primU8Byte     = 0x7d
	primS16Byte    = 0x7c
	primU16Byte    = 0x7b
	primS32Byte    = 0x7a
	primU32Byte    = 0x79
	primS64Byte    = 0x78
	primU64Byte    = 0x77
	primF32Byte    = 0x76
	primF64Byte    = 0x75
	primCharByte   = 0x74
	primStringByte = 0x73

	itemTypeDef = 0x01
	itemExport  = 0x04

	sortFunc      = 0x01
	sortType      = 0x03
	sortComponent = 0x04
	sortInstance  = 0x05

	nameKebab = 0x00
	boundEq   = 0x00

	resultSingle = 0x00
	resultNamed  = 0x01
)

// primToCMByte maps a WitPrim to its Component Model binary opcode.
func primToCMByte(p WitPrim) byte {
	switch p {
	case PrimBool:
		return primBoolByte
	case PrimU8:
		return primU8Byte
	case PrimS8:
		return primS8Byte
	case PrimU16:
		return primU16Byte
	case PrimS16:
		return primS16Byte
	case PrimU32:
		return primU32Byte
	case PrimS32:
		return primS32Byte
	case PrimU64:
		return primU64Byte
	case PrimS64:
		return primS64Byte
	case PrimF32:
		return primF32Byte
	case PrimF64:
		return primF64Byte
	case PrimChar:
		return primCharByte
	case PrimString:
		return primStringByte
	default:
		return primU32Byte
	}
}

// binaryWriter is a helper buffer for building binary output.
type binaryWriter struct {
	buf []byte
}

func (w *binaryWriter) emitByte(b byte) {
	w.buf = append(w.buf, b)
}

func (w *binaryWriter) emitULEB128(val uint32) {
	for {
		b := byte(val & 0x7f)
		val >>= 7
		if val != 0 {
			b |= 0x80
		}
		w.buf = append(w.buf, b)
		if val == 0 {
			break
		}
	}
}

func (w *binaryWriter) emitString(s string) {
	w.emitULEB128(uint32(len(s)))
	w.buf = append(w.buf, s...)
}

func (w *binaryWriter) emitBytes(data []byte) {
	w.buf = append(w.buf, data...)
}

func (w *binaryWriter) size() int {
	return len(w.buf)
}

func (w *binaryWriter) data() []byte {
	return w.buf
}

// cmEncoder encodes a WitWorld into Component Model binary format.
type cmEncoder struct {
	world *WitWorld
}

func (e *cmEncoder) emitValtype(w *binaryWriter, typeIdx int32, remap map[int32]uint32) {
	if IsPrimIdx(typeIdx) {
		w.emitByte(primToCMByte(IdxToPrim(typeIdx)))
	} else {
		if idx, ok := remap[typeIdx]; ok {
			w.emitULEB128(idx)
		} else {
			w.emitULEB128(uint32(typeIdx))
		}
	}
}

func (e *cmEncoder) emitDefinedType(w *binaryWriter, td *WitTypeDef, remap map[int32]uint32) {
	switch td.Kind {
	case KindRecord:
		w.emitByte(defRecord)
		w.emitULEB128(uint32(len(td.Fields)))
		for i := range td.Fields {
			w.emitString(td.Fields[i].Name)
			e.emitValtype(w, td.Fields[i].TypeIdx, remap)
		}
	case KindList:
		w.emitByte(defList)
		e.emitValtype(w, td.ElementTypeIdx, remap)
	case KindOption:
		w.emitByte(defOption)
		e.emitValtype(w, td.ElementTypeIdx, remap)
	case KindResult:
		w.emitByte(defResult)
		e.emitValtype(w, td.ElementTypeIdx, remap)
		e.emitValtype(w, td.ErrorTypeIdx, remap)
	case KindVariant:
		w.emitByte(defVariant)
		w.emitULEB128(uint32(len(td.Fields)))
		for i := range td.Fields {
			w.emitString(td.Fields[i].Name)
			e.emitValtype(w, td.Fields[i].TypeIdx, remap)
		}
	case KindEnum:
		w.emitByte(defEnum)
		w.emitULEB128(uint32(len(td.Fields)))
		for i := range td.Fields {
			w.emitString(td.Fields[i].Name)
		}
	case KindFlags:
		w.emitByte(defFlags)
		w.emitULEB128(uint32(len(td.Fields)))
		for i := range td.Fields {
			w.emitString(td.Fields[i].Name)
		}
	case KindTuple:
		w.emitByte(defTuple)
		w.emitULEB128(uint32(len(td.Fields)))
		for i := range td.Fields {
			e.emitValtype(w, td.Fields[i].TypeIdx, remap)
		}
	}
}

func (e *cmEncoder) emitFuncType(w *binaryWriter, fn *WitFunc, remap map[int32]uint32) {
	w.emitByte(typeFunc)
	// Parameters
	w.emitULEB128(uint32(len(fn.Params)))
	for i := range fn.Params {
		w.emitString(fn.Params[i].Name)
		e.emitValtype(w, fn.Params[i].TypeIdx, remap)
	}
	// Results
	if len(fn.Results) == 1 && fn.Results[0].Name == "" {
		w.emitByte(resultSingle)
		e.emitValtype(w, fn.Results[0].TypeIdx, remap)
	} else {
		w.emitByte(resultNamed)
		w.emitULEB128(uint32(len(fn.Results)))
		for i := range fn.Results {
			w.emitString(fn.Results[i].Name)
			e.emitValtype(w, fn.Results[i].TypeIdx, remap)
		}
	}
}

func (e *cmEncoder) emitExternName(w *binaryWriter, name string) {
	w.emitByte(nameKebab)
	w.emitString(name)
}

func (e *cmEncoder) ensureEmitted(typeIdx int32, w *binaryWriter,
	remap map[int32]uint32, nextTypeIdx *uint32, itemCount *uint32) {

	if IsPrimIdx(typeIdx) {
		return
	}
	if _, ok := remap[typeIdx]; ok {
		return
	}

	idx := int(typeIdx)
	if idx < 0 || idx >= len(e.world.Types) {
		return
	}

	td := &e.world.Types[idx]

	// Ensure dependencies are emitted first
	switch td.Kind {
	case KindRecord:
		for i := range td.Fields {
			e.ensureEmitted(td.Fields[i].TypeIdx, w, remap, nextTypeIdx, itemCount)
		}
	case KindList, KindOption:
		e.ensureEmitted(td.ElementTypeIdx, w, remap, nextTypeIdx, itemCount)
	case KindResult:
		e.ensureEmitted(td.ElementTypeIdx, w, remap, nextTypeIdx, itemCount)
		e.ensureEmitted(td.ErrorTypeIdx, w, remap, nextTypeIdx, itemCount)
	case KindVariant, KindTuple:
		for i := range td.Fields {
			e.ensureEmitted(td.Fields[i].TypeIdx, w, remap, nextTypeIdx, itemCount)
		}
	}

	// Check again after dependency resolution (may have been emitted)
	if _, ok := remap[typeIdx]; ok {
		return
	}

	// Emit type def item
	w.emitByte(itemTypeDef)
	e.emitDefinedType(w, td, remap)
	defIdx := *nextTypeIdx
	*nextTypeIdx++
	*itemCount++

	if td.Name != "" {
		// Named type: emit export alias
		w.emitByte(itemExport)
		e.emitExternName(w, td.Name)
		w.emitByte(sortType)
		w.emitByte(boundEq)
		w.emitULEB128(defIdx)
		remap[typeIdx] = *nextTypeIdx
		*nextTypeIdx++
		*itemCount++
	} else {
		// Anonymous type: remap directly to def index
		remap[typeIdx] = defIdx
	}
}

type instanceEncoding struct {
	bytes     []byte
	itemCount uint32
}

func (e *cmEncoder) encodeInterface(iface *WitInterface) instanceEncoding {
	remap := make(map[int32]uint32)
	var nextTypeIdx uint32
	var itemCount uint32
	w := &binaryWriter{}

	// Phase 1: Emit all named record types (and their dependencies)
	for _, typeIdx := range iface.TypeIdxs {
		e.ensureEmitted(int32(typeIdx), w, remap, &nextTypeIdx, &itemCount)
	}

	// Phase 2: Emit functions, lazily emitting remaining anonymous types
	for _, funcIdx := range iface.FuncIdxs {
		if int(funcIdx) >= len(e.world.Funcs) {
			continue
		}
		fn := &e.world.Funcs[funcIdx]

		for i := range fn.Params {
			e.ensureEmitted(fn.Params[i].TypeIdx, w, remap, &nextTypeIdx, &itemCount)
		}
		for i := range fn.Results {
			e.ensureEmitted(fn.Results[i].TypeIdx, w, remap, &nextTypeIdx, &itemCount)
		}

		// Emit func type def
		w.emitByte(itemTypeDef)
		e.emitFuncType(w, fn, remap)
		funcTypeIdx := nextTypeIdx
		nextTypeIdx++
		itemCount++

		// Emit func export
		w.emitByte(itemExport)
		e.emitExternName(w, fn.Name)
		w.emitByte(sortFunc)
		w.emitULEB128(funcTypeIdx)
		itemCount++
	}

	return instanceEncoding{
		bytes:     w.data(),
		itemCount: itemCount,
	}
}

func (e *cmEncoder) qualifiedInterfaceName(iface *WitInterface) string {
	pkg := e.world.Package
	atPos := strings.Index(pkg, "@")
	if atPos >= 0 {
		return pkg[:atPos] + "/" + iface.Name + pkg[atPos:]
	}
	return pkg + "/" + iface.Name
}

func (e *cmEncoder) qualifiedWorldName() string {
	pkg := e.world.Package
	atPos := strings.Index(pkg, "@")
	if atPos >= 0 {
		return pkg[:atPos] + "/" + e.world.Name + pkg[atPos:]
	}
	return pkg + "/" + e.world.Name
}

// Encode encodes a WitWorld to Component Model binary format.
// Returns bytes suitable for embedding as a component-type custom section.
func Encode(world *WitWorld) []byte {
	enc := &cmEncoder{world: world}
	out := &binaryWriter{}

	// Component header: \x00asm\x0d\x00\x01\x00
	out.emitByte(0x00)
	out.emitByte(0x61)
	out.emitByte(0x73)
	out.emitByte(0x6d)
	out.emitByte(0x0d)
	out.emitByte(0x00)
	out.emitByte(0x01)
	out.emitByte(0x00)

	// Custom section: wit-component-encoding (version 4, UTF-8)
	{
		cs := &binaryWriter{}
		cs.emitString("wit-component-encoding")
		cs.emitByte(0x04) // version
		cs.emitByte(0x00) // encoding: UTF-8
		out.emitByte(sectionCustom)
		out.emitULEB128(uint32(cs.size()))
		out.emitBytes(cs.data())
	}

	if len(world.Exports) == 0 {
		return out.data()
	}

	expIface := &world.Exports[0]
	inst := enc.encodeInterface(expIface)

	// Type section: 1 type = outer COMPONENT
	{
		typeSec := &binaryWriter{}
		typeSec.emitULEB128(1) // 1 type

		// Outer component (2 items: inner component + world export)
		typeSec.emitByte(typeComponent)
		typeSec.emitULEB128(2)

		// Item 0: inner component (2 items: instance + interface export)
		typeSec.emitByte(itemTypeDef)
		typeSec.emitByte(typeComponent)
		typeSec.emitULEB128(2)

		// Inner item 0: instance type
		typeSec.emitByte(itemTypeDef)
		typeSec.emitByte(typeInstance)
		typeSec.emitULEB128(inst.itemCount)
		typeSec.emitBytes(inst.bytes)

		// Inner item 1: export interface as instance(0)
		typeSec.emitByte(itemExport)
		enc.emitExternName(typeSec, enc.qualifiedInterfaceName(expIface))
		typeSec.emitByte(sortInstance)
		typeSec.emitULEB128(0)

		// Outer item 1: export world as component(0)
		typeSec.emitByte(itemExport)
		enc.emitExternName(typeSec, enc.qualifiedWorldName())
		typeSec.emitByte(sortComponent)
		typeSec.emitULEB128(0)

		// Write the type section
		out.emitByte(sectionType)
		out.emitULEB128(uint32(typeSec.size()))
		out.emitBytes(typeSec.data())
	}

	// Export section: export world name as type(0)
	{
		expSec := &binaryWriter{}
		expSec.emitULEB128(1) // 1 export
		enc.emitExternName(expSec, world.Name)
		expSec.emitByte(sortType)
		expSec.emitULEB128(0) // type index 0
		expSec.emitByte(0x00) // no explicit type annotation

		out.emitByte(sectionExport)
		out.emitULEB128(uint32(expSec.size()))
		out.emitBytes(expSec.data())
	}

	// Custom section: producers
	{
		cs := &binaryWriter{}
		cs.emitString("producers")
		cs.emitULEB128(1) // 1 field
		cs.emitString("processed-by")
		cs.emitULEB128(1) // 1 entry
		cs.emitString("psio-wit-gen")
		cs.emitString("1.0.0")

		out.emitByte(sectionCustom)
		out.emitULEB128(uint32(cs.size()))
		out.emitBytes(cs.data())
	}

	return out.data()
}
