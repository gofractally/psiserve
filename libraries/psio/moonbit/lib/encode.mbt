// Component Model binary encoder for WIT worlds.
//
// Takes a WitWorld and serializes it to the standard component-type custom
// section binary encoding -- the same format that wasm-tools and wit-component
// produce.

// ---- Component Model binary opcodes ----

let section_custom : Byte = b'\x00'

let section_type : Byte = b'\x07'

let section_export : Byte = b'\x0b'

let type_component : Byte = b'\x41'

let type_instance : Byte = b'\x42'

let type_func : Byte = b'\x40'

let def_record : Byte = b'\x72'

let def_variant : Byte = b'\x71'

let def_list : Byte = b'\x70'

let def_tuple : Byte = b'\x6f'

let def_flags : Byte = b'\x6e'

let def_enum : Byte = b'\x6d'

let def_option : Byte = b'\x6b'

let def_result : Byte = b'\x6a'

let prim_bool_byte : Byte = b'\x7f'

let prim_s8_byte : Byte = b'\x7e'

let prim_u8_byte : Byte = b'\x7d'

let prim_s16_byte : Byte = b'\x7c'

let prim_u16_byte : Byte = b'\x7b'

let prim_s32_byte : Byte = b'\x7a'

let prim_u32_byte : Byte = b'\x79'

let prim_s64_byte : Byte = b'\x78'

let prim_u64_byte : Byte = b'\x77'

let prim_f32_byte : Byte = b'\x76'

let prim_f64_byte : Byte = b'\x75'

let prim_char_byte : Byte = b'\x74'

let prim_string_byte : Byte = b'\x73'

let item_type_def : Byte = b'\x01'

let item_export : Byte = b'\x04'

let sort_func : Byte = b'\x01'

let sort_type : Byte = b'\x03'

let sort_component : Byte = b'\x04'

let sort_instance : Byte = b'\x05'

let name_kebab : Byte = b'\x00'

let bound_eq : Byte = b'\x00'

let result_single : Byte = b'\x00'

let result_named : Byte = b'\x01'

// ---- ByteBuffer ----

pub(all) struct ByteBuffer {
  data : Array[Byte]
}

pub fn ByteBuffer::new() -> ByteBuffer {
  { data: [] }
}

pub fn ByteBuffer::emit_byte(self : ByteBuffer, b : Byte) -> Unit {
  self.data.push(b)
}

pub fn ByteBuffer::emit_uleb128(self : ByteBuffer, value : Int) -> Unit {
  let mut val = value
  while true {
    let mut b = val.land(0x7f)
    val = val.lsr(7)
    if val != 0 {
      b = b.lor(0x80)
    }
    self.data.push(b.to_byte())
    if val == 0 {
      break
    }
  }
}

pub fn ByteBuffer::emit_string(self : ByteBuffer, s : String) -> Unit {
  // Get UTF-8 bytes from string
  let utf8 = string_to_utf8(s)
  self.emit_uleb128(utf8.length())
  for i = 0; i < utf8.length(); i = i + 1 {
    self.data.push(utf8[i])
  }
}

fn ByteBuffer::emit_bytes(self : ByteBuffer, other : Array[Byte]) -> Unit {
  for i = 0; i < other.length(); i = i + 1 {
    self.data.push(other[i])
  }
}

fn ByteBuffer::length(self : ByteBuffer) -> Int {
  self.data.length()
}

/// Convert a string to UTF-8 bytes.
fn string_to_utf8(s : String) -> Array[Byte] {
  let result : Array[Byte] = []
  let len = s.length()
  let mut i = 0
  while i < len {
    let c = s[i].to_int()
    if c < 0x80 {
      result.push(c.to_byte())
    } else if c < 0x800 {
      result.push((0xC0 | (c >> 6)).to_byte())
      result.push((0x80 | (c & 0x3F)).to_byte())
    } else if c >= 0xD800 && c <= 0xDBFF {
      // High surrogate -- combine with low surrogate
      if i + 1 < len {
        let c2 = s[i + 1].to_int()
        if c2 >= 0xDC00 && c2 <= 0xDFFF {
          let cp = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00)
          result.push((0xF0 | (cp >> 18)).to_byte())
          result.push((0x80 | ((cp >> 12) & 0x3F)).to_byte())
          result.push((0x80 | ((cp >> 6) & 0x3F)).to_byte())
          result.push((0x80 | (cp & 0x3F)).to_byte())
          i = i + 2 // Skip the low surrogate
          continue
        }
      }
      // Lone surrogate -- encode as replacement char
      result.push(b'\xEF')
      result.push(b'\xBF')
      result.push(b'\xBD')
    } else {
      result.push((0xE0 | (c >> 12)).to_byte())
      result.push((0x80 | ((c >> 6) & 0x3F)).to_byte())
      result.push((0x80 | (c & 0x3F)).to_byte())
    }
    i = i + 1
  }
  result
}

