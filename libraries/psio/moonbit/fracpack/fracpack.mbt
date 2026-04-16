// Fracpack binary serialization format for MoonBit.
//
// Provides pack, unpack, and zero-copy view types.
// All multi-byte integers are little-endian.

// ── Field kinds ─────────────────────────────────────────────────────────

pub enum FieldKind {
  Bool_
  U8
  I8
  U16
  I16
  U32
  I32
  U64
  I64
  F32
  F64
  String_
  Vec
  Optional
  Object
  Variant
  Tuple
} derive(Eq, Show)

// ── Type/Field specs ────────────────────────────────────────────────────

pub struct FieldSpec {
  name : String
  kind : FieldKind
  fixed_size : Int
  is_var : Bool
  elem_kind : FieldKind
  elem_spec : TypeSpec?
  inner_spec : TypeSpec?
}

pub struct TypeSpec {
  name : String
  extensible : Bool
  fields : Array[FieldSpec]
  is_variant : Bool
  is_tuple : Bool
  cases : Array[FieldSpec]
}

fn TypeSpec::fixed_size(self : TypeSpec) -> Int {
  let mut total = 0
  for f in self.fields {
    total = total + f.fixed_size
  }
  total
}

// ── Dynamic value ───────────────────────────────────────────────────────

pub struct Value {
  kind : FieldKind
  bool_v : Bool
  u8_v : Byte
  i8_v : Int      // stored as signed Int, truncated to 8 bits
  u16_v : Int
  i16_v : Int     // stored as signed Int, truncated to 16 bits
  u32_v : UInt
  i32_v : Int
  u64_v : UInt64
  i64_v : Int64
  f32_v : Float
  f64_v : Double
  str : String
  vec : Array[Value]
  opt : Value?
  fields : Array[Value]
  tag : Int
  variant : Value?
}

pub fn Value::new() -> Value {
  {
    kind: U32,
    bool_v: false,
    u8_v: b'\x00',
    i8_v: 0,
    u16_v: 0,
    i16_v: 0,
    u32_v: 0U,
    i32_v: 0,
    u64_v: 0UL,
    i64_v: 0L,
    f32_v: 0.0,
    f64_v: 0.0,
    str: "",
    vec: [],
    opt: None,
    fields: [],
    tag: 0,
    variant: None,
  }
}

// ── Byte buffer ─────────────────────────────────────────────────────────

struct PackBuf {
  data : Array[Byte]
}

fn PackBuf::new() -> PackBuf {
  { data: [] }
}

fn PackBuf::len(self : PackBuf) -> Int {
  self.data.length()
}

fn PackBuf::append_zeros(self : PackBuf, n : Int) -> Unit {
  for _i = 0; _i < n; _i = _i + 1 {
    self.data.push(b'\x00')
  }
}

fn PackBuf::append_slice(self : PackBuf, src : Array[Byte]) -> Unit {
  for b in src {
    self.data.push(b)
  }
}

fn PackBuf::append_str_bytes(self : PackBuf, s : String) -> Unit {
  let bytes = string_to_utf8(s)
  self.append_slice(bytes)
}

fn PackBuf::write_u16_le(self : PackBuf, pos : Int, val : Int) -> Unit {
  self.data[pos] = (val.land(0xFF)).to_byte()
  self.data[pos + 1] = (val.lsr(8).land(0xFF)).to_byte()
}

fn PackBuf::write_u32_le(self : PackBuf, pos : Int, val : UInt) -> Unit {
  self.data[pos] = val.to_byte()
  self.data[pos + 1] = (val >> 8).to_byte()
  self.data[pos + 2] = (val >> 16).to_byte()
  self.data[pos + 3] = (val >> 24).to_byte()
}

fn PackBuf::write_u64_le(self : PackBuf, pos : Int, val : UInt64) -> Unit {
  self.data[pos] = val.to_byte()
  self.data[pos + 1] = (val >> 8).to_byte()
  self.data[pos + 2] = (val >> 16).to_byte()
  self.data[pos + 3] = (val >> 24).to_byte()
  self.data[pos + 4] = (val >> 32).to_byte()
  self.data[pos + 5] = (val >> 40).to_byte()
  self.data[pos + 6] = (val >> 48).to_byte()
  self.data[pos + 7] = (val >> 56).to_byte()
}

// ── UTF-8 encoding ──────────────────────────────────────────────────────

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
      if i + 1 < len {
        let c2 = s[i + 1].to_int()
        if c2 >= 0xDC00 && c2 <= 0xDFFF {
          let cp = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00)
          result.push((0xF0 | (cp >> 18)).to_byte())
          result.push((0x80 | ((cp >> 12) & 0x3F)).to_byte())
          result.push((0x80 | ((cp >> 6) & 0x3F)).to_byte())
          result.push((0x80 | (cp & 0x3F)).to_byte())
          i = i + 2
          continue
        }
      }
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

fn utf8_byte_len(s : String) -> Int {
  string_to_utf8(s).length()
}

// ── Packing ─────────────────────────────────────────────────────────────

pub fn pack(spec : TypeSpec, val : Value) -> Array[Byte] {
  let buf = PackBuf::new()
  // Tuples and objects share the same encoding; tuples are always extensible.
  pack_object(spec, val, buf)
  buf.data
}

pub fn pack_tuple(spec : TypeSpec, val : Value) -> Array[Byte] {
  let buf = PackBuf::new()
  pack_object(spec, val, buf)
  buf.data
}

fn pack_object(spec : TypeSpec, val : Value, buf : PackBuf) -> Unit {
  let fixed_total = spec.fixed_size()
  let header_size = if spec.extensible { 2 } else { 0 }
  let actual_fixed = if spec.extensible {
    compute_actual_fixed(spec, val)
  } else {
    fixed_total
  }

  let fixed_start = buf.len()
  buf.append_zeros(header_size + actual_fixed)
  if spec.extensible {
    buf.write_u16_le(fixed_start, actual_fixed)
  }

  // Write fixed-size fields
  let mut fixed_pos = fixed_start + header_size
  for i = 0; i < spec.fields.length(); i = i + 1 {
    if fixed_pos - fixed_start - header_size >= actual_fixed {
      break
    }
    let f = spec.fields[i]
    if not(f.is_var) {
      write_fixed_value(buf, fixed_pos, f.kind, val.fields[i])
    }
    fixed_pos = fixed_pos + f.fixed_size
  }

  // Write variable-size fields (heap data + offsets)
  fixed_pos = fixed_start + header_size
  for i = 0; i < spec.fields.length(); i = i + 1 {
    if fixed_pos - fixed_start - header_size >= actual_fixed {
      break
    }
    let f = spec.fields[i]
    if f.is_var {
      let fv = val.fields[i]
      let heap_len = pack_heap_field(f, fv, buf)
      if heap_len == 0 {
        if f.kind == Optional && fv.opt.is_empty() {
          buf.write_u32_le(fixed_pos, 1U)
        } else {
          buf.write_u32_le(fixed_pos, 0U)
        }
      } else {
        let heap_start = buf.len() - heap_len
        buf.write_u32_le(fixed_pos, (heap_start - fixed_pos).to_uint())
      }
    }
    fixed_pos = fixed_pos + f.fixed_size
  }
}

fn compute_actual_fixed(spec : TypeSpec, val : Value) -> Int {
  let mut last_present = -1
  for i = 0; i < spec.fields.length(); i = i + 1 {
    if i >= val.fields.length() {
      break
    }
    if spec.fields[i].kind == Optional {
      match val.fields[i].opt {
        Some(_) => last_present = i
        None => ()
      }
    } else {
      last_present = i
    }
  }
  if last_present < 0 {
    return 0
  }
  let mut size = 0
  for j = 0; j <= last_present; j = j + 1 {
    size = size + spec.fields[j].fixed_size
  }
  size
}

fn write_fixed_value(buf : PackBuf, pos : Int, kind : FieldKind, val : Value) -> Unit {
  match kind {
    Bool_ => buf.data[pos] = if val.bool_v { b'\x01' } else { b'\x00' }
    U8 => buf.data[pos] = val.u8_v
    I8 => buf.data[pos] = val.i8_v.land(0xFF).to_byte()
    U16 => buf.write_u16_le(pos, val.u16_v)
    I16 => buf.write_u16_le(pos, val.i16_v.land(0xFFFF))
    U32 => buf.write_u32_le(pos, val.u32_v)
    I32 => buf.write_u32_le(pos, val.i32_v.reinterpret_as_uint())
    U64 => buf.write_u64_le(pos, val.u64_v)
    I64 => buf.write_u64_le(pos, val.i64_v.reinterpret_as_uint64())
    F32 => buf.write_u32_le(pos, val.f32_v.reinterpret_as_uint())
    F64 => buf.write_u64_le(pos, val.f64_v.reinterpret_as_uint64())
    _ => ()
  }
}

fn pack_heap_field(f : FieldSpec, val : Value, buf : PackBuf) -> Int {
  let before = buf.len()
  match f.kind {
    String_ => {
      let bytes = string_to_utf8(val.str)
      if bytes.length() == 0 {
        return 0
      }
      buf.append_zeros(4)
      buf.write_u32_le(before, bytes.length().to_uint())
      buf.append_slice(bytes)
    }
    Vec => {
      if val.vec.length() == 0 {
        return 0
      }
      pack_vec_data(f, val.vec, buf)
    }
    Optional => {
      match val.opt {
        None => return 0
        Some(inner) => pack_opt_payload(f, inner, buf)
      }
    }
    Object => {
      match f.inner_spec {
        Some(inner) => pack_object(inner, val, buf)
        None => ()
      }
    }
    Variant => {
      match f.inner_spec {
        Some(inner) => pack_variant_data(inner, val, buf)
        None => ()
      }
    }
    Tuple => {
      match f.inner_spec {
        Some(inner) => pack_object(inner, val, buf)
        None => ()
      }
    }
    _ => ()
  }
  buf.len() - before
}

