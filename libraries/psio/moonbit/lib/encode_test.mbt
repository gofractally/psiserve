// Golden binary comparison test for the WIT Component Model encoder.
//
// Constructs the InventoryApi world (same as the C++ test) and verifies
// the encoder produces an identical 708-byte binary.

/// Build the InventoryApi test world.
fn build_inventory_world() -> @lib.WitWorld {
  // Primitive index helpers
  let bool_ = @lib.prim_idx(@lib.WitPrim::Bool_)   // -1
  let u32_ = @lib.prim_idx(@lib.WitPrim::U32)      // -6
  let s32_ = @lib.prim_idx(@lib.WitPrim::S32)      // -7
  let u64_ = @lib.prim_idx(@lib.WitPrim::U64)      // -8
  let string_ = @lib.prim_idx(@lib.WitPrim::String_) // -13

  // Type 0: record "item"
  let type0 : @lib.WitTypeDef = @lib.WitTypeDef::{
    name: "item",
    kind: @lib.WitTypeKind::Record,
    fields: [
      @lib.WitNamedType::{ name: "id", type_idx: u64_ },
      @lib.WitNamedType::{ name: "name", type_idx: string_ },
      @lib.WitNamedType::{ name: "category", type_idx: string_ },
      @lib.WitNamedType::{ name: "price-cents", type_idx: u32_ },
      @lib.WitNamedType::{ name: "in-stock", type_idx: bool_ },
      @lib.WitNamedType::{ name: "tags", type_idx: 1 },         // list<string>
      @lib.WitNamedType::{ name: "weight-grams", type_idx: 2 },  // option<u32>
    ],
    element_type_idx: 0,
    error_type_idx: 0,
  }

  // Type 1: list<string> (anonymous)
  let type1 : @lib.WitTypeDef = @lib.WitTypeDef::{
    name: "",
    kind: @lib.WitTypeKind::List,
    fields: [],
    element_type_idx: string_,
    error_type_idx: 0,
  }

  // Type 2: option<u32> (anonymous)
  let type2 : @lib.WitTypeDef = @lib.WitTypeDef::{
    name: "",
    kind: @lib.WitTypeKind::Option_,
    fields: [],
    element_type_idx: u32_,
    error_type_idx: 0,
  }

  // Type 3: record "stock-result"
  let type3 : @lib.WitTypeDef = @lib.WitTypeDef::{
    name: "stock-result",
    kind: @lib.WitTypeKind::Record,
    fields: [
      @lib.WitNamedType::{ name: "item-id", type_idx: u32_ },
      @lib.WitNamedType::{ name: "old-quantity", type_idx: u32_ },
      @lib.WitNamedType::{ name: "new-quantity", type_idx: u32_ },
    ],
    element_type_idx: 0,
    error_type_idx: 0,
  }

  // Type 4: record "search-query"
  let type4 : @lib.WitTypeDef = @lib.WitTypeDef::{
    name: "search-query",
    kind: @lib.WitTypeKind::Record,
    fields: [
      @lib.WitNamedType::{ name: "text", type_idx: string_ },
      @lib.WitNamedType::{ name: "max-results", type_idx: u32_ },
      @lib.WitNamedType::{ name: "min-price", type_idx: 2 },    // option<u32>
      @lib.WitNamedType::{ name: "max-price", type_idx: 2 },    // option<u32>
      @lib.WitNamedType::{ name: "categories", type_idx: 1 },   // list<string>
    ],
    element_type_idx: 0,
    error_type_idx: 0,
  }

  // Type 5: record "search-response"
  let type5 : @lib.WitTypeDef = @lib.WitTypeDef::{
    name: "search-response",
    kind: @lib.WitTypeKind::Record,
    fields: [
      @lib.WitNamedType::{ name: "items", type_idx: 6 },         // list<item>
      @lib.WitNamedType::{ name: "total-count", type_idx: u64_ },
      @lib.WitNamedType::{ name: "has-more", type_idx: bool_ },
    ],
    element_type_idx: 0,
    error_type_idx: 0,
  }

  // Type 6: list<item> (anonymous, element_type_idx = 0)
  let type6 : @lib.WitTypeDef = @lib.WitTypeDef::{
    name: "",
    kind: @lib.WitTypeKind::List,
    fields: [],
    element_type_idx: 0,
    error_type_idx: 0,
  }

  // Type 7: record "bulk-result"
  let type7 : @lib.WitTypeDef = @lib.WitTypeDef::{
    name: "bulk-result",
    kind: @lib.WitTypeKind::Record,
    fields: [
      @lib.WitNamedType::{ name: "inserted", type_idx: u32_ },
      @lib.WitNamedType::{ name: "failed", type_idx: u32_ },
      @lib.WitNamedType::{ name: "errors", type_idx: 1 },       // list<string>
    ],
    element_type_idx: 0,
    error_type_idx: 0,
  }

  // Type 8: option<item> (anonymous, element_type_idx = 0)
  let type8 : @lib.WitTypeDef = @lib.WitTypeDef::{
    name: "",
    kind: @lib.WitTypeKind::Option_,
    fields: [],
    element_type_idx: 0,
    error_type_idx: 0,
  }

  // Functions
  let func0 : @lib.WitFunc = @lib.WitFunc::{
    name: "get-item",
    params: [@lib.WitNamedType::{ name: "item-id", type_idx: u32_ }],
    results: [@lib.WitNamedType::{ name: "", type_idx: 8 }],  // option<item>
  }

  let func1 : @lib.WitFunc = @lib.WitFunc::{
    name: "list-items",
    params: [@lib.WitNamedType::{ name: "category", type_idx: string_ }],
    results: [@lib.WitNamedType::{ name: "", type_idx: 6 }],  // list<item>
  }

  let func2 : @lib.WitFunc = @lib.WitFunc::{
    name: "add-item",
    params: [@lib.WitNamedType::{ name: "item", type_idx: 0 }],  // item record
    results: [@lib.WitNamedType::{ name: "", type_idx: u64_ }],
  }

  let func3 : @lib.WitFunc = @lib.WitFunc::{
    name: "update-stock",
    params: [
      @lib.WitNamedType::{ name: "item-id", type_idx: u32_ },
      @lib.WitNamedType::{ name: "delta", type_idx: s32_ },
    ],
    results: [@lib.WitNamedType::{ name: "", type_idx: 3 }],  // stock-result
  }

  let func4 : @lib.WitFunc = @lib.WitFunc::{
    name: "search",
    params: [@lib.WitNamedType::{ name: "query", type_idx: 4 }],  // search-query
    results: [@lib.WitNamedType::{ name: "", type_idx: 5 }],       // search-response
  }

  let func5 : @lib.WitFunc = @lib.WitFunc::{
    name: "bulk-import",
    params: [@lib.WitNamedType::{ name: "items", type_idx: 6 }],  // list<item>
    results: [@lib.WitNamedType::{ name: "", type_idx: 7 }],       // bulk-result
  }

  let func6 : @lib.WitFunc = @lib.WitFunc::{
    name: "ping",
    params: [],
    results: [],
  }

  // Export interface
  let iface : @lib.WitInterface = @lib.WitInterface::{
    name: "inventory-api",
    type_idxs: [0, 3, 4, 5, 7],
    func_idxs: [0, 1, 2, 3, 4, 5, 6],
  }

  // World
  @lib.WitWorld::{
    package_: "test:inventory@1.0.0",
    name: "inventory-api-world",
    types: [type0, type1, type2, type3, type4, type5, type6, type7, type8],
    funcs: [func0, func1, func2, func3, func4, func5, func6],
    exports: [iface],
    imports: [],
  }
}