// ---- Prim to byte mapping ----

fn prim_to_cm_byte(p : WitPrim) -> Byte {
  match p {
    Bool_ => prim_bool_byte
    U8 => prim_u8_byte
    S8 => prim_s8_byte
    U16 => prim_u16_byte
    S16 => prim_s16_byte
    U32 => prim_u32_byte
    S32 => prim_s32_byte
    U64 => prim_u64_byte
    S64 => prim_s64_byte
    F32 => prim_f32_byte
    F64 => prim_f64_byte
    Char_ => prim_char_byte
    String_ => prim_string_byte
  }
}

// ---- Encoding state ----

struct EncodeState {
  mut next_type_idx : Int
  mut item_count : Int
}

// ---- Emit helpers ----

/// Emit a valtype reference: primitive byte or remapped type index.
fn emit_valtype(
  buf : ByteBuffer,
  type_idx : Int,
  remap : Map[Int, Int]
) -> Unit {
  if is_prim_idx(type_idx) {
    buf.emit_byte(prim_to_cm_byte(idx_to_prim(type_idx)))
  } else {
    match remap.get(type_idx) {
      Some(idx) => buf.emit_uleb128(idx)
      None => buf.emit_uleb128(type_idx)
    }
  }
}

/// Emit a defined type (record, list, option, etc.)
fn emit_defined_type(
  buf : ByteBuffer,
  td : WitTypeDef,
  remap : Map[Int, Int]
) -> Unit {
  match td.kind {
    Record => {
      buf.emit_byte(def_record)
      buf.emit_uleb128(td.fields.length())
      for i = 0; i < td.fields.length(); i = i + 1 {
        let f = td.fields[i]
        buf.emit_string(f.name)
        emit_valtype(buf, f.type_idx, remap)
      }
    }
    List => {
      buf.emit_byte(def_list)
      emit_valtype(buf, td.element_type_idx, remap)
    }
    Option_ => {
      buf.emit_byte(def_option)
      emit_valtype(buf, td.element_type_idx, remap)
    }
    Result_ => {
      buf.emit_byte(def_result)
      emit_valtype(buf, td.element_type_idx, remap)
      emit_valtype(buf, td.error_type_idx, remap)
    }
    Variant => {
      buf.emit_byte(def_variant)
      buf.emit_uleb128(td.fields.length())
      for i = 0; i < td.fields.length(); i = i + 1 {
        let f = td.fields[i]
        buf.emit_string(f.name)
        emit_valtype(buf, f.type_idx, remap)
      }
    }
    Enum_ => {
      buf.emit_byte(def_enum)
      buf.emit_uleb128(td.fields.length())
      for i = 0; i < td.fields.length(); i = i + 1 {
        let f = td.fields[i]
        buf.emit_string(f.name)
      }
    }
    Flags => {
      buf.emit_byte(def_flags)
      buf.emit_uleb128(td.fields.length())
      for i = 0; i < td.fields.length(); i = i + 1 {
        let f = td.fields[i]
        buf.emit_string(f.name)
      }
    }
    Tuple => {
      buf.emit_byte(def_tuple)
      buf.emit_uleb128(td.fields.length())
      for i = 0; i < td.fields.length(); i = i + 1 {
        let f = td.fields[i]
        emit_valtype(buf, f.type_idx, remap)
      }
    }
  }
}

/// Emit a func type definition.
fn emit_func_type(
  buf : ByteBuffer,
  func : WitFunc,
  remap : Map[Int, Int]
) -> Unit {
  buf.emit_byte(type_func)
  // Parameters
  buf.emit_uleb128(func.params.length())
  for i = 0; i < func.params.length(); i = i + 1 {
    let p = func.params[i]
    buf.emit_string(p.name)
    emit_valtype(buf, p.type_idx, remap)
  }
  // Results
  if func.results.length() == 1 && func.results[0].name == "" {
    // Single unnamed result
    buf.emit_byte(result_single)
    emit_valtype(buf, func.results[0].type_idx, remap)
  } else {
    // Named results (or void = empty)
    buf.emit_byte(result_named)
    buf.emit_uleb128(func.results.length())
    for i = 0; i < func.results.length(); i = i + 1 {
      let r = func.results[i]
      buf.emit_string(r.name)
      emit_valtype(buf, r.type_idx, remap)
    }
  }
}