fn pack_vec_data(f : FieldSpec, elems : Array[Value], buf : PackBuf) -> Unit {
  match f.elem_spec {
    Some(es) =>
      if es.fields.length() > 0 {
        pack_var_elem_vec(es, elems, buf)
        return
      }
    None => ()
  }
  match f.elem_kind {
    String_ => pack_string_vec(elems, buf)
    Optional => pack_optional_vec(f, elems, buf)
    Vec => pack_nested_vec(elems, buf)
    _ => pack_primitive_vec(elems, buf)
  }
}

fn pack_primitive_vec(elems : Array[Value], buf : PackBuf) -> Unit {
  let kind = elems[0].kind
  let elem_size = fixed_size_of(kind)
  let n = elems.length()
  let total_bytes = n * elem_size
  let start = buf.len()
  buf.append_zeros(4 + total_bytes)
  buf.write_u32_le(start, total_bytes.to_uint())
  let mut pos = start + 4
  for e in elems {
    write_fixed_value(buf, pos, kind, e)
    pos = pos + elem_size
  }
}

fn pack_string_vec(elems : Array[Value], buf : PackBuf) -> Unit {
  let n = elems.length()
  let fixed_bytes = n * 4
  let start = buf.len()

  // Compute heap sizes
  let mut heap_total = 0
  for e in elems {
    let slen = utf8_byte_len(e.str)
    if slen > 0 {
      heap_total = heap_total + 4 + slen
    }
  }

  buf.append_zeros(4 + fixed_bytes + heap_total)
  buf.write_u32_le(start, fixed_bytes.to_uint())

  let heap_start = start + 4 + fixed_bytes
  let mut heap_off = 0
  for i = 0; i < n; i = i + 1 {
    let off_pos = start + 4 + i * 4
    let bytes = string_to_utf8(elems[i].str)
    if bytes.length() == 0 {
      buf.write_u32_le(off_pos, 0U)
    } else {
      buf.write_u32_le(off_pos, (heap_start + heap_off - off_pos).to_uint())
      let slen = bytes.length()
      buf.write_u32_le(heap_start + heap_off, slen.to_uint())
      for j = 0; j < slen; j = j + 1 {
        buf.data[heap_start + heap_off + 4 + j] = bytes[j]
      }
      heap_off = heap_off + 4 + slen
    }
  }
}

fn pack_var_elem_vec(
  elem_spec : TypeSpec,
  elems : Array[Value],
  buf : PackBuf
) -> Unit {
  let n = elems.length()
  let fixed_bytes = n * 4

  // Pack each element to temp buffers
  let temp_bufs : Array[Array[Byte]] = []
  let mut heap_total = 0
  for i = 0; i < n; i = i + 1 {
    let inner = PackBuf::new()
    pack_object(elem_spec, elems[i], inner)
    temp_bufs.push(inner.data)
    heap_total = heap_total + inner.data.length()
  }

  let start = buf.len()
  buf.append_zeros(4 + fixed_bytes + heap_total)
  buf.write_u32_le(start, fixed_bytes.to_uint())

  let heap_start = start + 4 + fixed_bytes
  let mut heap_off = 0
  for i = 0; i < n; i = i + 1 {
    let off_pos = start + 4 + i * 4
    if temp_bufs[i].length() == 0 {
      buf.write_u32_le(off_pos, 0U)
    } else {
      buf.write_u32_le(off_pos, (heap_start + heap_off - off_pos).to_uint())
      let tlen = temp_bufs[i].length()
      for j = 0; j < tlen; j = j + 1 {
        buf.data[heap_start + heap_off + j] = temp_bufs[i][j]
      }
      heap_off = heap_off + tlen
    }
  }
}

fn pack_optional_vec(
  f : FieldSpec,
  elems : Array[Value],
  buf : PackBuf
) -> Unit {
  let n = elems.length()
  let fixed_bytes = n * 4

  // Pack payloads to temp
  let parts : Array[Array[Byte]] = []
  let mut heap_total = 0
  for i = 0; i < n; i = i + 1 {
    match elems[i].opt {
      None => parts.push([])
      Some(inner) => {
        let inner_buf = PackBuf::new()
        pack_opt_payload(f, inner, inner_buf)
        parts.push(inner_buf.data)
        heap_total = heap_total + inner_buf.data.length()
      }
    }
  }

  let start = buf.len()
  buf.append_zeros(4 + fixed_bytes + heap_total)
  buf.write_u32_le(start, fixed_bytes.to_uint())

  let heap_start = start + 4 + fixed_bytes
  let mut heap_off = 0
  for i = 0; i < n; i = i + 1 {
    let off_pos = start + 4 + i * 4
    if parts[i].length() == 0 {
      buf.write_u32_le(off_pos, 1U) // absent
    } else {
      buf.write_u32_le(off_pos, (heap_start + heap_off - off_pos).to_uint())
      let plen = parts[i].length()
      for j = 0; j < plen; j = j + 1 {
        buf.data[heap_start + heap_off + j] = parts[i][j]
      }
      heap_off = heap_off + plen
    }
  }
}

fn pack_nested_vec(elems : Array[Value], buf : PackBuf) -> Unit {
  let n = elems.length()
  let fixed_bytes = n * 4

  let inner_fd : FieldSpec = {
    name: "",
    kind: Vec,
    fixed_size: 4,
    is_var: true,
    elem_kind: U32,
    elem_spec: None,
    inner_spec: None,
  }

  let inner_packed : Array[Array[Byte]] = []
  let mut heap_total = 0
  for i = 0; i < n; i = i + 1 {
    if elems[i].vec.length() == 0 {
      inner_packed.push([])
    } else {
      let inner = PackBuf::new()
      pack_vec_data(inner_fd, elems[i].vec, inner)
      inner_packed.push(inner.data)
      heap_total = heap_total + inner.data.length()
    }
  }

  let start = buf.len()
  buf.append_zeros(4 + fixed_bytes + heap_total)
  buf.write_u32_le(start, fixed_bytes.to_uint())

  let heap_start = start + 4 + fixed_bytes
  let mut heap_off = 0
  for i = 0; i < n; i = i + 1 {
    let off_pos = start + 4 + i * 4
    if inner_packed[i].length() == 0 {
      buf.write_u32_le(off_pos, 0U)
    } else {
      buf.write_u32_le(off_pos, (heap_start + heap_off - off_pos).to_uint())
      let plen = inner_packed[i].length()
      for j = 0; j < plen; j = j + 1 {
        buf.data[heap_start + heap_off + j] = inner_packed[i][j]
      }
      heap_off = heap_off + plen
    }
  }
}

fn pack_opt_payload(f : FieldSpec, val : Value, buf : PackBuf) -> Unit {
  match f.elem_kind {
    Object => {
      match f.elem_spec {
        Some(es) => { pack_object(es, val, buf); return }
        None => ()
      }
    }
    Vec => {
      if val.vec.length() == 0 {
        return
      }
      let inner_fd : FieldSpec = {
        name: "",
        kind: Vec,
        fixed_size: 4,
        is_var: true,
        elem_kind: U32,
        elem_spec: None,
        inner_spec: None,
      }
      pack_vec_data(inner_fd, val.vec, buf)
      return
    }
    _ => ()
  }
  if val.kind == String_ {
    let bytes = string_to_utf8(val.str)
    let start = buf.len()
    buf.append_zeros(4 + bytes.length())
    buf.write_u32_le(start, bytes.length().to_uint())
    for j = 0; j < bytes.length(); j = j + 1 {
      buf.data[start + 4 + j] = bytes[j]
    }
    return
  }
  // Fixed-size
  let sz = fixed_size_of(val.kind)
  let start = buf.len()
  buf.append_zeros(sz)
  write_fixed_value(buf, start, val.kind, val)
}

fn pack_variant_data(
  spec : TypeSpec,
  val : Value,
  buf : PackBuf
) -> Unit {
  // Pack content to temp buffer
  let content = PackBuf::new()
  match val.variant {
    Some(v) => {
      let case_spec = spec.cases[val.tag]
      match case_spec.inner_spec {
        Some(inner) => pack_object(inner, v, content)
        None => {
          if case_spec.kind == String_ {
            let bytes = string_to_utf8(v.str)
            content.append_zeros(4 + bytes.length())
            content.write_u32_le(0, bytes.length().to_uint())
            for j = 0; j < bytes.length(); j = j + 1 {
              content.data[4 + j] = bytes[j]
            }
          } else {
            let sz = fixed_size_of(case_spec.kind)
            content.append_zeros(sz)
            write_fixed_value(content, 0, case_spec.kind, v)
          }
        }
      }
    }
    None => ()
  }

  // [u8 tag][u32 content_size][content]
  let start = buf.len()
  buf.append_zeros(1 + 4 + content.len())
  buf.data[start] = val.tag.to_byte()
  buf.write_u32_le(start + 1, content.len().to_uint())
  for j = 0; j < content.len(); j = j + 1 {
    buf.data[start + 5 + j] = content.data[j]
  }
}

fn fixed_size_of(kind : FieldKind) -> Int {
  match kind {
    Bool_ | U8 | I8 => 1
    U16 | I16 => 2
    U32 | I32 | F32 => 4
    U64 | I64 | F64 => 8
    String_ | Vec | Optional | Object | Variant | Tuple => 4
  }
}

// ── View ────────────────────────────────────────────────────────────────

pub struct FracView {
  spec : TypeSpec
  data : Array[Byte]
  base : Int
}

