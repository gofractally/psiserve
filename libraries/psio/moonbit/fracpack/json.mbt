// Canonical JSON ↔ fracpack Value conversion.
//
// Schema-driven conversion between fracpack Value and JSON strings.
// Matches the canonical format used by all fracpack implementations
// (JS, Rust, etc.).
//
// MoonBit's JSON ecosystem is still maturing. Standard library JSON support
// (if available) handles basic types but has no concept of hex-encoded bytes,
// u64-as-string for JS interop, or canonical variant encoding. This module
// provides schema-driven canonical JSON matching all fracpack implementations.

// ========================= Value → JSON string =========================

/// Convert a fracpack Value to canonical JSON string.
///
/// Canonical format rules:
/// - u64/i64 → JSON string (e.g., "12345678901234")
/// - u8-u32, i8-i32 → JSON number
/// - f32/f64 → JSON number; NaN/Inf → null
/// - bool → true/false
/// - string → JSON string (with proper escaping)
/// - optional None → null; Some(x) → recurse on inner
/// - variant → {"CaseName": value} (single-key object)
/// - vec/array → JSON array
/// - tuple → JSON array
/// - struct → JSON object with field names as keys
pub fn to_json(spec : TypeSpec, value : Value) -> String {
  let buf : Array[String] = []
  value_to_json(spec, value, buf)
  buf.iter().fold(init="", fn(acc, s) { acc + s })
}

/// Internal recursive JSON serializer.
fn value_to_json(spec : TypeSpec, value : Value, buf : Array[String]) -> Unit {
  // Variant type
  if spec.is_variant {
    write_variant_json(spec, value, buf)
    return
  }
  // Tuple type
  if spec.is_tuple {
    write_tuple_json(spec, value, buf)
    return
  }
  // Struct/object type (has fields)
  if spec.fields.length() > 0 {
    write_struct_json(spec, value, buf)
    return
  }
  // Should not reach here for well-formed specs with fields
  buf.push("null")
}

/// Write a single field value as JSON, driven by the FieldSpec.
fn field_value_to_json(
  field : FieldSpec,
  value : Value,
  buf : Array[String]
) -> Unit {
  match field.kind {
    Bool_ => buf.push(if value.bool_v { "true" } else { "false" })
    U8 => buf.push(value.u8_v.to_int().to_string())
    I8 => buf.push(value.i8_v.to_string())
    U16 => buf.push(value.u16_v.to_string())
    I16 => buf.push(value.i16_v.to_string())
    U32 => buf.push(value.u32_v.to_string())
    I32 => buf.push(value.i32_v.to_string())
    U64 => {
      // u64 → JSON string for JS interop
      buf.push("\"")
      buf.push(value.u64_v.to_string())
      buf.push("\"")
    }
    I64 => {
      // i64 → JSON string for JS interop
      buf.push("\"")
      buf.push(value.i64_v.to_string())
      buf.push("\"")
    }
    F32 => write_float_json(value.f32_v.to_double(), buf)
    F64 => write_float_json(value.f64_v, buf)
    String_ => write_json_string(value.str, buf)
    Vec => write_vec_json(field, value, buf)
    Optional => write_optional_json(field, value, buf)
    Object => {
      match field.inner_spec {
        Some(inner) => value_to_json(inner, value, buf)
        None => buf.push("null")
      }
    }
    Variant => {
      match field.inner_spec {
        Some(inner) => write_variant_json(inner, value, buf)
        None => buf.push("null")
      }
    }
    Tuple => {
      match field.inner_spec {
        Some(inner) => write_tuple_json(inner, value, buf)
        None => buf.push("null")
      }
    }
  }
}

/// Write a struct/object as a JSON object with field names as keys.
fn write_struct_json(
  spec : TypeSpec,
  value : Value,
  buf : Array[String]
) -> Unit {
  buf.push("{")
  for i = 0; i < spec.fields.length(); i = i + 1 {
    if i > 0 {
      buf.push(",")
    }
    let f = spec.fields[i]
    write_json_string(f.name, buf)
    buf.push(":")
    if i < value.fields.length() {
      field_value_to_json(f, value.fields[i], buf)
    } else {
      buf.push("null")
    }
  }
  buf.push("}")
}