/// Emit an extern name (discriminant + string).
fn emit_extern_name(buf : ByteBuffer, name : String) -> Unit {
  buf.emit_byte(name_kebab)
  buf.emit_string(name)
}

/// Ensure a type is emitted, recursively emitting dependencies first.
fn ensure_emitted(
  world : WitWorld,
  type_idx : Int,
  buf : ByteBuffer,
  remap : Map[Int, Int],
  state : EncodeState
) -> Unit {
  if is_prim_idx(type_idx) {
    return
  }
  match remap.get(type_idx) {
    Some(_) => return
    None => ()
  }
  if type_idx < 0 || type_idx >= world.types.length() {
    return
  }
  let td = world.types[type_idx]

  // Ensure dependencies are emitted first
  match td.kind {
    Record => {
      for i = 0; i < td.fields.length(); i = i + 1 {
        ensure_emitted(world, td.fields[i].type_idx, buf, remap, state)
      }
    }
    List | Option_ => {
      ensure_emitted(world, td.element_type_idx, buf, remap, state)
    }
    Result_ => {
      ensure_emitted(world, td.element_type_idx, buf, remap, state)
      ensure_emitted(world, td.error_type_idx, buf, remap, state)
    }
    Variant | Tuple => {
      for i = 0; i < td.fields.length(); i = i + 1 {
        ensure_emitted(world, td.fields[i].type_idx, buf, remap, state)
      }
    }
    _ => ()
  }

  // Check again -- dependency resolution may have emitted this
  match remap.get(type_idx) {
    Some(_) => return
    None => ()
  }

  // Emit type definition
  buf.emit_byte(item_type_def)
  emit_defined_type(buf, td, remap)
  let def_idx = state.next_type_idx
  state.next_type_idx = state.next_type_idx + 1
  state.item_count = state.item_count + 1

  if td.name != "" {
    // Named type: emit export alias
    buf.emit_byte(item_export)
    emit_extern_name(buf, td.name)
    buf.emit_byte(sort_type)
    buf.emit_byte(bound_eq)
    buf.emit_uleb128(def_idx)
    remap.set(type_idx, state.next_type_idx)
    state.next_type_idx = state.next_type_idx + 1
    state.item_count = state.item_count + 1
  } else {
    // Anonymous type: remap directly to def index
    remap.set(type_idx, def_idx)
  }
}

/// Build the fully qualified interface name.
fn qualified_interface_name(world : WitWorld, iface : WitInterface) -> String {
  let pkg = world.package_
  // Find '@' position
  let mut at_pos = -1
  for i = 0; i < pkg.length(); i = i + 1 {
    if pkg[i] == '@' {
      at_pos = i
      break
    }
  }
  if at_pos >= 0 {
    let before_at = pkg.substring(start=0, end=at_pos)
    let from_at = pkg.substring(start=at_pos)
    "\{before_at}/\{iface.name}\{from_at}"
  } else {
    "\{pkg}/\{iface.name}"
  }
}

/// Build the fully qualified world name.
fn qualified_world_name(world : WitWorld) -> String {
  let pkg = world.package_
  let mut at_pos = -1
  for i = 0; i < pkg.length(); i = i + 1 {
    if pkg[i] == '@' {
      at_pos = i
      break
    }
  }
  if at_pos >= 0 {
    let before_at = pkg.substring(start=0, end=at_pos)
    let from_at = pkg.substring(start=at_pos)
    "\{before_at}/\{world.name}\{from_at}"
  } else {
    "\{pkg}/\{world.name}"
  }
}