pub fn FracView::new(spec : TypeSpec, data : Array[Byte]) -> FracView {
  { spec, data, base: 0 }
}

pub fn FracView::new_at(
  spec : TypeSpec,
  data : Array[Byte],
  pos : Int
) -> FracView {
  { spec, data, base: pos }
}

fn FracView::field_pos(self : FracView, name : String) -> (FieldSpec, Int) {
  let mut pos = self.base
  if self.spec.extensible {
    pos = pos + 2
  }
  for f in self.spec.fields {
    if f.name == name {
      return (f, pos)
    }
    pos = pos + f.fixed_size
  }
  abort("fracpack: field not found: " + name)
}

pub fn FracView::get_bool(self : FracView, name : String) -> Bool {
  let (_, pos) = self.field_pos(name)
  self.data[pos] != b'\x00'
}

pub fn FracView::get_u8(self : FracView, name : String) -> Byte {
  let (_, pos) = self.field_pos(name)
  self.data[pos]
}

pub fn FracView::get_u32(self : FracView, name : String) -> UInt {
  let (_, pos) = self.field_pos(name)
  read_u32(self.data, pos)
}

pub fn FracView::get_i32(self : FracView, name : String) -> Int {
  let (_, pos) = self.field_pos(name)
  read_u32(self.data, pos).reinterpret_as_int()
}

pub fn FracView::get_u64(self : FracView, name : String) -> UInt64 {
  let (_, pos) = self.field_pos(name)
  read_u64(self.data, pos)
}

pub fn FracView::get_i64(self : FracView, name : String) -> Int64 {
  let (_, pos) = self.field_pos(name)
  read_u64(self.data, pos).reinterpret_as_int64()
}

pub fn FracView::get_f32(self : FracView, name : String) -> Float {
  let (_, pos) = self.field_pos(name)
  read_u32(self.data, pos).reinterpret_as_float()
}

pub fn FracView::get_f64(self : FracView, name : String) -> Double {
  let (_, pos) = self.field_pos(name)
  read_u64(self.data, pos).reinterpret_as_double()
}

pub fn FracView::get_string(self : FracView, name : String) -> Array[Byte] {
  let (_, pos) = self.field_pos(name)
  let off = read_u32(self.data, pos)
  if off == 0U {
    return []
  }
  let hp = pos + off.reinterpret_as_int()
  let slen = read_u32(self.data, hp).reinterpret_as_int()
  let result : Array[Byte] = []
  for i = 0; i < slen; i = i + 1 {
    result.push(self.data[hp + 4 + i])
  }
  result
}

pub fn FracView::get_object(self : FracView, name : String) -> FracView? {
  let (f, pos) = self.field_pos(name)
  let off = read_u32(self.data, pos)
  if off == 0U {
    return None
  }
  match f.inner_spec {
    Some(inner) => Some(FracView::new_at(inner, self.data, pos + off.reinterpret_as_int()))
    None => None
  }
}

// ── Low-level read helpers ──────────────────────────────────────────────

fn read_u32(data : Array[Byte], pos : Int) -> UInt {
  data[pos].to_uint()
    | (data[pos + 1].to_uint() << 8)
    | (data[pos + 2].to_uint() << 16)
    | (data[pos + 3].to_uint() << 24)
}

fn read_u64(data : Array[Byte], pos : Int) -> UInt64 {
  data[pos].to_uint64()
    | (data[pos + 1].to_uint64() << 8)
    | (data[pos + 2].to_uint64() << 16)
    | (data[pos + 3].to_uint64() << 24)
    | (data[pos + 4].to_uint64() << 32)
    | (data[pos + 5].to_uint64() << 40)
    | (data[pos + 6].to_uint64() << 48)
    | (data[pos + 7].to_uint64() << 56)
}

// ── Read helpers for mutable arrays ─────────────────────────────────────

fn read_u16(data : Array[Byte], pos : Int) -> Int {
  data[pos].to_int() | (data[pos + 1].to_int() << 8)
}

// ── In-place mutation ──────────────────────────────────────────────────
//
// MutView wraps a mutable byte buffer and a TypeSpec, supporting both
// reading fields (like FracView) and writing fields with splice-and-patch
// (canonical mode) or overwrite-and-append (fast mode).

/// Replace `data[pos..pos+old_len]` with `new_bytes`, shifting the tail.
/// Returns the delta (positive = grew, negative = shrank, 0 = same size).
pub fn splice_buffer(
  data : Array[Byte],
  pos : Int,
  old_len : Int,
  new_bytes : Array[Byte]
) -> Int {
  let new_len = new_bytes.length()
  let delta = new_len - old_len
  if delta == 0 {
    // Same size: overwrite in place
    for i = 0; i < new_len; i = i + 1 {
      data[pos + i] = new_bytes[i]
    }
  } else if delta > 0 {
    // Grow: insert extra bytes at the end, then shift tail right
    let old_total = data.length()
    for _i = 0; _i < delta; _i = _i + 1 {
      data.push(b'\x00')
    }
    // Shift tail right
    let tail_start = pos + old_len
    let tail_len = old_total - tail_start
    for i = tail_len - 1; i >= 0; i = i - 1 {
      data[tail_start + delta + i] = data[tail_start + i]
    }
    // Write new bytes
    for i = 0; i < new_len; i = i + 1 {
      data[pos + i] = new_bytes[i]
    }
  } else {
    // Shrink: shift tail left, then remove extra bytes
    let neg_delta = -delta
    let tail_start = pos + old_len
    let tail_len = data.length() - tail_start
    // Shift tail left
    for i = 0; i < tail_len; i = i + 1 {
      data[tail_start - neg_delta + i] = data[tail_start + i]
    }
    // Write new bytes
    for i = 0; i < new_len; i = i + 1 {
      data[pos + i] = new_bytes[i]
    }
    // Remove trailing bytes
    for _i = 0; _i < neg_delta; _i = _i + 1 {
      let _ = data.pop()
    }
  }
  delta
}

/// Adjust a self-relative u32 offset at `offset_pos` after a splice.
/// Only adjusts if the offset is a valid pointer (>= 4) and the target
/// it points to is at or past `splice_pos`.
pub fn patch_offset(
  data : Array[Byte],
  offset_pos : Int,
  splice_pos : Int,
  delta : Int
) -> Unit {
  let offset_val = read_u32(data, offset_pos)
  if offset_val.reinterpret_as_int() <= 1 {
    // 0 = empty container, 1 = None marker -- never patch
    return
  }
  let target = offset_pos + offset_val.reinterpret_as_int()
  if target >= splice_pos {
    let new_val = offset_val.reinterpret_as_int() + delta
    let buf = PackBuf::new()
    buf.append_zeros(4)
    buf.write_u32_le(0, new_val.to_uint())
    data[offset_pos] = buf.data[0]
    data[offset_pos + 1] = buf.data[1]
    data[offset_pos + 2] = buf.data[2]
    data[offset_pos + 3] = buf.data[3]
  }
}

/// Write a u32 LE value into a mutable array at a given position.
fn write_u32_at(data : Array[Byte], pos : Int, val : UInt) -> Unit {
  data[pos] = val.to_byte()
  data[pos + 1] = (val >> 8).to_byte()
  data[pos + 2] = (val >> 16).to_byte()
  data[pos + 3] = (val >> 24).to_byte()
}

/// Write a u16 LE value into a mutable array at a given position.
fn write_u16_at(data : Array[Byte], pos : Int, val : Int) -> Unit {
  data[pos] = (val.land(0xFF)).to_byte()
  data[pos + 1] = (val.lsr(8).land(0xFF)).to_byte()
}

/// Measure the packed size of heap data at a given position for a field.
fn packed_size_at(field : FieldSpec, data : Array[Byte], pos : Int) -> Int {
  match field.kind {
    Bool_ => 1
    U8 | I8 => 1
    U16 | I16 => 2
    U32 | I32 | F32 => 4
    U64 | I64 | F64 => 8
    String_ => {
      let slen = read_u32(data, pos).reinterpret_as_int()
      4 + slen
    }
    Vec => packed_size_vec(field, data, pos)
    Optional => {
      // On heap, optionals store the inner value directly
      match field.elem_spec {
        Some(es) => packed_size_struct(es, data, pos)
        None => {
          match field.elem_kind {
            String_ => {
              let slen = read_u32(data, pos).reinterpret_as_int()
              4 + slen
            }
            Vec => packed_size_vec(field, data, pos)
            _ => fixed_size_of(field.elem_kind)
          }
        }
      }
    }
    Object | Tuple => {
      match field.inner_spec {
        Some(inner) => packed_size_struct(inner, data, pos)
        None => 0
      }
    }
    Variant => {
      // tag(1) + data_size(4) + data
      let data_size = read_u32(data, pos + 1).reinterpret_as_int()
      1 + 4 + data_size
    }
  }
}

fn packed_size_vec(field : FieldSpec, data : Array[Byte], pos : Int) -> Int {
  let data_size = read_u32(data, pos).reinterpret_as_int()
  if not(is_var_kind(field.elem_kind)) && field.elem_spec.is_empty() {
    return 4 + data_size
  }
  // Variable-size elements: walk each element's heap data
  let elem_fixed = fixed_size_of(field.elem_kind)
  let count = if elem_fixed > 0 { data_size / elem_fixed } else { 0 }
  let mut total = 4 + data_size
  for i = 0; i < count; i = i + 1 {
    let elem_offset_pos = pos + 4 + i * elem_fixed
    let elem_offset = read_u32(data, elem_offset_pos)
    if elem_offset.reinterpret_as_int() > 1 {
      let elem_target = elem_offset_pos + elem_offset.reinterpret_as_int()
      let elem_field : FieldSpec = {
        ..default_field_spec(),
        kind: field.elem_kind,
        elem_spec: field.elem_spec,
        inner_spec: field.elem_spec,
      }
      let elem_size = packed_size_at(elem_field, data, elem_target)
      let end = elem_target + elem_size
      if end - pos > total {
        total = end - pos
      }
    }
  }
  total
}

