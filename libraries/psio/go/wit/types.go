// Package wit provides data structures and a binary encoder for WIT
// (WebAssembly Interface Types) Component Model format.
package wit

// WitPrim represents a WIT primitive type.
type WitPrim uint8

const (
	PrimBool   WitPrim = 0
	PrimU8     WitPrim = 1
	PrimS8     WitPrim = 2
	PrimU16    WitPrim = 3
	PrimS16    WitPrim = 4
	PrimU32    WitPrim = 5
	PrimS32    WitPrim = 6
	PrimU64    WitPrim = 7
	PrimS64    WitPrim = 8
	PrimF32    WitPrim = 9
	PrimF64    WitPrim = 10
	PrimChar   WitPrim = 11
	PrimString WitPrim = 12
)

// PrimIdx encodes a primitive as a negative type index.
func PrimIdx(p WitPrim) int32 {
	return -(int32(p) + 1)
}

// IdxToPrim decodes a negative type index back to a primitive.
func IdxToPrim(idx int32) WitPrim {
	return WitPrim(-(idx + 1))
}

// IsPrimIdx returns true if the type index refers to a primitive (negative).
func IsPrimIdx(idx int32) bool {
	return idx < 0
}

// WitTypeKind discriminates compound type definitions.
type WitTypeKind uint8

const (
	KindRecord  WitTypeKind = 0
	KindVariant WitTypeKind = 1
	KindEnum    WitTypeKind = 2
	KindFlags   WitTypeKind = 3
	KindList    WitTypeKind = 4
	KindOption  WitTypeKind = 5
	KindResult  WitTypeKind = 6
	KindTuple   WitTypeKind = 7
)

// WitNamedType is a named field within a record, variant case,
// function param/result, or label within an enum/flags.
type WitNamedType struct {
	Name    string
	TypeIdx int32
}

// WitTypeDef is a compound type definition (record, variant, enum, flags,
// list, option, result, tuple).
type WitTypeDef struct {
	Name           string
	Kind           WitTypeKind
	Fields         []WitNamedType
	ElementTypeIdx int32 // list/option element; result ok type
	ErrorTypeIdx   int32 // result err type
}

// WitFunc is a WIT function signature with optional link to a WASM core export.
type WitFunc struct {
	Name        string
	Params      []WitNamedType
	Results     []WitNamedType
	CoreFuncIdx uint32
}

// WitInterface is a named group of types and functions (WIT interface).
type WitInterface struct {
	Name     string
	TypeIdxs []uint32 // indices into World.Types[]
	FuncIdxs []uint32 // indices into World.Funcs[]
}

// WitWorld is a complete WIT world definition.
type WitWorld struct {
	Package   string // e.g. "test:inventory@1.0.0"
	Name      string // world name
	WitSource string // raw .wit text for tooling
	Types     []WitTypeDef
	Funcs     []WitFunc
	Exports   []WitInterface
	Imports   []WitInterface
}