/// Write a variant as {"CaseName": value}.
fn write_variant_json(
  spec : TypeSpec,
  value : Value,
  buf : Array[String]
) -> Unit {
  let tag = value.tag
  if tag >= 0 && tag < spec.cases.length() {
    let case_spec = spec.cases[tag]
    buf.push("{")
    write_json_string(case_spec.name, buf)
    buf.push(":")
    match value.variant {
      Some(v) => field_value_to_json(case_spec, v, buf)
      None => buf.push("null")
    }
    buf.push("}")
  } else {
    buf.push("null")
  }
}

/// Write a tuple as a JSON array.
fn write_tuple_json(
  spec : TypeSpec,
  value : Value,
  buf : Array[String]
) -> Unit {
  buf.push("[")
  for i = 0; i < spec.fields.length(); i = i + 1 {
    if i > 0 {
      buf.push(",")
    }
    if i < value.fields.length() {
      field_value_to_json(spec.fields[i], value.fields[i], buf)
    } else {
      buf.push("null")
    }
  }
  buf.push("]")
}

/// Write a vec/array as a JSON array.
fn write_vec_json(field : FieldSpec, value : Value, buf : Array[String]) -> Unit {
  buf.push("[")
  for i = 0; i < value.vec.length(); i = i + 1 {
    if i > 0 {
      buf.push(",")
    }
    // Build a FieldSpec for the element
    let elem_field : FieldSpec = {
      ..default_field_spec(),
      name: "",
      kind: field.elem_kind,
      inner_spec: field.elem_spec,
      elem_spec: field.elem_spec,
    }
    field_value_to_json(elem_field, value.vec[i], buf)
  }
  buf.push("]")
}

/// Write an optional value: None → null, Some(x) → recurse.
fn write_optional_json(
  field : FieldSpec,
  value : Value,
  buf : Array[String]
) -> Unit {
  match value.opt {
    None => buf.push("null")
    Some(inner) => {
      let inner_field : FieldSpec = {
        ..default_field_spec(),
        name: "",
        kind: field.elem_kind,
        inner_spec: field.elem_spec,
        elem_spec: field.elem_spec,
      }
      field_value_to_json(inner_field, inner, buf)
    }
  }
}

/// Write a float as JSON number, with NaN/Inf → null.
fn write_float_json(v : Double, buf : Array[String]) -> Unit {
  if v != v {
    // NaN
    buf.push("null")
  } else if v == 1.0 / 0.0 || v == -1.0 / 0.0 {
    // +/- Infinity
    buf.push("null")
  } else {
    buf.push(v.to_string())
  }
}

/// Write a JSON-escaped string (with surrounding quotes).
fn write_json_string(s : String, buf : Array[String]) -> Unit {
  buf.push("\"")
  let len = s.length()
  let mut i = 0
  while i < len {
    let c = s[i].to_int()
    if c == 0x22 {
      // "
      buf.push("\\\"")
    } else if c == 0x5C {
      // backslash
      buf.push("\\\\")
    } else if c == 0x0A {
      // newline
      buf.push("\\n")
    } else if c == 0x0D {
      // carriage return
      buf.push("\\r")
    } else if c == 0x09 {
      // tab
      buf.push("\\t")
    } else if c < 0x20 {
      // Other control characters → \uXXXX
      buf.push("\\u")
      buf.push(json_hex_digit((c >> 12).land(0xF)))
      buf.push(json_hex_digit((c >> 8).land(0xF)))
      buf.push(json_hex_digit((c >> 4).land(0xF)))
      buf.push(json_hex_digit(c.land(0xF)))
    } else {
      buf.push(Char::from_int(c).to_string())
    }
    i = i + 1
  }
  buf.push("\"")
}

/// Single hex digit character (for JSON escaping).
fn json_hex_digit(n : Int) -> String {
  let chars = "0123456789abcdef"
  Char::from_int(chars[n.land(0xF)].to_int()).to_string()
}

// ========================= Number parsing helpers =========================

/// Parse a string as a signed Int (for u8/i8/u16/i16/u32/i32).
fn json_parse_int(s : String) -> Int {
  let len = s.length()
  if len == 0 {
    return 0
  }
  let mut neg = false
  let mut i = 0
  if s[0].to_int() == 0x2D {
    // '-'
    neg = true
    i = 1
  }
  let mut val = 0
  while i < len {
    let c = s[i].to_int()
    if c >= 0x30 && c <= 0x39 {
      val = val * 10 + (c - 0x30)
    } else {
      break
    }
    i = i + 1
  }
  if neg { -val } else { val }
}