/// The golden binary (708 bytes) produced by the C++ encoder.
fn golden_binary() -> Array[Int] {
  [
    0x00, 0x61, 0x73, 0x6d, 0x0d, 0x00, 0x01, 0x00, 0x00, 0x19, 0x16, 0x77, 0x69, 0x74, 0x2d, 0x63,
    0x6f, 0x6d, 0x70, 0x6f, 0x6e, 0x65, 0x6e, 0x74, 0x2d, 0x65, 0x6e, 0x63, 0x6f, 0x64, 0x69, 0x6e,
    0x67, 0x04, 0x00, 0x07, 0xd5, 0x04, 0x01, 0x41, 0x02, 0x01, 0x41, 0x02, 0x01, 0x42, 0x1c, 0x01,
    0x70, 0x73, 0x01, 0x6b, 0x79, 0x01, 0x72, 0x07, 0x02, 0x69, 0x64, 0x77, 0x04, 0x6e, 0x61, 0x6d,
    0x65, 0x73, 0x08, 0x63, 0x61, 0x74, 0x65, 0x67, 0x6f, 0x72, 0x79, 0x73, 0x0b, 0x70, 0x72, 0x69,
    0x63, 0x65, 0x2d, 0x63, 0x65, 0x6e, 0x74, 0x73, 0x79, 0x08, 0x69, 0x6e, 0x2d, 0x73, 0x74, 0x6f,
    0x63, 0x6b, 0x7f, 0x04, 0x74, 0x61, 0x67, 0x73, 0x00, 0x0c, 0x77, 0x65, 0x69, 0x67, 0x68, 0x74,
    0x2d, 0x67, 0x72, 0x61, 0x6d, 0x73, 0x01, 0x04, 0x00, 0x04, 0x69, 0x74, 0x65, 0x6d, 0x03, 0x00,
    0x02, 0x01, 0x72, 0x03, 0x07, 0x69, 0x74, 0x65, 0x6d, 0x2d, 0x69, 0x64, 0x79, 0x0c, 0x6f, 0x6c,
    0x64, 0x2d, 0x71, 0x75, 0x61, 0x6e, 0x74, 0x69, 0x74, 0x79, 0x79, 0x0c, 0x6e, 0x65, 0x77, 0x2d,
    0x71, 0x75, 0x61, 0x6e, 0x74, 0x69, 0x74, 0x79, 0x79, 0x04, 0x00, 0x0c, 0x73, 0x74, 0x6f, 0x63,
    0x6b, 0x2d, 0x72, 0x65, 0x73, 0x75, 0x6c, 0x74, 0x03, 0x00, 0x04, 0x01, 0x72, 0x05, 0x04, 0x74,
    0x65, 0x78, 0x74, 0x73, 0x0b, 0x6d, 0x61, 0x78, 0x2d, 0x72, 0x65, 0x73, 0x75, 0x6c, 0x74, 0x73,
    0x79, 0x09, 0x6d, 0x69, 0x6e, 0x2d, 0x70, 0x72, 0x69, 0x63, 0x65, 0x01, 0x09, 0x6d, 0x61, 0x78,
    0x2d, 0x70, 0x72, 0x69, 0x63, 0x65, 0x01, 0x0a, 0x63, 0x61, 0x74, 0x65, 0x67, 0x6f, 0x72, 0x69,
    0x65, 0x73, 0x00, 0x04, 0x00, 0x0c, 0x73, 0x65, 0x61, 0x72, 0x63, 0x68, 0x2d, 0x71, 0x75, 0x65,
    0x72, 0x79, 0x03, 0x00, 0x06, 0x01, 0x70, 0x03, 0x01, 0x72, 0x03, 0x05, 0x69, 0x74, 0x65, 0x6d,
    0x73, 0x08, 0x0b, 0x74, 0x6f, 0x74, 0x61, 0x6c, 0x2d, 0x63, 0x6f, 0x75, 0x6e, 0x74, 0x77, 0x08,
    0x68, 0x61, 0x73, 0x2d, 0x6d, 0x6f, 0x72, 0x65, 0x7f, 0x04, 0x00, 0x0f, 0x73, 0x65, 0x61, 0x72,
    0x63, 0x68, 0x2d, 0x72, 0x65, 0x73, 0x70, 0x6f, 0x6e, 0x73, 0x65, 0x03, 0x00, 0x09, 0x01, 0x72,
    0x03, 0x08, 0x69, 0x6e, 0x73, 0x65, 0x72, 0x74, 0x65, 0x64, 0x79, 0x06, 0x66, 0x61, 0x69, 0x6c,
    0x65, 0x64, 0x79, 0x06, 0x65, 0x72, 0x72, 0x6f, 0x72, 0x73, 0x00, 0x04, 0x00, 0x0b, 0x62, 0x75,
    0x6c, 0x6b, 0x2d, 0x72, 0x65, 0x73, 0x75, 0x6c, 0x74, 0x03, 0x00, 0x0b, 0x01, 0x6b, 0x03, 0x01,
    0x40, 0x01, 0x07, 0x69, 0x74, 0x65, 0x6d, 0x2d, 0x69, 0x64, 0x79, 0x00, 0x0d, 0x04, 0x00, 0x08,
    0x67, 0x65, 0x74, 0x2d, 0x69, 0x74, 0x65, 0x6d, 0x01, 0x0e, 0x01, 0x40, 0x01, 0x08, 0x63, 0x61,
    0x74, 0x65, 0x67, 0x6f, 0x72, 0x79, 0x73, 0x00, 0x08, 0x04, 0x00, 0x0a, 0x6c, 0x69, 0x73, 0x74,
    0x2d, 0x69, 0x74, 0x65, 0x6d, 0x73, 0x01, 0x0f, 0x01, 0x40, 0x01, 0x04, 0x69, 0x74, 0x65, 0x6d,
    0x03, 0x00, 0x77, 0x04, 0x00, 0x08, 0x61, 0x64, 0x64, 0x2d, 0x69, 0x74, 0x65, 0x6d, 0x01, 0x10,
    0x01, 0x40, 0x02, 0x07, 0x69, 0x74, 0x65, 0x6d, 0x2d, 0x69, 0x64, 0x79, 0x05, 0x64, 0x65, 0x6c,
    0x74, 0x61, 0x7a, 0x00, 0x05, 0x04, 0x00, 0x0c, 0x75, 0x70, 0x64, 0x61, 0x74, 0x65, 0x2d, 0x73,
    0x74, 0x6f, 0x63, 0x6b, 0x01, 0x11, 0x01, 0x40, 0x01, 0x05, 0x71, 0x75, 0x65, 0x72, 0x79, 0x07,
    0x00, 0x0a, 0x04, 0x00, 0x06, 0x73, 0x65, 0x61, 0x72, 0x63, 0x68, 0x01, 0x12, 0x01, 0x40, 0x01,
    0x05, 0x69, 0x74, 0x65, 0x6d, 0x73, 0x08, 0x00, 0x0c, 0x04, 0x00, 0x0b, 0x62, 0x75, 0x6c, 0x6b,
    0x2d, 0x69, 0x6d, 0x70, 0x6f, 0x72, 0x74, 0x01, 0x13, 0x01, 0x40, 0x00, 0x01, 0x00, 0x04, 0x00,
    0x04, 0x70, 0x69, 0x6e, 0x67, 0x01, 0x14, 0x04, 0x00, 0x22, 0x74, 0x65, 0x73, 0x74, 0x3a, 0x69,
    0x6e, 0x76, 0x65, 0x6e, 0x74, 0x6f, 0x72, 0x79, 0x2f, 0x69, 0x6e, 0x76, 0x65, 0x6e, 0x74, 0x6f,
    0x72, 0x79, 0x2d, 0x61, 0x70, 0x69, 0x40, 0x31, 0x2e, 0x30, 0x2e, 0x30, 0x05, 0x00, 0x04, 0x00,
    0x28, 0x74, 0x65, 0x73, 0x74, 0x3a, 0x69, 0x6e, 0x76, 0x65, 0x6e, 0x74, 0x6f, 0x72, 0x79, 0x2f,
    0x69, 0x6e, 0x76, 0x65, 0x6e, 0x74, 0x6f, 0x72, 0x79, 0x2d, 0x61, 0x70, 0x69, 0x2d, 0x77, 0x6f,
    0x72, 0x6c, 0x64, 0x40, 0x31, 0x2e, 0x30, 0x2e, 0x30, 0x04, 0x00, 0x0b, 0x19, 0x01, 0x00, 0x13,
    0x69, 0x6e, 0x76, 0x65, 0x6e, 0x74, 0x6f, 0x72, 0x79, 0x2d, 0x61, 0x70, 0x69, 0x2d, 0x77, 0x6f,
    0x72, 0x6c, 0x64, 0x03, 0x00, 0x00, 0x00, 0x2c, 0x09, 0x70, 0x72, 0x6f, 0x64, 0x75, 0x63, 0x65,
    0x72, 0x73, 0x01, 0x0c, 0x70, 0x72, 0x6f, 0x63, 0x65, 0x73, 0x73, 0x65, 0x64, 0x2d, 0x62, 0x79,
    0x01, 0x0c, 0x70, 0x73, 0x69, 0x6f, 0x2d, 0x77, 0x69, 0x74, 0x2d, 0x67, 0x65, 0x6e, 0x05, 0x31,
    0x2e, 0x30, 0x2e, 0x30,
  ]
}

