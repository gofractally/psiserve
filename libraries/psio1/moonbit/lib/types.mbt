// WIT (WebAssembly Interface Types) data structures for Component Model encoding.
//
// Type references use index-based encoding:
//   type_idx >= 0  -> index into WitWorld::types[]
//   type_idx < 0   -> primitive: -(type_idx + 1) gives the WitPrim ordinal

pub(all) enum WitPrim {
  Bool_
  U8
  S8
  U16
  S16
  U32
  S32
  U64
  S64
  F32
  F64
  Char_
  String_
}

pub(all) enum WitTypeKind {
  Record
  Variant
  Enum_
  Flags
  List
  Option_
  Result_
  Tuple
}

pub(all) struct WitNamedType {
  name : String
  type_idx : Int
}

pub(all) struct WitTypeDef {
  name : String
  kind : WitTypeKind
  fields : Array[WitNamedType]
  element_type_idx : Int
  error_type_idx : Int
}

pub(all) struct WitFunc {
  name : String
  params : Array[WitNamedType]
  results : Array[WitNamedType]
}

pub(all) struct WitInterface {
  name : String
  type_idxs : Array[Int]
  func_idxs : Array[Int]
}

pub(all) struct WitWorld {
  package_ : String
  name : String
  types : Array[WitTypeDef]
  funcs : Array[WitFunc]
  exports : Array[WitInterface]
  imports : Array[WitInterface]
}

/// Encode a primitive as a negative type index.
pub fn prim_idx(p : WitPrim) -> Int {
  let ordinal = match p {
    Bool_ => 0
    U8 => 1
    S8 => 2
    U16 => 3
    S16 => 4
    U32 => 5
    S32 => 6
    U64 => 7
    S64 => 8
    F32 => 9
    F64 => 10
    Char_ => 11
    String_ => 12
  }
  -(ordinal + 1)
}

/// Decode a negative type index back to a primitive.
pub fn idx_to_prim(idx : Int) -> WitPrim {
  let ordinal = -(idx + 1)
  match ordinal {
    0 => Bool_
    1 => U8
    2 => S8
    3 => U16
    4 => S16
    5 => U32
    6 => S32
    7 => U64
    8 => S64
    9 => F32
    10 => F64
    11 => Char_
    12 => String_
    _ => U32
  }
}

/// True if the type index refers to a primitive (negative).
pub fn is_prim_idx(idx : Int) -> Bool {
  idx < 0
}