/// Parse a string as UInt64.
fn json_parse_uint64(s : String) -> UInt64 {
  let len = s.length()
  if len == 0 {
    return 0UL
  }
  let mut val = 0UL
  let mut i = 0
  while i < len {
    let c = s[i].to_int()
    if c >= 0x30 && c <= 0x39 {
      val = val * 10UL + (c - 0x30).to_uint64()
    } else {
      break
    }
    i = i + 1
  }
  val
}

/// Parse a string as Int64 (signed).
fn json_parse_int64(s : String) -> Int64 {
  let len = s.length()
  if len == 0 {
    return 0L
  }
  let mut neg = false
  let mut i = 0
  if s[0].to_int() == 0x2D {
    neg = true
    i = 1
  }
  let mut val = 0L
  while i < len {
    let c = s[i].to_int()
    if c >= 0x30 && c <= 0x39 {
      val = val * 10L + (c - 0x30).to_int64()
    } else {
      break
    }
    i = i + 1
  }
  if neg { -val } else { val }
}

/// Parse a string as Double.
/// Handles integer, decimal, and exponent notation.
fn json_parse_double(s : String) -> Double {
  let len = s.length()
  if len == 0 {
    return 0.0
  }
  let mut neg = false
  let mut i = 0
  if s[0].to_int() == 0x2D {
    neg = true
    i = 1
  }
  // Integer part
  let mut int_part = 0.0
  while i < len {
    let c = s[i].to_int()
    if c >= 0x30 && c <= 0x39 {
      int_part = int_part * 10.0 + (c - 0x30).to_double()
      i = i + 1
    } else {
      break
    }
  }
  // Fractional part
  let mut frac_part = 0.0
  if i < len && s[i].to_int() == 0x2E {
    i = i + 1
    let mut divisor = 10.0
    while i < len {
      let c = s[i].to_int()
      if c >= 0x30 && c <= 0x39 {
        frac_part = frac_part + (c - 0x30).to_double() / divisor
        divisor = divisor * 10.0
        i = i + 1
      } else {
        break
      }
    }
  }
  let mut result = int_part + frac_part
  // Exponent
  if i < len {
    let ec = s[i].to_int()
    if ec == 0x45 || ec == 0x65 {
      // E or e
      i = i + 1
      let mut exp_neg = false
      if i < len {
        let sc = s[i].to_int()
        if sc == 0x2D {
          exp_neg = true
          i = i + 1
        } else if sc == 0x2B {
          i = i + 1
        }
      }
      let mut exp = 0
      while i < len {
        let dc = s[i].to_int()
        if dc >= 0x30 && dc <= 0x39 {
          exp = exp * 10 + (dc - 0x30)
          i = i + 1
        } else {
          break
        }
      }
      let mut multiplier = 1.0
      for _j = 0; _j < exp; _j = _j + 1 {
        multiplier = multiplier * 10.0
      }
      if exp_neg {
        result = result / multiplier
      } else {
        result = result * multiplier
      }
    }
  }
  if neg { -result } else { result }
}

// ========================= JSON string → Value =========================

/// Parse canonical JSON string to a fracpack Value.
///
/// Uses the TypeSpec to guide parsing:
/// - JSON string → UInt64/Int64 for u64/i64 fields
/// - JSON string → String for string fields
/// - JSON null → optional None
/// - JSON object {"CaseName": value} → variant
/// - JSON object → struct
/// - JSON array → vec/tuple
pub fn from_json(spec : TypeSpec, json : String) -> Value!Failure {
  let parser = JsonParser::new(json)
  let result = parse_value!(spec, parser)
  result
}

/// JSON parser state.
struct JsonParser {
  src : String
  len : Int
  mut pos : Int
}

fn JsonParser::new(src : String) -> JsonParser {
  { src, len: src.length(), pos: 0 }
}

fn JsonParser::skip_ws(self : JsonParser) -> Unit {
  while self.pos < self.len {
    let c = self.src[self.pos].to_int()
    if c == 0x20 || c == 0x09 || c == 0x0A || c == 0x0D {
      self.pos = self.pos + 1
    } else {
      break
    }
  }
}