test "encoder produces golden binary" {
  let world = build_inventory_world()
  let result = @lib.encode(world)
  let expected_ints = golden_binary()

  // Convert expected to bytes for comparison
  let expected : Array[Byte] = []
  for i = 0; i < expected_ints.length(); i = i + 1 {
    expected.push(expected_ints[i].to_byte())
  }

  // Check length first
  if result.length() != expected.length() {
    println("Length mismatch: expected \{expected.length()}, got \{result.length()}")
    // Find first difference to help debug
    let min_len = if result.length() < expected.length() {
      result.length()
    } else {
      expected.length()
    }
    for i = 0; i < min_len; i = i + 1 {
      if result[i] != expected[i] {
        println(
          "First byte mismatch at position \{i}: expected 0x\{byte_to_hex(expected[i])}, got 0x\{byte_to_hex(result[i])}",
        )
        break
      }
    }
    fail!("Binary length mismatch")
  }

  // Byte-by-byte comparison
  for i = 0; i < expected.length(); i = i + 1 {
    if result[i] != expected[i] {
      println(
        "Byte mismatch at position \{i}: expected 0x\{byte_to_hex(expected[i])}, got 0x\{byte_to_hex(result[i])}",
      )
      fail!("Binary content mismatch")
    }
  }
}

test "prim_idx round-trip" {
  assert_eq!(@lib.prim_idx(@lib.WitPrim::Bool_), -1)
  assert_eq!(@lib.prim_idx(@lib.WitPrim::U8), -2)
  assert_eq!(@lib.prim_idx(@lib.WitPrim::S8), -3)
  assert_eq!(@lib.prim_idx(@lib.WitPrim::U16), -4)
  assert_eq!(@lib.prim_idx(@lib.WitPrim::S16), -5)
  assert_eq!(@lib.prim_idx(@lib.WitPrim::U32), -6)
  assert_eq!(@lib.prim_idx(@lib.WitPrim::S32), -7)
  assert_eq!(@lib.prim_idx(@lib.WitPrim::U64), -8)
  assert_eq!(@lib.prim_idx(@lib.WitPrim::S64), -9)
  assert_eq!(@lib.prim_idx(@lib.WitPrim::F32), -10)
  assert_eq!(@lib.prim_idx(@lib.WitPrim::F64), -11)
  assert_eq!(@lib.prim_idx(@lib.WitPrim::Char_), -12)
  assert_eq!(@lib.prim_idx(@lib.WitPrim::String_), -13)
}