/// Encode an interface, returning the instance body bytes and item count.
fn encode_interface(
  world : WitWorld,
  iface : WitInterface
) -> (Array[Byte], Int) {
  let remap : Map[Int, Int] = {}
  let state : EncodeState = { next_type_idx: 0, item_count: 0 }
  let buf = ByteBuffer::new()

  // Phase 1: Emit all named record types (and their dependencies)
  for i = 0; i < iface.type_idxs.length(); i = i + 1 {
    ensure_emitted(world, iface.type_idxs[i], buf, remap, state)
  }

  // Phase 2: Emit functions, lazily emitting remaining anonymous types
  for i = 0; i < iface.func_idxs.length(); i = i + 1 {
    let func_idx = iface.func_idxs[i]
    if func_idx >= world.funcs.length() {
      continue
    }
    let func = world.funcs[func_idx]

    // Ensure param/result types are emitted
    for j = 0; j < func.params.length(); j = j + 1 {
      ensure_emitted(world, func.params[j].type_idx, buf, remap, state)
    }
    for j = 0; j < func.results.length(); j = j + 1 {
      ensure_emitted(world, func.results[j].type_idx, buf, remap, state)
    }

    // Emit func type definition
    buf.emit_byte(item_type_def)
    emit_func_type(buf, func, remap)
    let func_type_idx = state.next_type_idx
    state.next_type_idx = state.next_type_idx + 1
    state.item_count = state.item_count + 1

    // Emit func export
    buf.emit_byte(item_export)
    emit_extern_name(buf, func.name)
    buf.emit_byte(sort_func)
    buf.emit_uleb128(func_type_idx)
    state.item_count = state.item_count + 1
  }

  (buf.data, state.item_count)
}

/// Encode a WitWorld to Component Model binary format.
/// Returns bytes suitable for a component-type custom section.
pub fn encode(world : WitWorld) -> Array[Byte] {
  let out = ByteBuffer::new()

  // Component header
  out.emit_byte(b'\x00')
  out.emit_byte(b'\x61')
  out.emit_byte(b'\x73')
  out.emit_byte(b'\x6d')
  out.emit_byte(b'\x0d')
  out.emit_byte(b'\x00')
  out.emit_byte(b'\x01')
  out.emit_byte(b'\x00')

  // Custom section: wit-component-encoding (version 4, UTF-8)
  let cs = ByteBuffer::new()
  cs.emit_string("wit-component-encoding")
  cs.emit_byte(b'\x04') // version
  cs.emit_byte(b'\x00') // encoding: UTF-8
  out.emit_byte(section_custom)
  out.emit_uleb128(cs.length())
  out.emit_bytes(cs.data)

  // Must have at least one export
  if world.exports.length() == 0 {
    return out.data
  }

  let exp_iface = world.exports[0]
  let (inst_bytes, inst_item_count) = encode_interface(world, exp_iface)

  // Type section: 1 type = outer COMPONENT
  let type_sec = ByteBuffer::new()
  type_sec.emit_uleb128(1) // 1 type

  // Outer component (2 items: inner component + world export)
  type_sec.emit_byte(type_component)
  type_sec.emit_uleb128(2)

  // Item 0: inner component (2 items: instance + interface export)
  type_sec.emit_byte(item_type_def)
  type_sec.emit_byte(type_component)
  type_sec.emit_uleb128(2)

  // Inner item 0: instance type
  type_sec.emit_byte(item_type_def)
  type_sec.emit_byte(type_instance)
  type_sec.emit_uleb128(inst_item_count)
  type_sec.emit_bytes(inst_bytes)

  // Inner item 1: export interface as instance(0)
  type_sec.emit_byte(item_export)
  emit_extern_name(type_sec, qualified_interface_name(world, exp_iface))
  type_sec.emit_byte(sort_instance)
  type_sec.emit_uleb128(0)

  // Outer item 1: export world as component(0)
  type_sec.emit_byte(item_export)
  emit_extern_name(type_sec, qualified_world_name(world))
  type_sec.emit_byte(sort_component)
  type_sec.emit_uleb128(0)

  // Write the type section
  out.emit_byte(section_type)
  out.emit_uleb128(type_sec.length())
  out.emit_bytes(type_sec.data)

  // Export section: export world name as type(0)
  let exp_sec = ByteBuffer::new()
  exp_sec.emit_uleb128(1) // 1 export
  emit_extern_name(exp_sec, world.name)
  exp_sec.emit_byte(sort_type)
  exp_sec.emit_uleb128(0) // type index 0
  exp_sec.emit_byte(b'\x00') // no explicit type annotation

  out.emit_byte(section_export)
  out.emit_uleb128(exp_sec.length())
  out.emit_bytes(exp_sec.data)

  // Custom section: producers
  let prod = ByteBuffer::new()
  prod.emit_string("producers")
  prod.emit_uleb128(1) // 1 field
  prod.emit_string("processed-by")
  prod.emit_uleb128(1) // 1 entry
  prod.emit_string("psio-wit-gen")
  prod.emit_string("1.0.0")

  out.emit_byte(section_custom)
  out.emit_uleb128(prod.length())
  out.emit_bytes(prod.data)

  out.data
}