fn JsonParser::peek(self : JsonParser) -> Int {
  self.skip_ws()
  if self.pos >= self.len {
    return -1
  }
  self.src[self.pos].to_int()
}

fn JsonParser::advance(self : JsonParser) -> Unit {
  self.pos = self.pos + 1
}

fn JsonParser::expect_char(self : JsonParser, ch : Int) -> Unit!Failure {
  self.skip_ws()
  if self.pos >= self.len || self.src[self.pos].to_int() != ch {
    let expected = Char::from_int(ch).to_string()
    raise Failure(
      "JSON: expected '" + expected + "' at position " + self.pos.to_string(),
    )
  }
  self.pos = self.pos + 1
}

/// Parse a JSON string literal (including surrounding quotes).
/// Returns the unescaped string content.
fn JsonParser::parse_string(self : JsonParser) -> String!Failure {
  self.skip_ws()
  if self.pos >= self.len || self.src[self.pos].to_int() != 0x22 {
    raise Failure("JSON: expected '\"' at position " + self.pos.to_string())
  }
  self.pos = self.pos + 1 // skip opening quote
  let mut result = ""
  while self.pos < self.len {
    let c = self.src[self.pos].to_int()
    if c == 0x22 {
      // closing quote
      self.pos = self.pos + 1
      return result
    }
    if c == 0x5C {
      // backslash escape
      self.pos = self.pos + 1
      if self.pos >= self.len {
        raise Failure("JSON: unterminated escape")
      }
      let esc = self.src[self.pos].to_int()
      if esc == 0x22 {
        result = result + "\""
      } else if esc == 0x5C {
        result = result + "\\"
      } else if esc == 0x6E {
        // n
        result = result + "\n"
      } else if esc == 0x72 {
        // r
        result = result + "\r"
      } else if esc == 0x74 {
        // t
        result = result + "\t"
      } else if esc == 0x2F {
        // /
        result = result + "/"
      } else if esc == 0x62 {
        // b (backspace)
        result = result + Char::from_int(0x08).to_string()
      } else if esc == 0x66 {
        // f (formfeed)
        result = result + Char::from_int(0x0C).to_string()
      } else if esc == 0x75 {
        // \uXXXX
        self.pos = self.pos + 1
        let hex_v = json_parse_hex4!(self)
        // Check for surrogate pair
        if hex_v >= 0xD800 && hex_v <= 0xDBFF {
          // High surrogate — expect \uXXXX low surrogate
          if self.pos + 1 < self.len &&
            self.src[self.pos].to_int() == 0x5C &&
            self.src[self.pos + 1].to_int() == 0x75 {
            self.pos = self.pos + 2
            let low = json_parse_hex4!(self)
            if low >= 0xDC00 && low <= 0xDFFF {
              let cp = 0x10000 + ((hex_v - 0xD800) << 10) + (low - 0xDC00)
              result = result + Char::from_int(cp).to_string()
            } else {
              result = result + Char::from_int(0xFFFD).to_string()
            }
          } else {
            result = result + Char::from_int(0xFFFD).to_string()
          }
        } else {
          result = result + Char::from_int(hex_v).to_string()
        }
        continue
      } else {
        result = result + Char::from_int(esc).to_string()
      }
      self.pos = self.pos + 1
    } else {
      result = result + Char::from_int(self.src[self.pos].to_int()).to_string()
      self.pos = self.pos + 1
    }
  }
  raise Failure("JSON: unterminated string")
}

/// Parse 4 hex digits at current position, advance past them.
fn json_parse_hex4(parser : JsonParser) -> Int!Failure {
  let mut val = 0
  for _i = 0; _i < 4; _i = _i + 1 {
    if parser.pos >= parser.len {
      raise Failure("JSON: incomplete \\u escape")
    }
    let c = parser.src[parser.pos].to_int()
    val = val << 4
    if c >= 0x30 && c <= 0x39 {
      val = val | (c - 0x30)
    } else if c >= 0x41 && c <= 0x46 {
      val = val | (c - 0x41 + 10)
    } else if c >= 0x61 && c <= 0x66 {
      val = val | (c - 0x61 + 10)
    } else {
      raise Failure(
        "JSON: invalid hex digit at position " + parser.pos.to_string(),
      )
    }
    parser.pos = parser.pos + 1
  }
  val
}