test "is_prim_idx" {
  assert_eq!(@lib.is_prim_idx(-1), true)
  assert_eq!(@lib.is_prim_idx(-13), true)
  assert_eq!(@lib.is_prim_idx(0), false)
  assert_eq!(@lib.is_prim_idx(5), false)
}

test "uleb128 encoding" {
  let buf = @lib.ByteBuffer::new()
  buf.emit_uleb128(0)
  assert_eq!(buf.data[0], b'\x00')

  let buf2 = @lib.ByteBuffer::new()
  buf2.emit_uleb128(127)
  assert_eq!(buf2.data[0], b'\x7f')

  let buf3 = @lib.ByteBuffer::new()
  buf3.emit_uleb128(128)
  assert_eq!(buf3.data.length(), 2)
  assert_eq!(buf3.data[0], b'\x80')
  assert_eq!(buf3.data[1], b'\x01')

  let buf4 = @lib.ByteBuffer::new()
  buf4.emit_uleb128(624)
  // 624 = 0x270 = 0b10_0111_0000
  // LEB128: 0xF0 0x04
  assert_eq!(buf4.data.length(), 2)
  assert_eq!(buf4.data[0], b'\xf0')
  assert_eq!(buf4.data[1], b'\x04')
}

test "string encoding" {
  let buf = @lib.ByteBuffer::new()
  buf.emit_string("item")
  // Length 4, then 'i', 't', 'e', 'm'
  assert_eq!(buf.data.length(), 5)
  assert_eq!(buf.data[0], b'\x04')
  assert_eq!(buf.data[1], b'\x69')
  assert_eq!(buf.data[2], b'\x74')
  assert_eq!(buf.data[3], b'\x65')
  assert_eq!(buf.data[4], b'\x6d')
}

test "component header" {
  let world : @lib.WitWorld = @lib.WitWorld::{
    package_: "test:empty@1.0.0",
    name: "empty-world",
    types: [],
    funcs: [],
    exports: [],
    imports: [],
  }
  let result = @lib.encode(world)
  // Should at least have the 8-byte component header + custom section
  assert_eq!(result[0], b'\x00')
  assert_eq!(result[1], b'\x61')
  assert_eq!(result[2], b'\x73')
  assert_eq!(result[3], b'\x6d')
  assert_eq!(result[4], b'\x0d')
  assert_eq!(result[5], b'\x00')
  assert_eq!(result[6], b'\x01')
  assert_eq!(result[7], b'\x00')
}

/// Convert a byte to a two-character hex string.
fn byte_to_hex(b : Byte) -> String {
  let hi = b.to_int().lsr(4).land(0x0f)
  let lo = b.to_int().land(0x0f)
  let hex_chars = "0123456789abcdef"
  "\{hex_chars[hi]}\{hex_chars[lo]}"
}