fn packed_size_struct(spec : TypeSpec, data : Array[Byte], pos : Int) -> Int {
  let header_size = if spec.extensible { 2 } else { 0 }
  let fixed_size = if spec.extensible {
    read_u16(data, pos)
  } else {
    spec.fixed_size()
  }
  let fixed_start = pos + header_size
  let mut end = fixed_start + fixed_size
  // Walk variable-size fields to find extent of heap data
  let mut field_pos = fixed_start
  for f in spec.fields {
    if field_pos >= fixed_start + fixed_size {
      break
    }
    if f.is_var {
      let off_val = read_u32(data, field_pos)
      if off_val.reinterpret_as_int() > 1 {
        let target = field_pos + off_val.reinterpret_as_int()
        let inner_size = packed_size_at(f, data, target)
        let candidate = target + inner_size
        if candidate > end {
          end = candidate
        }
      }
    }
    field_pos = field_pos + f.fixed_size
  }
  end - pos
}

fn is_var_kind(kind : FieldKind) -> Bool {
  match kind {
    String_ | Vec | Optional | Object | Variant | Tuple => true
    _ => false
  }
}

/// Mutable view over packed fracpack data.
///
/// Supports reading fields (like FracView) and writing fields.
/// In canonical mode, writes splice the buffer and patch sibling offsets.
/// In fast mode, writes overwrite in-place or append and update the offset.
pub struct MutView {
  spec : TypeSpec
  data : Array[Byte]
  mut base : Int
  fast : Bool
}

pub fn MutView::new(spec : TypeSpec, data : Array[Byte]) -> MutView {
  { spec, data, base: 0, fast: false }
}

pub fn MutView::new_fast(spec : TypeSpec, data : Array[Byte]) -> MutView {
  { spec, data, base: 0, fast: true }
}

pub fn MutView::new_at(
  spec : TypeSpec,
  data : Array[Byte],
  base : Int,
  fast : Bool
) -> MutView {
  { spec, data, base, fast }
}

/// Get the underlying data buffer.
pub fn MutView::to_bytes(self : MutView) -> Array[Byte] {
  let result : Array[Byte] = []
  for b in self.data {
    result.push(b)
  }
  result
}

/// Find the absolute position of a named field's fixed-region slot.
fn MutView::field_info(
  self : MutView,
  name : String
) -> (FieldSpec, Int, Int) {
  // Returns (field_spec, abs_offset_pos, field_index)
  let header_size = if self.spec.extensible { 2 } else { 0 }
  let mut pos = self.base + header_size
  for i = 0; i < self.spec.fields.length(); i = i + 1 {
    let f = self.spec.fields[i]
    if f.name == name {
      return (f, pos, i)
    }
    pos = pos + f.fixed_size
  }
  abort("MutView: field not found: " + name)
}

/// Check whether a field is elided (trailing optional elision).
fn MutView::is_field_elided(self : MutView, field_index : Int) -> Bool {
  if not(self.spec.extensible) {
    return false
  }
  let declared_fixed = read_u16(self.data, self.base)
  let mut offset = 0
  for j = 0; j < field_index; j = j + 1 {
    offset = offset + self.spec.fields[j].fixed_size
  }
  offset >= declared_fixed
}

/// Expand the fixed region to include a field at the given index.
/// Inserts zero-initialized offset slots. Returns the byte delta.
fn MutView::expand_fixed_region(self : MutView, field_index : Int) -> Int {
  let header_size = 2 // Only called for extensible specs
  let declared_fixed = read_u16(self.data, self.base)
  let mut needed_fixed = 0
  for j = 0; j <= field_index; j = j + 1 {
    needed_fixed = needed_fixed + self.spec.fields[j].fixed_size
  }
  if needed_fixed <= declared_fixed {
    return 0
  }
  let insert_pos = self.base + header_size + declared_fixed
  let insert_size = needed_fixed - declared_fixed
  // Build inserted bytes: optional fields get offset=1 (None), others get 0
  let insert_bytes : Array[Byte] = []
  for _i = 0; _i < insert_size; _i = _i + 1 {
    insert_bytes.push(b'\x00')
  }
  // Set optional field slots to 1 (None marker)
  let mut check_offset = 0
  for j = 0; j < self.spec.fields.length(); j = j + 1 {
    if check_offset >= declared_fixed && check_offset < needed_fixed {
      if self.spec.fields[j].kind == Optional {
        let slot_start = check_offset - declared_fixed
        // Write u32 LE value of 1
        insert_bytes[slot_start] = b'\x01'
        insert_bytes[slot_start + 1] = b'\x00'
        insert_bytes[slot_start + 2] = b'\x00'
        insert_bytes[slot_start + 3] = b'\x00'
      }
    }
    check_offset = check_offset + self.spec.fields[j].fixed_size
  }
  let delta = splice_buffer(self.data, insert_pos, 0, insert_bytes)
  // Update the header
  write_u16_at(self.data, self.base, needed_fixed)
  // Patch existing sibling offsets that point past the insert point
  let mut fpos = self.base + header_size
  for j = 0; j < self.spec.fields.length(); j = j + 1 {
    let mut field_offset = 0
    for k = 0; k < j; k = k + 1 {
      field_offset = field_offset + self.spec.fields[k].fixed_size
    }
    if field_offset >= declared_fixed {
      fpos = fpos + self.spec.fields[j].fixed_size
      continue
    }
    if self.spec.fields[j].is_var {
      patch_offset(self.data, fpos, insert_pos, delta)
    }
    fpos = fpos + self.spec.fields[j].fixed_size
  }
  delta
}

/// Patch all sibling variable-size field offsets after a splice.
fn MutView::patch_siblings(
  self : MutView,
  modified_index : Int,
  splice_pos : Int,
  delta : Int
) -> Unit {
  if delta == 0 {
    return
  }
  let header_size = if self.spec.extensible { 2 } else { 0 }
  let declared_fixed = if self.spec.extensible {
    read_u16(self.data, self.base)
  } else {
    self.spec.fixed_size()
  }
  let mut fpos = self.base + header_size
  for j = 0; j < self.spec.fields.length(); j = j + 1 {
    let mut field_offset = 0
    for k = 0; k < j; k = k + 1 {
      field_offset = field_offset + self.spec.fields[k].fixed_size
    }
    if field_offset >= declared_fixed {
      break
    }
    if j != modified_index && self.spec.fields[j].is_var {
      patch_offset(self.data, fpos, splice_pos, delta)
    }
    fpos = fpos + self.spec.fields[j].fixed_size
  }
}

// ── MutView readers ───────────────────────────────────────────────────

pub fn MutView::get_bool(self : MutView, name : String) -> Bool {
  let (_, pos, _) = self.field_info(name)
  self.data[pos] != b'\x00'
}

pub fn MutView::get_u8(self : MutView, name : String) -> Byte {
  let (_, pos, _) = self.field_info(name)
  self.data[pos]
}

pub fn MutView::get_u32(self : MutView, name : String) -> UInt {
  let (_, pos, _) = self.field_info(name)
  read_u32(self.data, pos)
}

pub fn MutView::get_i32(self : MutView, name : String) -> Int {
  let (_, pos, _) = self.field_info(name)
  read_u32(self.data, pos).reinterpret_as_int()
}

pub fn MutView::get_u64(self : MutView, name : String) -> UInt64 {
  let (_, pos, _) = self.field_info(name)
  read_u64(self.data, pos)
}

pub fn MutView::get_i64(self : MutView, name : String) -> Int64 {
  let (_, pos, _) = self.field_info(name)
  read_u64(self.data, pos).reinterpret_as_int64()
}

pub fn MutView::get_f32(self : MutView, name : String) -> Float {
  let (_, pos, _) = self.field_info(name)
  read_u32(self.data, pos).reinterpret_as_float()
}

pub fn MutView::get_f64(self : MutView, name : String) -> Double {
  let (_, pos, _) = self.field_info(name)
  read_u64(self.data, pos).reinterpret_as_double()
}

pub fn MutView::get_string(self : MutView, name : String) -> Array[Byte] {
  let (_, pos, _) = self.field_info(name)
  let off = read_u32(self.data, pos)
  if off == 0U {
    return []
  }
  let hp = pos + off.reinterpret_as_int()
  let slen = read_u32(self.data, hp).reinterpret_as_int()
  let result : Array[Byte] = []
  for i = 0; i < slen; i = i + 1 {
    result.push(self.data[hp + 4 + i])
  }
  result
}

pub fn MutView::get_opt_u32(self : MutView, name : String) -> UInt? {
  let (_, pos, idx) = self.field_info(name)
  if self.is_field_elided(idx) {
    return None
  }
  let off = read_u32(self.data, pos)
  if off.reinterpret_as_int() == 1 {
    return None
  }
  if off == 0U {
    return None
  }
  let hp = pos + off.reinterpret_as_int()
  Some(read_u32(self.data, hp))
}

pub fn MutView::get_opt_string(self : MutView, name : String) -> Array[Byte]? {
  let (_, pos, idx) = self.field_info(name)
  if self.is_field_elided(idx) {
    return None
  }
  let off = read_u32(self.data, pos)
  if off.reinterpret_as_int() == 1 {
    return None
  }
  if off == 0U {
    return Some([])
  }
  let hp = pos + off.reinterpret_as_int()
  let slen = read_u32(self.data, hp).reinterpret_as_int()
  let result : Array[Byte] = []
  for i = 0; i < slen; i = i + 1 {
    result.push(self.data[hp + 4 + i])
  }
  Some(result)
}