/// Parse a JSON number (integer or float) as a string.
fn JsonParser::parse_number_str(self : JsonParser) -> String!Failure {
  self.skip_ws()
  let start = self.pos
  // Optional negative sign
  if self.pos < self.len && self.src[self.pos].to_int() == 0x2D {
    self.pos = self.pos + 1
  }
  // Integer part
  while self.pos < self.len {
    let c = self.src[self.pos].to_int()
    if c >= 0x30 && c <= 0x39 {
      self.pos = self.pos + 1
    } else {
      break
    }
  }
  // Fractional part
  if self.pos < self.len && self.src[self.pos].to_int() == 0x2E {
    self.pos = self.pos + 1
    while self.pos < self.len {
      let c = self.src[self.pos].to_int()
      if c >= 0x30 && c <= 0x39 {
        self.pos = self.pos + 1
      } else {
        break
      }
    }
  }
  // Exponent
  if self.pos < self.len {
    let c = self.src[self.pos].to_int()
    if c == 0x45 || c == 0x65 {
      // E or e
      self.pos = self.pos + 1
      if self.pos < self.len {
        let sc = self.src[self.pos].to_int()
        if sc == 0x2B || sc == 0x2D {
          self.pos = self.pos + 1
        }
      }
      while self.pos < self.len {
        let dc = self.src[self.pos].to_int()
        if dc >= 0x30 && dc <= 0x39 {
          self.pos = self.pos + 1
        } else {
          break
        }
      }
    }
  }
  if self.pos == start {
    raise Failure(
      "JSON: expected number at position " + self.pos.to_string(),
    )
  }
  let mut result = ""
  for i = start; i < self.pos; i = i + 1 {
    result = result + Char::from_int(self.src[i].to_int()).to_string()
  }
  result
}

/// Parse a value driven by a TypeSpec (struct/variant/tuple level).
fn parse_value(spec : TypeSpec, parser : JsonParser) -> Value!Failure {
  if spec.is_variant {
    return parse_variant_value!(spec, parser)
  }
  if spec.is_tuple {
    return parse_tuple_value!(spec, parser)
  }
  // Struct/object
  if spec.fields.length() > 0 {
    return parse_struct_value!(spec, parser)
  }
  raise Failure("JSON: cannot parse value for empty spec")
}

/// Parse a struct as a JSON object.
fn parse_struct_value(
  spec : TypeSpec,
  parser : JsonParser
) -> Value!Failure {
  parser.expect_char!(0x7B) // {
  // Build field values array with defaults
  let field_vals : Array[Value] = []
  let field_set : Array[Bool] = []
  for _i = 0; _i < spec.fields.length(); _i = _i + 1 {
    field_vals.push(Value::new())
    field_set.push(false)
  }
  // Initialize optional fields to None
  for i = 0; i < spec.fields.length(); i = i + 1 {
    if spec.fields[i].kind == Optional {
      field_vals[i] = V::opt_none()
      field_set[i] = true
    }
  }
  let mut first = true
  while parser.peek() != 0x7D {
    // }
    if not(first) {
      parser.expect_char!(0x2C) // ,
    }
    first = false
    let key = parser.parse_string!()
    parser.expect_char!(0x3A) // :
    // Find the field by name
    let mut found = false
    for i = 0; i < spec.fields.length(); i = i + 1 {
      if spec.fields[i].name == key {
        field_vals[i] = parse_field_value!(spec.fields[i], parser)
        field_set[i] = true
        found = true
        break
      }
    }
    if not(found) {
      // Skip unknown field value
      skip_json_value!(parser)
    }
  }
  parser.expect_char!(0x7D) // }
  V::obj(field_vals)
}

/// Parse a variant as {"CaseName": value}.
fn parse_variant_value(
  spec : TypeSpec,
  parser : JsonParser
) -> Value!Failure {
  parser.expect_char!(0x7B) // {
  let key = parser.parse_string!()
  parser.expect_char!(0x3A) // :
  // Find matching case
  for i = 0; i < spec.cases.length(); i = i + 1 {
    if spec.cases[i].name == key {
      let payload = parse_field_value!(spec.cases[i], parser)
      parser.expect_char!(0x7D) // }
      return V::variant(i, payload)
    }
  }
  raise Failure("JSON: unknown variant case: " + key)
}