// ── MutView writers ───────────────────────────────────────────────────

/// Write a fixed-size scalar field.
pub fn MutView::set_bool(self : MutView, name : String, val : Bool) -> Unit {
  let (_, pos, _) = self.field_info(name)
  self.data[pos] = if val { b'\x01' } else { b'\x00' }
}

pub fn MutView::set_u8(self : MutView, name : String, val : Byte) -> Unit {
  let (_, pos, _) = self.field_info(name)
  self.data[pos] = val
}

pub fn MutView::set_u32(self : MutView, name : String, val : UInt) -> Unit {
  let (_, pos, _) = self.field_info(name)
  write_u32_at(self.data, pos, val)
}

pub fn MutView::set_i32(self : MutView, name : String, val : Int) -> Unit {
  let (_, pos, _) = self.field_info(name)
  write_u32_at(self.data, pos, val.reinterpret_as_uint())
}

pub fn MutView::set_u64(self : MutView, name : String, val : UInt64) -> Unit {
  let (_, pos, _) = self.field_info(name)
  let buf = PackBuf::new()
  buf.append_zeros(8)
  buf.write_u64_le(0, val)
  for i = 0; i < 8; i = i + 1 {
    self.data[pos + i] = buf.data[i]
  }
}

pub fn MutView::set_i64(self : MutView, name : String, val : Int64) -> Unit {
  self.set_u64(name, val.reinterpret_as_uint64())
}

pub fn MutView::set_f32(self : MutView, name : String, val : Float) -> Unit {
  self.set_u32(name, val.reinterpret_as_uint())
}

pub fn MutView::set_f64(self : MutView, name : String, val : Double) -> Unit {
  self.set_u64(name, val.reinterpret_as_uint64())
}

/// Write a variable-size field (string).
pub fn MutView::set_string(
  self : MutView,
  name : String,
  val : String
) -> Unit {
  let (f, offset_pos, idx) = self.field_info(name)
  let new_bytes = string_to_utf8(val)
  // Pack the new string value
  let packed : Array[Byte] = []
  if new_bytes.length() == 0 {
    // Empty string uses offset 0
    self.set_var_field_empty(f, offset_pos, idx)
    return
  }
  // Length prefix + data
  let pbuf = PackBuf::new()
  pbuf.append_zeros(4 + new_bytes.length())
  pbuf.write_u32_le(0, new_bytes.length().to_uint())
  for i = 0; i < new_bytes.length(); i = i + 1 {
    pbuf.data[4 + i] = new_bytes[i]
  }
  for b in pbuf.data {
    packed.push(b)
  }
  self.set_var_field(f, offset_pos, idx, packed)
}

/// Write a variable-size field (generic): handles both canonical and fast mode.
fn MutView::set_var_field(
  self : MutView,
  field : FieldSpec,
  offset_pos : Int,
  field_index : Int,
  new_bytes : Array[Byte]
) -> Unit {
  // Expand fixed region if field was elided
  if self.spec.extensible && self.is_field_elided(field_index) {
    let _ = self.expand_fixed_region(field_index)
    // Re-compute offset_pos after expansion
    let (_, new_offset_pos, _) = self.field_info(field.name)
    self.set_var_field_inner(field, new_offset_pos, field_index, new_bytes)
    return
  }
  self.set_var_field_inner(field, offset_pos, field_index, new_bytes)
}

fn MutView::set_var_field_inner(
  self : MutView,
  _field : FieldSpec,
  offset_pos : Int,
  field_index : Int,
  new_bytes : Array[Byte]
) -> Unit {
  let old_offset = read_u32(self.data, offset_pos)
  if self.fast {
    // Fast mode
    if old_offset.reinterpret_as_int() <= 1 {
      // No existing data -- append
      let append_pos = self.data.length()
      for b in new_bytes {
        self.data.push(b)
      }
      write_u32_at(self.data, offset_pos, (append_pos - offset_pos).to_uint())
      return
    }
    // Measure old data
    let old_target = offset_pos + old_offset.reinterpret_as_int()
    let old_field : FieldSpec = _field
    let old_size = packed_size_at(old_field, self.data, old_target)
    if new_bytes.length() <= old_size {
      // Fits: overwrite in place
      for i = 0; i < new_bytes.length(); i = i + 1 {
        self.data[old_target + i] = new_bytes[i]
      }
      return
    }
    // Doesn't fit: append to end, update offset
    let append_pos = self.data.length()
    for b in new_bytes {
      self.data.push(b)
    }
    write_u32_at(self.data, offset_pos, (append_pos - offset_pos).to_uint())
  } else {
    // Canonical mode
    if old_offset.reinterpret_as_int() <= 1 {
      // No existing heap data -- insert at end of buffer
      let splice_pos = self.data.length()
      let _ = splice_buffer(self.data, splice_pos, 0, new_bytes)
      write_u32_at(
        self.data,
        offset_pos,
        (splice_pos - offset_pos).to_uint(),
      )
      return
    }
    // Replace existing heap data
    let old_target = offset_pos + old_offset.reinterpret_as_int()
    let old_field : FieldSpec = _field
    let old_size = packed_size_at(old_field, self.data, old_target)
    let splice_pos = old_target
    let delta = splice_buffer(self.data, splice_pos, old_size, new_bytes)
    // Offset stays the same since we spliced at the target position.
    // But we must patch siblings.
    self.patch_siblings(field_index, splice_pos, delta)
  }
}

/// Set a variable-size field to empty (offset = 0).
fn MutView::set_var_field_empty(
  self : MutView,
  field : FieldSpec,
  offset_pos : Int,
  field_index : Int
) -> Unit {
  if self.spec.extensible && self.is_field_elided(field_index) {
    // Already elided; nothing to do for empty
    return
  }
  let old_offset = read_u32(self.data, offset_pos)
  if old_offset.reinterpret_as_int() <= 1 {
    // Already empty or None -- just set to 0
    write_u32_at(self.data, offset_pos, 0U)
    return
  }
  if self.fast {
    // Fast: just update the offset, old data becomes dead bytes
    write_u32_at(self.data, offset_pos, 0U)
  } else {
    // Canonical: remove existing heap data
    let old_target = offset_pos + old_offset.reinterpret_as_int()
    let old_size = packed_size_at(field, self.data, old_target)
    let splice_pos = old_target
    let delta = splice_buffer(self.data, splice_pos, old_size, [])
    write_u32_at(self.data, offset_pos, 0U)
    self.patch_siblings(field_index, splice_pos, delta)
  }
}

/// Set an optional field to None.
pub fn MutView::set_none(self : MutView, name : String) -> Unit {
  let (field, offset_pos, idx) = self.field_info(name)
  if self.spec.extensible && self.is_field_elided(idx) {
    // Already elided = None, nothing to do
    return
  }
  let old_offset = read_u32(self.data, offset_pos)
  if old_offset.reinterpret_as_int() <= 1 {
    // Already None or empty -- just set to 1
    write_u32_at(self.data, offset_pos, 1U)
    return
  }
  if self.fast {
    write_u32_at(self.data, offset_pos, 1U)
  } else {
    let old_target = offset_pos + old_offset.reinterpret_as_int()
    let old_size = packed_size_at(field, self.data, old_target)
    let splice_pos = old_target
    let delta = splice_buffer(self.data, splice_pos, old_size, [])
    write_u32_at(self.data, offset_pos, 1U)
    self.patch_siblings(idx, splice_pos, delta)
  }
}

/// Set an optional u32 field.
pub fn MutView::set_opt_u32(
  self : MutView,
  name : String,
  val : UInt?
) -> Unit {
  match val {
    None => self.set_none(name)
    Some(v) => {
      let (f, offset_pos, idx) = self.field_info(name)
      // Pack the u32 value
      let packed : Array[Byte] = []
      let pbuf = PackBuf::new()
      pbuf.append_zeros(4)
      pbuf.write_u32_le(0, v)
      for b in pbuf.data {
        packed.push(b)
      }
      self.set_var_field(f, offset_pos, idx, packed)
    }
  }
}

/// Set an optional string field.
pub fn MutView::set_opt_string(
  self : MutView,
  name : String,
  val : String?
) -> Unit {
  match val {
    None => self.set_none(name)
    Some(s) => {
      let (f, offset_pos, idx) = self.field_info(name)
      let bytes = string_to_utf8(s)
      if bytes.length() == 0 {
        self.set_var_field_empty(f, offset_pos, idx)
        return
      }
      let packed : Array[Byte] = []
      let pbuf = PackBuf::new()
      pbuf.append_zeros(4 + bytes.length())
      pbuf.write_u32_le(0, bytes.length().to_uint())
      for i = 0; i < bytes.length(); i = i + 1 {
        pbuf.data[4 + i] = bytes[i]
      }
      for b in pbuf.data {
        packed.push(b)
      }
      self.set_var_field(f, offset_pos, idx, packed)
    }
  }
}

/// Compact: repack from scratch (removes dead bytes from fast mode).
pub fn MutView::compact(self : MutView) -> Unit {
  // Create a FracView to read all field values, then repack
  let view = FracView::new_at(self.spec, self.data, self.base)
  // Build value from view
  let field_vals : Array[Value] = []
  for f in self.spec.fields {
    let v = read_field_value(view, f)
    field_vals.push(v)
  }
  let val = V::obj(field_vals)
  let new_data = pack(self.spec, val)
  // Replace buffer contents in place (mutating the existing array)
  let old_len = self.data.length()
  let new_len = new_data.length()
  // Grow or shrink the array
  if new_len > old_len {
    for _i = 0; _i < new_len - old_len; _i = _i + 1 {
      self.data.push(b'\x00')
    }
  } else {
    for _i = 0; _i < old_len - new_len; _i = _i + 1 {
      let _ = self.data.pop()
    }
  }
  // Copy new data
  for i = 0; i < new_len; i = i + 1 {
    self.data[i] = new_data[i]
  }
  self.base = 0
}