/// Parse a tuple as a JSON array.
fn parse_tuple_value(
  spec : TypeSpec,
  parser : JsonParser
) -> Value!Failure {
  parser.expect_char!(0x5B) // [
  let elems : Array[Value] = []
  let mut first = true
  let mut idx = 0
  while parser.peek() != 0x5D {
    // ]
    if not(first) {
      parser.expect_char!(0x2C) // ,
    }
    first = false
    if idx < spec.fields.length() {
      elems.push(parse_field_value!(spec.fields[idx], parser))
    } else {
      skip_json_value!(parser)
    }
    idx = idx + 1
  }
  parser.expect_char!(0x5D) // ]
  V::tuple_(elems)
}

/// Parse a field value driven by a FieldSpec.
fn parse_field_value(
  field : FieldSpec,
  parser : JsonParser
) -> Value!Failure {
  let ch = parser.peek()
  match field.kind {
    Bool_ => parse_json_bool!(parser)
    U8 => {
      if ch == 0x6E {
        // null
        json_parse_null!(parser)
        return V::u8_(b'\x00')
      }
      let s = parser.parse_number_str!()
      V::u8_(json_parse_int(s).to_byte())
    }
    I8 => {
      if ch == 0x6E {
        json_parse_null!(parser)
        return V::i8_(0)
      }
      let s = parser.parse_number_str!()
      V::i8_(json_parse_int(s))
    }
    U16 => {
      if ch == 0x6E {
        json_parse_null!(parser)
        return V::u16_(0)
      }
      let s = parser.parse_number_str!()
      V::u16_(json_parse_int(s))
    }
    I16 => {
      if ch == 0x6E {
        json_parse_null!(parser)
        return V::i16_(0)
      }
      let s = parser.parse_number_str!()
      V::i16_(json_parse_int(s))
    }
    U32 => {
      if ch == 0x6E {
        json_parse_null!(parser)
        return V::u32_(0U)
      }
      let s = parser.parse_number_str!()
      V::u32_(json_parse_int(s).to_uint())
    }
    I32 => {
      if ch == 0x6E {
        json_parse_null!(parser)
        return V::i32_(0)
      }
      let s = parser.parse_number_str!()
      V::i32_(json_parse_int(s))
    }
    U64 => {
      // u64 comes as JSON string
      if ch == 0x6E {
        json_parse_null!(parser)
        return V::u64_(0UL)
      }
      if ch == 0x22 {
        let s = parser.parse_string!()
        V::u64_(json_parse_uint64(s))
      } else {
        let s = parser.parse_number_str!()
        V::u64_(json_parse_uint64(s))
      }
    }
    I64 => {
      // i64 comes as JSON string
      if ch == 0x6E {
        json_parse_null!(parser)
        return V::i64_(0L)
      }
      if ch == 0x22 {
        let s = parser.parse_string!()
        V::i64_(json_parse_int64(s))
      } else {
        let s = parser.parse_number_str!()
        V::i64_(json_parse_int64(s))
      }
    }
    F32 => {
      if ch == 0x6E {
        json_parse_null!(parser)
        // null → NaN for floats
        return V::f32_((0.0 / 0.0).to_float())
      }
      let s = parser.parse_number_str!()
      V::f32_(json_parse_double(s).to_float())
    }
    F64 => {
      if ch == 0x6E {
        json_parse_null!(parser)
        return V::f64_(0.0 / 0.0)
      }
      let s = parser.parse_number_str!()
      V::f64_(json_parse_double(s))
    }
    String_ => {
      if ch == 0x6E {
        json_parse_null!(parser)
        return V::str("")
      }
      let s = parser.parse_string!()
      V::str(s)
    }
    Vec => json_parse_vec_value!(field, parser)
    Optional => json_parse_optional_value!(field, parser)
    Object => {
      match field.inner_spec {
        Some(inner) => parse_value!(inner, parser)
        None => {
          skip_json_value!(parser)
          V::obj([])
        }
      }
    }
    Variant => {
      match field.inner_spec {
        Some(inner) => parse_variant_value!(inner, parser)
        None => {
          skip_json_value!(parser)
          V::variant(0, V::u32_(0U))
        }
      }
    }
    Tuple => {
      match field.inner_spec {
        Some(inner) => parse_tuple_value!(inner, parser)
        None => {
          skip_json_value!(parser)
          V::tuple_([])
        }
      }
    }
  }
}