/// Read a field value from a FracView for compaction.
fn read_field_value(view : FracView, f : FieldSpec) -> Value {
  match f.kind {
    Bool_ => V::bool_(view.get_bool(f.name))
    U8 => V::u8_(view.get_u8(f.name))
    I8 => V::i8_(view.data[{ let (_, p) = view.field_pos(f.name); p }].to_int())
    U16 => V::u16_(read_u16(view.data, { let (_, p) = view.field_pos(f.name); p }))
    I16 => V::i16_(read_u16(view.data, { let (_, p) = view.field_pos(f.name); p }))
    U32 => V::u32_(view.get_u32(f.name))
    I32 => V::i32_(view.get_i32(f.name))
    U64 => V::u64_(view.get_u64(f.name))
    I64 => V::i64_(view.get_i64(f.name))
    F32 => V::f32_(view.get_f32(f.name))
    F64 => V::f64_(view.get_f64(f.name))
    String_ => {
      let bytes = view.get_string(f.name)
      // Convert bytes back to string value
      V::str(bytes_to_string(bytes))
    }
    Optional => {
      let (_, pos) = view.field_pos(f.name)
      let off = read_u32(view.data, pos)
      if off.reinterpret_as_int() == 1 || off == 0U {
        if off == 0U && f.elem_kind == String_ {
          V::opt(V::str(""))
        } else if off.reinterpret_as_int() == 1 {
          V::opt_none()
        } else {
          V::opt_none()
        }
      } else {
        match f.elem_kind {
          U32 => {
            let hp = pos + off.reinterpret_as_int()
            V::opt(V::u32_(read_u32(view.data, hp)))
          }
          String_ => {
            let hp = pos + off.reinterpret_as_int()
            let slen = read_u32(view.data, hp).reinterpret_as_int()
            let result : Array[Byte] = []
            for i = 0; i < slen; i = i + 1 {
              result.push(view.data[hp + 4 + i])
            }
            V::opt(V::str(bytes_to_string(result)))
          }
          _ => V::opt_none()
        }
      }
    }
    _ => V::u32_(0U) // Fallback for complex types
  }
}

/// Convert UTF-8 bytes back to a string.
fn bytes_to_string(bytes : Array[Byte]) -> String {
  // Build string from UTF-8 bytes
  let mut result = ""
  let len = bytes.length()
  let mut i = 0
  while i < len {
    let b = bytes[i].to_int()
    if b < 0x80 {
      result = result + Char::from_int(b).to_string()
      i = i + 1
    } else if b < 0xE0 {
      if i + 1 < len {
        let cp = ((b.land(0x1F)) << 6) | (bytes[i + 1].to_int().land(0x3F))
        result = result + Char::from_int(cp).to_string()
        i = i + 2
      } else {
        i = i + 1
      }
    } else if b < 0xF0 {
      if i + 2 < len {
        let cp = ((b.land(0x0F)) << 12)
          | ((bytes[i + 1].to_int().land(0x3F)) << 6)
          | (bytes[i + 2].to_int().land(0x3F))
        result = result + Char::from_int(cp).to_string()
        i = i + 3
      } else {
        i = i + 1
      }
    } else {
      if i + 3 < len {
        let cp = ((b.land(0x07)) << 18)
          | ((bytes[i + 1].to_int().land(0x3F)) << 12)
          | ((bytes[i + 2].to_int().land(0x3F)) << 6)
          | (bytes[i + 3].to_int().land(0x3F))
        // Encode as surrogate pair for the string
        result = result + Char::from_int(cp).to_string()
        i = i + 4
      } else {
        i = i + 1
      }
    }
  }
  result
}

// ── Value builders ─────────────────────────────────────────────────────
//
// Concise constructors so users write `V.u32(42U)` instead of
// spelling out all 16 fields of the Value struct.

pub fn V::bool_(b : Bool) -> Value {
  { ..Value::new(), kind: Bool_, bool_v: b }
}

pub fn V::u8_(v : Byte) -> Value {
  { ..Value::new(), kind: U8, u8_v: v }
}

pub fn V::i8_(v : Int) -> Value {
  { ..Value::new(), kind: I8, i8_v: v }
}

pub fn V::u16_(v : Int) -> Value {
  { ..Value::new(), kind: U16, u16_v: v }
}

pub fn V::i16_(v : Int) -> Value {
  { ..Value::new(), kind: I16, i16_v: v }
}

pub fn V::u32_(v : UInt) -> Value {
  { ..Value::new(), kind: U32, u32_v: v }
}

pub fn V::i32_(v : Int) -> Value {
  { ..Value::new(), kind: I32, i32_v: v }
}

pub fn V::u64_(v : UInt64) -> Value {
  { ..Value::new(), kind: U64, u64_v: v }
}

pub fn V::i64_(v : Int64) -> Value {
  { ..Value::new(), kind: I64, i64_v: v }
}

pub fn V::f32_(v : Float) -> Value {
  { ..Value::new(), kind: F32, f32_v: v }
}

pub fn V::f64_(v : Double) -> Value {
  { ..Value::new(), kind: F64, f64_v: v }
}

pub fn V::str(s : String) -> Value {
  { ..Value::new(), kind: String_, str: s }
}

pub fn V::vec(elems : Array[Value]) -> Value {
  { ..Value::new(), kind: Vec, vec: elems }
}

pub fn V::opt_none() -> Value {
  { ..Value::new(), kind: Optional }
}

pub fn V::opt(v : Value) -> Value {
  { ..Value::new(), kind: Optional, opt: Some(v) }
}

pub fn V::obj(fields : Array[Value]) -> Value {
  { ..Value::new(), kind: Object, fields }
}

pub fn V::variant(tag : Int, payload : Value) -> Value {
  { ..Value::new(), kind: Variant, tag, variant: Some(payload) }
}

pub fn V::tuple_(elems : Array[Value]) -> Value {
  { ..Value::new(), kind: Tuple, fields: elems }
}

// ── FieldSpec builders ─────────────────────────────────────────────────

fn default_field_spec() -> FieldSpec {
  { name: "", kind: U32, fixed_size: 4, is_var: false, elem_kind: U32, elem_spec: None, inner_spec: None }
}

pub fn F::bool_(name : String) -> FieldSpec {
  { ..default_field_spec(), name, kind: Bool_, fixed_size: 1 }
}

pub fn F::u8_(name : String) -> FieldSpec {
  { ..default_field_spec(), name, kind: U8, fixed_size: 1 }
}

pub fn F::i8_(name : String) -> FieldSpec {
  { ..default_field_spec(), name, kind: I8, fixed_size: 1 }
}

pub fn F::u16_(name : String) -> FieldSpec {
  { ..default_field_spec(), name, kind: U16, fixed_size: 2 }
}

pub fn F::i16_(name : String) -> FieldSpec {
  { ..default_field_spec(), name, kind: I16, fixed_size: 2 }
}

pub fn F::u32_(name : String) -> FieldSpec {
  { ..default_field_spec(), name, kind: U32, fixed_size: 4 }
}

pub fn F::i32_(name : String) -> FieldSpec {
  { ..default_field_spec(), name, kind: I32, fixed_size: 4 }
}

pub fn F::u64_(name : String) -> FieldSpec {
  { ..default_field_spec(), name, kind: U64, fixed_size: 8 }
}

pub fn F::i64_(name : String) -> FieldSpec {
  { ..default_field_spec(), name, kind: I64, fixed_size: 8 }
}

pub fn F::f32_(name : String) -> FieldSpec {
  { ..default_field_spec(), name, kind: F32, fixed_size: 4 }
}

pub fn F::f64_(name : String) -> FieldSpec {
  { ..default_field_spec(), name, kind: F64, fixed_size: 8 }
}

pub fn F::string(name : String) -> FieldSpec {
  { ..default_field_spec(), name, kind: String_, fixed_size: 4, is_var: true }
}

pub fn F::vec(name : String, elem_kind : FieldKind) -> FieldSpec {
  { ..default_field_spec(), name, kind: Vec, fixed_size: 4, is_var: true, elem_kind }
}

pub fn F::vec_obj(name : String, elem_spec : TypeSpec) -> FieldSpec {
  { ..default_field_spec(), name, kind: Vec, fixed_size: 4, is_var: true, elem_kind: Object, elem_spec: Some(elem_spec) }
}

pub fn F::opt(name : String, elem_kind : FieldKind) -> FieldSpec {
  { ..default_field_spec(), name, kind: Optional, fixed_size: 4, is_var: true, elem_kind }
}

pub fn F::opt_obj(name : String, elem_spec : TypeSpec) -> FieldSpec {
  { ..default_field_spec(), name, kind: Optional, fixed_size: 4, is_var: true, elem_kind: Object, elem_spec: Some(elem_spec) }
}

pub fn F::object(name : String, inner_spec : TypeSpec) -> FieldSpec {
  { ..default_field_spec(), name, kind: Object, fixed_size: 4, is_var: true, inner_spec: Some(inner_spec) }
}

pub fn F::variant(name : String, variant_spec : TypeSpec) -> FieldSpec {
  { ..default_field_spec(), name, kind: Variant, fixed_size: 4, is_var: true, inner_spec: Some(variant_spec) }
}

pub fn F::tuple_(name : String, tuple_spec : TypeSpec) -> FieldSpec {
  { ..default_field_spec(), name, kind: Tuple, fixed_size: 4, is_var: true, inner_spec: Some(tuple_spec) }
}

// ── TypeSpec builders ──────────────────────────────────────────────────

pub fn T::fixed(name : String, fields : Array[FieldSpec]) -> TypeSpec {
  { name, extensible: false, fields, is_variant: false, is_tuple: false, cases: [] }
}

pub fn T::object(name : String, fields : Array[FieldSpec]) -> TypeSpec {
  { name, extensible: true, fields, is_variant: false, is_tuple: false, cases: [] }
}

pub fn T::variant(name : String, cases : Array[FieldSpec]) -> TypeSpec {
  { name, extensible: false, fields: [], is_variant: true, is_tuple: false, cases }
}

pub fn T::tuple(name : String, elem_specs : Array[FieldSpec]) -> TypeSpec {
  { name, extensible: true, fields: elem_specs, is_variant: false, is_tuple: true, cases: [] }
}

// ── Packable trait ─────────────────────────────────────────────────────
//
// User types implement this to enable typed pack/view:
//
//   impl Packable for Inner with spec() { T::object("Inner", [...]) }
//   impl Packable for Inner with to_value(self) { V::obj([...]) }

pub trait Packable {
  spec() -> TypeSpec
  to_value(Self) -> Value
}

pub fn fracpack_pack[P : Packable](val : P) -> Array[Byte] {
  pack(P::spec(), P::to_value(val))
}

pub fn fracpack_view(spec : TypeSpec, data : Array[Byte]) -> FracView {
  FracView::new(spec, data)
}

// ── Validation ────────────────────────────────────────────────────────
//
// Validates packed fracpack data against a TypeSpec without constructing
// values.  Returns Valid, Extended (unknown fields present), or Invalid.
//
// Uses an error-tracking approach: the Validator records the first error
// encountered and short-circuits subsequent operations via has_error().

pub enum ValidationStatus {
  Valid
  Extended
  Invalid
} derive(Eq, Show)

pub struct ValidationResult {
  status : ValidationStatus
  message : String
} derive(Show)

/// Internal validator state.
struct Validator {
  data : Array[Byte]
  end : Int
  mut extended : Bool
  mut error : String
}

fn Validator::new(data : Array[Byte]) -> Validator {
  { data, end: data.length(), extended: false, error: "" }
}

fn Validator::has_error(self : Validator) -> Bool {
  self.error.length() > 0
}

fn Validator::fail(self : Validator, msg : String) -> Unit {
  if not(self.has_error()) {
    self.error = msg
  }
}

fn Validator::check(self : Validator, pos : Int, size : Int) -> Unit {
  if pos < 0 || pos + size > self.end {
    self.fail(
      "Read past end at " +
      pos.to_string() +
      "+" +
      size.to_string() +
      " (buffer size " +
      self.end.to_string() +
      ")",
    )
  }
}

fn Validator::read_u8_v(self : Validator, pos : Int) -> Int {
  self.data[pos].to_int()
}

fn Validator::read_u16_le_v(self : Validator, pos : Int) -> Int {
  self.data[pos].to_int() | (self.data[pos + 1].to_int() << 8)
}

fn Validator::read_u32_le_v(self : Validator, pos : Int) -> Int {
  self.data[pos].to_int()
    | (self.data[pos + 1].to_int() << 8)
    | (self.data[pos + 2].to_int() << 16)
    | (self.data[pos + 3].to_int() << 24)
}

/// Validate a field (or top-level type) at position `pos`.
/// Returns the position immediately after the consumed data.
fn Validator::validate_field(
  self : Validator,
  field : FieldSpec,
  pos : Int
) -> Int {
  if self.has_error() {
    return pos
  }
  match field.kind {
    Bool_ => {
      self.check(pos, 1)
      if self.has_error() {
        return pos
      }
      let v = self.read_u8_v(pos)
      if v > 1 {
        self.fail("Invalid bool value: " + v.to_string())
      }
      pos + 1
    }
    U8 | I8 => {
      self.check(pos, 1)
      pos + 1
    }
    U16 | I16 => {
      self.check(pos, 2)
      pos + 2
    }
    U32 | I32 | F32 => {
      self.check(pos, 4)
      pos + 4
    }
    U64 | I64 | F64 => {
      self.check(pos, 8)
      pos + 8
    }
    String_ => {
      self.check(pos, 4)
      if self.has_error() {
        return pos
      }
      let len = self.read_u32_le_v(pos)
      self.check(pos + 4, len)
      if self.has_error() {
        return pos
      }
      self.validate_utf8(pos + 4, len)
      pos + 4 + len
    }
    Vec => self.validate_vec(field, pos)
    Optional => self.validate_optional(field, pos)
    Object => {
      match field.inner_spec {
        Some(spec) => self.validate_struct(spec, pos)
        None => pos
      }
    }
    Variant => {
      match field.inner_spec {
        Some(spec) => self.validate_variant(spec, pos)
        None => pos
      }
    }
    Tuple => {
      match field.inner_spec {
        Some(spec) => self.validate_struct(spec, pos)
        None => pos
      }
    }
  }
}

/// Validate an optional at `pos` (when Optional is the top-level kind).
fn Validator::validate_optional(
  self : Validator,
  field : FieldSpec,
  pos : Int
) -> Int {
  if self.has_error() {
    return pos
  }
  self.check(pos, 4)
  if self.has_error() {
    return pos
  }
  let offset = self.read_u32_le_v(pos)
  if offset == 1 {
    return pos + 4 // None
  }
  if offset == 0 {
    return pos + 4 // empty container (Some("") etc.)
  }
  if offset == 2 || offset == 3 {
    self.fail("Reserved offset value: " + offset.to_string())
    return pos
  }
  let target = pos + offset
  self.check(target, 0)
  if self.has_error() {
    return pos
  }
  // Validate the inner value at the target
  let inner_field = make_inner_field_for_optional(field)
  self.validate_field(inner_field, target)
}

/// Validate a vec at `pos`.
fn Validator::validate_vec(
  self : Validator,
  field : FieldSpec,
  pos : Int
) -> Int {
  if self.has_error() {
    return pos
  }
  self.check(pos, 4)
  if self.has_error() {
    return pos
  }
  let data_size = self.read_u32_le_v(pos)
  let fixed_start = pos + 4
  self.check(fixed_start, data_size)
  if self.has_error() {
    return pos
  }
  // Determine element fixed size
  let elem_fs = match field.elem_spec {
    Some(_) => 4 // Object elements use offset
    None => fixed_size_of(field.elem_kind)
  }
  if elem_fs > 0 && data_size % elem_fs != 0 {
    self.fail(
      "Vec data_size " +
      data_size.to_string() +
      " not divisible by element size " +
      elem_fs.to_string(),
    )
    return pos
  }
  let count = if elem_fs > 0 { data_size / elem_fs } else { 0 }
  let elem_is_var = match field.elem_spec {
    Some(_) => true
    None => is_var_kind(field.elem_kind)
  }
  if not(elem_is_var) {
    // Fixed-size elements: validate each inline
    let elem_field = make_vec_elem_field(field)
    for i = 0; i < count; i = i + 1 {
      if self.has_error() {
        return pos
      }
      let _ = self.validate_field(elem_field, fixed_start + i * elem_fs)
    }
    return fixed_start + data_size
  }
  // Variable-size elements: follow offsets
  let mut end_pos = fixed_start + data_size
  let elem_field = make_vec_elem_field(field)
  for i = 0; i < count; i = i + 1 {
    if self.has_error() {
      return pos
    }
    let fp = fixed_start + i * elem_fs
    end_pos = self.validate_embedded(elem_field, fp, end_pos)
  }
  end_pos
}

/// Validate an extensible or non-extensible struct at `pos`.
/// Returns position after consumed data.
fn Validator::validate_struct(
  self : Validator,
  spec : TypeSpec,
  pos : Int
) -> Int {
  if self.has_error() {
    return pos
  }
  if not(spec.extensible) {
    return self.validate_fixed_struct(spec, pos)
  }
  // Extensible struct: [u16 fixed_size][fields...][heap...]
  self.check(pos, 2)
  if self.has_error() {
    return pos
  }
  let fixed_size = self.read_u16_le_v(pos)
  let fixed_start = pos + 2
  self.check(fixed_start, fixed_size)
  if self.has_error() {
    return pos
  }
  let heap_pos = fixed_start + fixed_size
  // Compute expected fixed size from spec
  let mut expected_fixed = 0
  for f in spec.fields {
    expected_fixed = expected_fixed + f.fixed_size
  }
  if fixed_size > expected_fixed {
    self.extended = true
  }
  let mut end_pos = heap_pos
  let mut field_pos = fixed_start
  for i = 0; i < spec.fields.length(); i = i + 1 {
    if self.has_error() {
      return pos
    }
    let f = spec.fields[i]
    if field_pos + f.fixed_size > heap_pos {
      // Field is elided (trailing optional)
      break
    }
    end_pos = self.validate_embedded(f, field_pos, end_pos)
    field_pos = field_pos + f.fixed_size
  }
  end_pos
}

/// Validate a non-extensible (fixed) struct at `pos`.
fn Validator::validate_fixed_struct(
  self : Validator,
  spec : TypeSpec,
  pos : Int
) -> Int {
  if self.has_error() {
    return pos
  }
  let mut total_fixed = 0
  for f in spec.fields {
    total_fixed = total_fixed + f.fixed_size
  }
  self.check(pos, total_fixed)
  if self.has_error() {
    return pos
  }
  let mut end_pos = pos + total_fixed
  let mut field_pos = pos
  for f in spec.fields {
    if self.has_error() {
      return pos
    }
    end_pos = self.validate_embedded(f, field_pos, end_pos)
    field_pos = field_pos + f.fixed_size
  }
  end_pos
}