/// Parse a JSON boolean.
fn parse_json_bool(parser : JsonParser) -> Value!Failure {
  parser.skip_ws()
  if parser.pos + 4 <= parser.len {
    let mut is_true = true
    let true_s = "true"
    for i = 0; i < 4; i = i + 1 {
      if parser.src[parser.pos + i] != true_s[i] {
        is_true = false
        break
      }
    }
    if is_true {
      parser.pos = parser.pos + 4
      return V::bool_(true)
    }
  }
  if parser.pos + 5 <= parser.len {
    let mut is_false = true
    let false_s = "false"
    for i = 0; i < 5; i = i + 1 {
      if parser.src[parser.pos + i] != false_s[i] {
        is_false = false
        break
      }
    }
    if is_false {
      parser.pos = parser.pos + 5
      return V::bool_(false)
    }
  }
  raise Failure(
    "JSON: expected boolean at position " + parser.pos.to_string(),
  )
}

/// Parse a JSON null literal.
fn json_parse_null(parser : JsonParser) -> Unit!Failure {
  parser.skip_ws()
  if parser.pos + 4 <= parser.len {
    let null_s = "null"
    let mut ok = true
    for i = 0; i < 4; i = i + 1 {
      if parser.src[parser.pos + i] != null_s[i] {
        ok = false
        break
      }
    }
    if ok {
      parser.pos = parser.pos + 4
      return
    }
  }
  raise Failure(
    "JSON: expected null at position " + parser.pos.to_string(),
  )
}

/// Parse a vec value as JSON array.
fn json_parse_vec_value(
  field : FieldSpec,
  parser : JsonParser
) -> Value!Failure {
  let ch = parser.peek()
  if ch == 0x6E {
    json_parse_null!(parser)
    return V::vec([])
  }
  parser.expect_char!(0x5B) // [
  let elems : Array[Value] = []
  let mut first = true
  while parser.peek() != 0x5D {
    // ]
    if not(first) {
      parser.expect_char!(0x2C) // ,
    }
    first = false
    let elem_field : FieldSpec = {
      ..default_field_spec(),
      name: "",
      kind: field.elem_kind,
      inner_spec: field.elem_spec,
      elem_spec: field.elem_spec,
    }
    elems.push(parse_field_value!(elem_field, parser))
  }
  parser.expect_char!(0x5D) // ]
  V::vec(elems)
}

/// Parse an optional value: null → None, otherwise Some(inner).
fn json_parse_optional_value(
  field : FieldSpec,
  parser : JsonParser
) -> Value!Failure {
  let ch = parser.peek()
  if ch == 0x6E {
    // null
    json_parse_null!(parser)
    return V::opt_none()
  }
  let inner_field : FieldSpec = {
    ..default_field_spec(),
    name: "",
    kind: field.elem_kind,
    inner_spec: field.elem_spec,
    elem_spec: field.elem_spec,
  }
  let inner = parse_field_value!(inner_field, parser)
  V::opt(inner)
}

/// Skip an arbitrary JSON value (for unknown fields).
fn skip_json_value(parser : JsonParser) -> Unit!Failure {
  let ch = parser.peek()
  if ch == 0x22 {
    // string
    let _ = parser.parse_string!()
  } else if ch == 0x7B {
    // object
    parser.advance()
    let mut first = true
    while parser.peek() != 0x7D {
      if not(first) {
        parser.expect_char!(0x2C)
      }
      first = false
      let _ = parser.parse_string!()
      parser.expect_char!(0x3A)
      skip_json_value!(parser)
    }
    parser.expect_char!(0x7D)
  } else if ch == 0x5B {
    // array
    parser.advance()
    let mut first = true
    while parser.peek() != 0x5D {
      if not(first) {
        parser.expect_char!(0x2C)
      }
      first = false
      skip_json_value!(parser)
    }
    parser.expect_char!(0x5D)
  } else if ch == 0x74 || ch == 0x66 {
    // true / false
    let _ = parse_json_bool!(parser)
  } else if ch == 0x6E {
    // null
    json_parse_null!(parser)
  } else {
    // number
    let _ = parser.parse_number_str!()
  }
}