/// Validate a variant at `pos`.
fn Validator::validate_variant(
  self : Validator,
  spec : TypeSpec,
  pos : Int
) -> Int {
  if self.has_error() {
    return pos
  }
  self.check(pos, 5)
  if self.has_error() {
    return pos
  }
  let tag = self.read_u8_v(pos)
  let data_size = self.read_u32_le_v(pos + 1)
  if tag >= spec.cases.length() {
    self.fail(
      "Variant tag " +
      tag.to_string() +
      " out of range (" +
      spec.cases.length().to_string() +
      " cases)",
    )
    return pos
  }
  self.check(pos + 5, data_size)
  if self.has_error() {
    return pos
  }
  // Validate the inner data
  let case_field = spec.cases[tag]
  let inner_field = match case_field.inner_spec {
    Some(inner) =>
      {
        ..default_field_spec(),
        name: "",
        kind: Object,
        fixed_size: 4,
        is_var: true,
        inner_spec: Some(inner),
      }
    None => {
      let fs = fixed_size_of(case_field.kind)
      let iv = is_var_kind(case_field.kind)
      { ..default_field_spec(), name: "", kind: case_field.kind, fixed_size: fs, is_var: iv }
    }
  }
  let _ = self.validate_field(inner_field, pos + 5)
  pos + 5 + data_size
}

/// Validate an embedded field at `fixed_pos`. Variable-size data starts
/// at `end_pos`. Returns the new end_pos after consuming any heap data.
fn Validator::validate_embedded(
  self : Validator,
  field : FieldSpec,
  fixed_pos : Int,
  end_pos : Int
) -> Int {
  if self.has_error() {
    return end_pos
  }
  if not(field.is_var) {
    let _ = self.validate_field(field, fixed_pos)
    return end_pos
  }
  // Variable-size: read offset
  self.check(fixed_pos, 4)
  if self.has_error() {
    return end_pos
  }
  let offset = self.read_u32_le_v(fixed_pos)
  if offset == 0 {
    return end_pos // empty container
  }
  if offset == 1 {
    if field.kind == Optional {
      return end_pos // None
    }
    // offset=1 for non-optional is technically a reserved value
    // but we treat it as empty/none for robustness
    return end_pos
  }
  if offset == 2 || offset == 3 {
    self.fail("Reserved offset value: " + offset.to_string())
    return end_pos
  }
  let target = fixed_pos + offset
  if target < end_pos {
    self.fail(
      "Offset " +
      offset.to_string() +
      " at " +
      fixed_pos.to_string() +
      " points before heap (expected >= " +
      end_pos.to_string() +
      ")",
    )
    return end_pos
  }
  // For optional fields, we need to resolve one level of indirection:
  // the offset already points to the inner value's data, not to another
  // offset.
  if field.kind == Optional {
    let inner = make_inner_field_for_optional(field)
    return self.validate_field(inner, target)
  }
  self.validate_field(field, target)
}

/// Validate UTF-8 encoding.
fn Validator::validate_utf8(
  self : Validator,
  pos : Int,
  len : Int
) -> Unit {
  let mut i = pos
  let end = pos + len
  while i < end {
    if self.has_error() {
      return
    }
    let b = self.data[i].to_int()
    if b < 0x80 {
      i = i + 1
      continue
    }
    let mut need = 0
    if b.land(0xE0) == 0xC0 {
      if b.land(0x1E) == 0 {
        // Overlong: 0xC0 or 0xC1
        self.fail("Overlong UTF-8 sequence at " + i.to_string())
        return
      }
      need = 1
    } else if b.land(0xF0) == 0xE0 {
      need = 2
    } else if b.land(0xF8) == 0xF0 {
      if b > 0xF4 {
        self.fail(
          "Invalid UTF-8 lead byte 0x" +
          byte_to_hex(b) +
          " at " +
          i.to_string(),
        )
        return
      }
      need = 3
    } else {
      self.fail(
        "Invalid UTF-8 lead byte 0x" +
        byte_to_hex(b) +
        " at " +
        i.to_string(),
      )
      return
    }
    if i + 1 + need > end {
      self.fail("Truncated UTF-8 sequence at " + i.to_string())
      return
    }
    for j = 1; j <= need; j = j + 1 {
      if self.data[i + j].to_int().land(0xC0) != 0x80 {
        self.fail(
          "Invalid UTF-8 continuation byte at " + (i + j).to_string(),
        )
        return
      }
    }
    // Check for overlong 3-byte and 4-byte sequences and surrogate halves
    if need == 2 {
      let cp = ((b.land(0x0F)) << 12)
        | ((self.data[i + 1].to_int().land(0x3F)) << 6)
        | (self.data[i + 2].to_int().land(0x3F))
      if cp < 0x800 {
        self.fail("Overlong UTF-8 sequence at " + i.to_string())
        return
      }
      if cp >= 0xD800 && cp <= 0xDFFF {
        self.fail("UTF-8 surrogate half at " + i.to_string())
        return
      }
    }
    if need == 3 {
      let cp = ((b.land(0x07)) << 18)
        | ((self.data[i + 1].to_int().land(0x3F)) << 12)
        | ((self.data[i + 2].to_int().land(0x3F)) << 6)
        | (self.data[i + 3].to_int().land(0x3F))
      if cp < 0x10000 {
        self.fail("Overlong UTF-8 sequence at " + i.to_string())
        return
      }
      if cp > 0x10FFFF {
        self.fail("UTF-8 codepoint out of range at " + i.to_string())
        return
      }
    }
    i = i + 1 + need
  }
}

fn byte_to_hex(b : Int) -> String {
  let hex_chars = "0123456789ABCDEF"
  let hi = b.lsr(4).land(0xF)
  let lo = b.land(0xF)
  hex_chars[hi].to_string() + hex_chars[lo].to_string()
}

/// Build a FieldSpec for the inner value of an Optional.
fn make_inner_field_for_optional(field : FieldSpec) -> FieldSpec {
  match field.elem_spec {
    Some(es) =>
      {
        ..default_field_spec(),
        name: "",
        kind: Object,
        fixed_size: 4,
        is_var: true,
        inner_spec: Some(es),
      }
    None =>
      match field.elem_kind {
        String_ =>
          {
            ..default_field_spec(),
            name: "",
            kind: String_,
            fixed_size: 4,
            is_var: true,
          }
        Vec =>
          // When an Optional wraps a Vec, the packer defaults to U32
          // elements (see pack_opt_payload). Match that convention.
          {
            ..default_field_spec(),
            name: "",
            kind: Vec,
            fixed_size: 4,
            is_var: true,
            elem_kind: U32,
            elem_spec: None,
          }
        Optional =>
          {
            ..default_field_spec(),
            name: "",
            kind: Optional,
            fixed_size: 4,
            is_var: true,
            elem_kind: U32,
          }
        _ => {
          let fs = fixed_size_of(field.elem_kind)
          {
            ..default_field_spec(),
            name: "",
            kind: field.elem_kind,
            fixed_size: fs,
            is_var: false,
          }
        }
      }
  }
}

/// Build a FieldSpec for the element of a Vec.
fn make_vec_elem_field(field : FieldSpec) -> FieldSpec {
  match field.elem_spec {
    Some(es) =>
      {
        ..default_field_spec(),
        name: "",
        kind: Object,
        fixed_size: 4,
        is_var: true,
        inner_spec: Some(es),
      }
    None =>
      match field.elem_kind {
        String_ =>
          {
            ..default_field_spec(),
            name: "",
            kind: String_,
            fixed_size: 4,
            is_var: true,
          }
        Optional =>
          {
            ..default_field_spec(),
            name: "",
            kind: Optional,
            fixed_size: 4,
            is_var: true,
            elem_kind: U32,
          }
        Vec =>
          {
            ..default_field_spec(),
            name: "",
            kind: Vec,
            fixed_size: 4,
            is_var: true,
            elem_kind: U32,
          }
        _ => {
          let fs = fixed_size_of(field.elem_kind)
          {
            ..default_field_spec(),
            name: "",
            kind: field.elem_kind,
            fixed_size: fs,
            is_var: false,
          }
        }
      }
  }
}

// ── Validation public API ─────────────────────────────────────────────

/// Validate packed fracpack data against a TypeSpec without unpacking.
///
/// For extensible structs (Object/Tuple), the top-level value is validated
/// as a struct. For non-extensible fixed structs, as a fixed struct.
/// For variants, as a variant.
///
/// Returns:
/// - `{ status: Valid }` if data matches schema exactly
/// - `{ status: Extended, message }` if valid but has unknown fields
/// - `{ status: Invalid, message }` if data is malformed
pub fn validate(spec : TypeSpec, data : Array[Byte]) -> ValidationResult {
  let v = Validator::new(data)
  let end_pos = if spec.is_variant {
    // Top-level variant
    v.validate_variant(spec, 0)
  } else {
    // Struct (extensible or fixed)
    v.validate_struct(spec, 0)
  }
  if v.has_error() {
    return { status: Invalid, message: v.error }
  }
  if end_pos != v.end {
    let extra = v.end - end_pos
    return {
      status: Invalid,
      message: "Extra data: " +
        extra.to_string() +
        " bytes after valid data",
    }
  }
  if v.extended {
    return { status: Extended, message: "Unknown fields present" }
  }
  { status: Valid, message: "" }
}

// ── V/F/T namespace types ──────────────────────────────────────────────

pub struct V {}

pub struct F {}

pub struct T {}
