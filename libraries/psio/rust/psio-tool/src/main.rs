//! psio-tool — Universal serialization format tool.
//!
//! Validate, inspect, and convert between serialization formats
//! (fracpack, Cap'n Proto, FlatBuffers, WIT Canonical ABI) using
//! dynamic dispatch over `DynamicView` and `DynamicSchema`.

use clap::{Parser, Subcommand, ValueEnum};
use psio::dynamic_schema::{DynamicSchema, DynamicType, SchemaBuilder};
use psio::dynamic_view::{DynamicValue, DynamicView, WireFormat};
use psio::schema_export;
use psio::schema_import::{parse_capnp, parse_fbs, parse_wit};
use psio::wit::layout::compute_struct_layout;
use psio::wit::pack::WitPack;
use psio::Pack;
use std::collections::HashMap;
use std::io::{self, Read, Write};
use std::process;

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// CLI argument types
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

#[derive(Parser)]
#[command(
    name = "psio-tool",
    about = "Universal serialization format tool — validate, inspect, and convert",
    version
)]
struct Cli {
    #[command(subcommand)]
    command: Commands,
}

#[derive(Subcommand)]
enum Commands {
    /// Read binary data and print field values as JSON-like output
    Inspect {
        /// Wire format of the input data
        #[arg(value_enum)]
        format: Format,

        /// Schema name (use --example to use built-in schemas)
        schema: String,

        /// Input file path, or "-" for stdin. Omit to auto-generate example data.
        input: Option<String>,

        /// Use built-in example schemas (Point, UserProfile, SensorReading)
        #[arg(long)]
        example: bool,
    },

    /// Validate binary data against a schema
    Validate {
        /// Wire format of the input data
        #[arg(value_enum)]
        format: Format,

        /// Schema name (use --example to use built-in schemas)
        schema: String,

        /// Input file path, or "-" for stdin. Omit to auto-generate example data.
        input: Option<String>,

        /// Use built-in example schemas
        #[arg(long)]
        example: bool,
    },

    /// Convert binary data between serialization formats
    Convert {
        /// Source wire format
        #[arg(value_enum)]
        src_format: Format,

        /// Destination wire format
        #[arg(value_enum)]
        dst_format: Format,

        /// Schema name (use --example to use built-in schemas)
        schema: String,

        /// Input file path, or "-" for stdin. Omit to auto-generate example data.
        input: Option<String>,

        /// Output file path (default: stdout binary)
        #[arg(short, long)]
        output: Option<String>,

        /// Use built-in example schemas
        #[arg(long)]
        example: bool,
    },

    /// Export a schema to IDL text format
    Schema {
        /// Target IDL format for export
        #[arg(value_enum)]
        format: SchemaFormat,

        /// Schema name (use --example to use built-in schemas)
        schema: String,

        /// Use built-in example schemas
        #[arg(long)]
        example: bool,
    },

    /// Generate type definitions from a schema (cpp, rust, go, typescript, python, zig)
    Codegen {
        /// Source schema format (capnp, flatbuf)
        #[arg(value_enum)]
        format: SchemaFormat,

        /// Schema name (use --example to use built-in schemas)
        schema: String,

        /// Target language for code generation
        #[arg(long, value_enum, default_value = "cpp")]
        lang: CodegenLang,

        /// Output file path (default: stdout)
        #[arg(short, long)]
        output: Option<String>,

        /// Use built-in example schemas (Point, UserProfile, SensorReading)
        #[arg(long)]
        example: bool,
    },

    /// Print supported formats and capabilities
    Info,
}

#[derive(Clone, Copy, ValueEnum)]
enum CodegenLang {
    Cpp,
    Rust,
    Go,
    Typescript,
    Python,
    Zig,
}

#[derive(Clone, Copy, ValueEnum)]
enum Format {
    Fracpack,
    Capnp,
    Flatbuf,
    Wit,
}

#[derive(Clone, Copy, ValueEnum)]
enum SchemaFormat {
    Capnp,
    Flatbuf,
    Wit,
}

impl Format {
    fn to_wire_format(self) -> WireFormat {
        match self {
            Format::Fracpack => WireFormat::Fracpack,
            Format::Capnp => WireFormat::Capnp,
            Format::Flatbuf => WireFormat::Flatbuf,
            Format::Wit => WireFormat::Wit,
        }
    }

    fn name(self) -> &'static str {
        match self {
            Format::Fracpack => "fracpack",
            Format::Capnp => "capnp",
            Format::Flatbuf => "flatbuf",
            Format::Wit => "wit",
        }
    }
}

impl SchemaFormat {
    #[allow(dead_code)]
    fn name(self) -> &'static str {
        match self {
            SchemaFormat::Capnp => "capnp",
            SchemaFormat::Flatbuf => "flatbuf",
            SchemaFormat::Wit => "wit",
        }
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Example schemas — built-in test types
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

/// Metadata for a named schema — the DynamicSchema plus per-format info
/// needed to pack data into that format.
struct ExampleSchema {
    /// Schema for WIT format (field offsets computed from WIT layout).
    wit_schema: DynamicSchema,
    /// Schema for fracpack format (field offsets from fracpack layout).
    fracpack_schema: DynamicSchema,
    /// Ordered field info: (name, type, alignment, size) for packing.
    fields: Vec<(&'static str, DynamicType, u32, u32)>,
    /// Name of the type.
    name: &'static str,
}

/// Registry of built-in example schemas.
struct SchemaRegistry {
    schemas: HashMap<String, ExampleSchema>,
}

impl SchemaRegistry {
    fn new() -> Self {
        let mut schemas = HashMap::new();

        // ── Point { x: i32, y: i32 } ─────────────────────────────────
        schemas.insert("Point".to_lowercase(), Self::build_point());
        schemas.insert("UserProfile".to_lowercase(), Self::build_user_profile());
        schemas.insert("SensorReading".to_lowercase(), Self::build_sensor_reading());

        Self { schemas }
    }

    fn get(&self, name: &str) -> Option<&ExampleSchema> {
        self.schemas.get(&name.to_lowercase())
    }

    fn names(&self) -> Vec<&str> {
        let mut names: Vec<&str> = self.schemas.values().map(|s| s.name).collect();
        names.sort();
        names
    }

    // ── Point { x: i32, y: i32 } ─────────────────────────────────────

    fn build_point() -> ExampleSchema {
        let fields: Vec<(&str, DynamicType, u32, u32)> = vec![
            ("x", DynamicType::I32, 4, 4),
            ("y", DynamicType::I32, 4, 4),
        ];

        let wit_schema = Self::build_wit_schema(&fields);
        let fracpack_schema = Self::build_fracpack_schema(&fields);

        ExampleSchema {
            wit_schema,
            fracpack_schema,
            fields,
            name: "Point",
        }
    }

    // ── UserProfile { id: u64, age: u32, score: f64, active: bool } ──

    fn build_user_profile() -> ExampleSchema {
        let fields: Vec<(&str, DynamicType, u32, u32)> = vec![
            ("id", DynamicType::U64, 8, 8),
            ("age", DynamicType::U32, 4, 4),
            ("score", DynamicType::F64, 8, 8),
            ("active", DynamicType::Bool, 1, 1),
        ];

        let wit_schema = Self::build_wit_schema(&fields);
        let fracpack_schema = Self::build_fracpack_schema(&fields);

        ExampleSchema {
            wit_schema,
            fracpack_schema,
            fields,
            name: "UserProfile",
        }
    }

    // ── SensorReading { timestamp: u64, temperature: f32, humidity: f32, device_id: u32 } ──

    fn build_sensor_reading() -> ExampleSchema {
        let fields: Vec<(&str, DynamicType, u32, u32)> = vec![
            ("timestamp", DynamicType::U64, 8, 8),
            ("temperature", DynamicType::F32, 4, 4),
            ("humidity", DynamicType::F32, 4, 4),
            ("device_id", DynamicType::U32, 4, 4),
        ];

        let wit_schema = Self::build_wit_schema(&fields);
        let fracpack_schema = Self::build_fracpack_schema(&fields);

        ExampleSchema {
            wit_schema,
            fracpack_schema,
            fields,
            name: "SensorReading",
        }
    }

    // ── Helpers ───────────────────────────────────────────────────────────

    fn build_wit_schema(fields: &[(&str, DynamicType, u32, u32)]) -> DynamicSchema {
        let layout_pairs: Vec<(u32, u32)> = fields.iter().map(|(_, _, a, s)| (*a, *s)).collect();
        let (locs, total, _align) = compute_struct_layout(&layout_pairs);

        let data_words = (total + 7) / 8;
        let mut builder = SchemaBuilder::new();
        for (i, (name, ty, _, _)) in fields.iter().enumerate() {
            builder = builder.field_scalar(name, *ty, locs[i].offset);
        }
        builder.data_words(data_words as u16).build()
    }

    fn build_fracpack_schema(fields: &[(&str, DynamicType, u32, u32)]) -> DynamicSchema {
        // Fracpack lays out fixed-size fields sequentially without padding.
        let mut offset: u32 = 2; // 2-byte fixed_size header
        let mut builder = SchemaBuilder::new();
        for (name, ty, _, size) in fields {
            builder = builder.field_scalar(name, *ty, offset);
            offset += size;
        }
        let total = offset;
        let data_words = (total + 7) / 8;
        builder.data_words(data_words as u16).build()
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Dynamic value formatting
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

fn format_value(val: &DynamicValue<'_>) -> String {
    match val {
        DynamicValue::Void => "null".to_string(),
        DynamicValue::Bool(v) => v.to_string(),
        DynamicValue::I8(v) => v.to_string(),
        DynamicValue::I16(v) => v.to_string(),
        DynamicValue::I32(v) => v.to_string(),
        DynamicValue::I64(v) => v.to_string(),
        DynamicValue::U8(v) => v.to_string(),
        DynamicValue::U16(v) => v.to_string(),
        DynamicValue::U32(v) => v.to_string(),
        DynamicValue::U64(v) => v.to_string(),
        DynamicValue::F32(v) => format!("{:.6}", v),
        DynamicValue::F64(v) => format!("{:.6}", v),
        DynamicValue::Text(v) => format!("\"{}\"", v.replace('\\', "\\\\").replace('"', "\\\"")),
        DynamicValue::Data(v) => format!("[{} bytes]", v.len()),
        DynamicValue::Struct(view) => format_view(view),
        DynamicValue::List(list) => {
            let items: Vec<String> = (0..list.len()).map(|i| format_value(&list.get(i))).collect();
            format!("[{}]", items.join(", "))
        }
    }
}

fn format_view(view: &DynamicView<'_>) -> String {
    let mut parts = Vec::new();
    for name in view.field_names() {
        let val = view.field(name);
        parts.push(format!("  \"{}\": {}", name, format_value(&val)));
    }
    if parts.is_empty() {
        "{}".to_string()
    } else {
        format!("{{\n{}\n}}", parts.join(",\n"))
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Dynamic packing — write values from a DynamicView into a target format
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

/// Extract scalar values from a DynamicView and repack into WIT format.
fn repack_wit(
    view: &DynamicView<'_>,
    schema: &ExampleSchema,
) -> Vec<u8> {
    let layout_pairs: Vec<(u32, u32)> = schema.fields.iter().map(|(_, _, a, s)| (*a, *s)).collect();
    let (locs, total, _align) = compute_struct_layout(&layout_pairs);

    // Compute total buffer size (just the fixed struct for scalar-only types)
    let buf_size = total as usize;
    let mut buf = vec![0u8; buf_size];

    for (i, (name, ty, _, _)) in schema.fields.iter().enumerate() {
        let val = view.field(name);
        let off = locs[i].offset as usize;
        write_scalar_to_buf(&mut buf, off, &val, *ty);
    }

    buf
}

/// Extract scalar values from a DynamicView and repack into fracpack format.
fn repack_fracpack(
    view: &DynamicView<'_>,
    schema: &ExampleSchema,
) -> Vec<u8> {
    // Compute total fixed size
    let mut total: u32 = 2; // 2-byte fixed_size header
    for (_, _, _, size) in &schema.fields {
        total += size;
    }

    let mut buf = vec![0u8; total as usize];
    // Write fixed_size header (little-endian u16)
    let fixed_size = total as u16;
    buf[0..2].copy_from_slice(&fixed_size.to_le_bytes());

    let mut offset: u32 = 2;
    for (name, ty, _, size) in &schema.fields {
        let val = view.field(name);
        write_scalar_to_buf(&mut buf, offset as usize, &val, *ty);
        offset += size;
    }

    buf
}

/// Write a scalar DynamicValue into a byte buffer at the given offset.
fn write_scalar_to_buf(buf: &mut [u8], off: usize, val: &DynamicValue<'_>, ty: DynamicType) {
    match ty {
        DynamicType::Bool => {
            if let Some(v) = val.as_bool() {
                buf[off] = v as u8;
            }
        }
        DynamicType::I8 => {
            if let Some(v) = val.as_i8() {
                buf[off] = v as u8;
            }
        }
        DynamicType::U8 => {
            if let Some(v) = val.as_u8() {
                buf[off] = v;
            }
        }
        DynamicType::I16 => {
            if let Some(v) = val.as_i16() {
                buf[off..off + 2].copy_from_slice(&v.to_le_bytes());
            }
        }
        DynamicType::U16 => {
            if let Some(v) = val.as_u16() {
                buf[off..off + 2].copy_from_slice(&v.to_le_bytes());
            }
        }
        DynamicType::I32 => {
            if let Some(v) = val.as_i32() {
                buf[off..off + 4].copy_from_slice(&v.to_le_bytes());
            }
        }
        DynamicType::U32 => {
            if let Some(v) = val.as_u32() {
                buf[off..off + 4].copy_from_slice(&v.to_le_bytes());
            }
        }
        DynamicType::I64 => {
            if let Some(v) = val.as_i64() {
                buf[off..off + 8].copy_from_slice(&v.to_le_bytes());
            }
        }
        DynamicType::U64 => {
            if let Some(v) = val.as_u64() {
                buf[off..off + 8].copy_from_slice(&v.to_le_bytes());
            }
        }
        DynamicType::F32 => {
            if let Some(v) = val.as_f32() {
                buf[off..off + 4].copy_from_slice(&v.to_le_bytes());
            }
        }
        DynamicType::F64 => {
            if let Some(v) = val.as_f64() {
                buf[off..off + 8].copy_from_slice(&v.to_le_bytes());
            }
        }
        _ => {}
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Schema export
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

fn dynamic_type_to_capnp(ty: DynamicType) -> &'static str {
    match ty {
        DynamicType::Bool => "Bool",
        DynamicType::I8 => "Int8",
        DynamicType::I16 => "Int16",
        DynamicType::I32 => "Int32",
        DynamicType::I64 => "Int64",
        DynamicType::U8 => "UInt8",
        DynamicType::U16 => "UInt16",
        DynamicType::U32 => "UInt32",
        DynamicType::U64 => "UInt64",
        DynamicType::F32 => "Float32",
        DynamicType::F64 => "Float64",
        DynamicType::Text => "Text",
        DynamicType::Data => "Data",
        _ => "Void",
    }
}

fn dynamic_type_to_fbs(ty: DynamicType) -> &'static str {
    match ty {
        DynamicType::Bool => "bool",
        DynamicType::I8 => "byte",
        DynamicType::I16 => "short",
        DynamicType::I32 => "int",
        DynamicType::I64 => "long",
        DynamicType::U8 => "ubyte",
        DynamicType::U16 => "ushort",
        DynamicType::U32 => "uint",
        DynamicType::U64 => "ulong",
        DynamicType::F32 => "float",
        DynamicType::F64 => "double",
        DynamicType::Text => "string",
        DynamicType::Data => "[ubyte]",
        _ => "void",
    }
}

fn dynamic_type_to_wit(ty: DynamicType) -> &'static str {
    match ty {
        DynamicType::Bool => "bool",
        DynamicType::I8 => "s8",
        DynamicType::I16 => "s16",
        DynamicType::I32 => "s32",
        DynamicType::I64 => "s64",
        DynamicType::U8 => "u8",
        DynamicType::U16 => "u16",
        DynamicType::U32 => "u32",
        DynamicType::U64 => "u64",
        DynamicType::F32 => "f32",
        DynamicType::F64 => "f64",
        DynamicType::Text => "string",
        DynamicType::Data => "list<u8>",
        _ => "()",
    }
}

fn export_capnp_schema(schema: &ExampleSchema) -> String {
    let mut out = String::new();
    out.push_str(&format!("struct {} {{\n", schema.name));
    for (i, (name, ty, _, _)) in schema.fields.iter().enumerate() {
        out.push_str(&format!(
            "  {} @{} :{};\n",
            name,
            i,
            dynamic_type_to_capnp(*ty)
        ));
    }
    out.push_str("}\n");
    out
}

fn export_fbs_schema(schema: &ExampleSchema) -> String {
    let mut out = String::new();
    out.push_str(&format!("table {} {{\n", schema.name));
    for (name, ty, _, _) in &schema.fields {
        out.push_str(&format!("  {}:{};\n", name, dynamic_type_to_fbs(*ty)));
    }
    out.push_str("}\n");
    out
}

fn export_wit_schema(schema: &ExampleSchema) -> String {
    let mut out = String::new();
    out.push_str(&format!("record {} {{\n", to_kebab_case(schema.name)));
    for (name, ty, _, _) in &schema.fields {
        out.push_str(&format!(
            "  {}: {},\n",
            to_kebab_case(name),
            dynamic_type_to_wit(*ty)
        ));
    }
    out.push_str("}\n");
    out
}

fn to_kebab_case(s: &str) -> String {
    let mut out = String::new();
    for (i, ch) in s.chars().enumerate() {
        if ch.is_uppercase() && i > 0 {
            out.push('-');
        }
        out.push(ch.to_lowercase().next().unwrap());
    }
    // Also convert underscores to hyphens
    out.replace('_', "-")
}

fn to_snake_case(s: &str) -> String {
    let mut out = String::new();
    for (i, ch) in s.chars().enumerate() {
        if ch.is_uppercase() && i > 0 {
            out.push('_');
        }
        out.push(ch.to_lowercase().next().unwrap());
    }
    out
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Code generation — C++ and Rust output from DynamicSchema
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

fn dynamic_type_to_cpp(ty: DynamicType) -> &'static str {
    match ty {
        DynamicType::Void => "void",
        DynamicType::Bool => "bool",
        DynamicType::I8 => "int8_t",
        DynamicType::I16 => "int16_t",
        DynamicType::I32 => "int32_t",
        DynamicType::I64 => "int64_t",
        DynamicType::U8 => "uint8_t",
        DynamicType::U16 => "uint16_t",
        DynamicType::U32 => "uint32_t",
        DynamicType::U64 => "uint64_t",
        DynamicType::F32 => "float",
        DynamicType::F64 => "double",
        DynamicType::Text => "std::string",
        DynamicType::Data => "std::vector<uint8_t>",
        _ => "/* unknown */",
    }
}

fn dynamic_type_to_rust(ty: DynamicType) -> &'static str {
    match ty {
        DynamicType::Void => "()",
        DynamicType::Bool => "bool",
        DynamicType::I8 => "i8",
        DynamicType::I16 => "i16",
        DynamicType::I32 => "i32",
        DynamicType::I64 => "i64",
        DynamicType::U8 => "u8",
        DynamicType::U16 => "u16",
        DynamicType::U32 => "u32",
        DynamicType::U64 => "u64",
        DynamicType::F32 => "f32",
        DynamicType::F64 => "f64",
        DynamicType::Text => "String",
        DynamicType::Data => "Vec<u8>",
        _ => "/* unknown */",
    }
}

fn is_scalar_type(ty: DynamicType) -> bool {
    matches!(
        ty,
        DynamicType::Bool
            | DynamicType::I8
            | DynamicType::I16
            | DynamicType::I32
            | DynamicType::I64
            | DynamicType::U8
            | DynamicType::U16
            | DynamicType::U32
            | DynamicType::U64
            | DynamicType::F32
            | DynamicType::F64
    )
}

/// Generate C++ code from an ExampleSchema (schema-driven codegen).
fn codegen_cpp(schema: &ExampleSchema, source_label: &str) -> String {
    let mut out = String::new();
    out.push_str(&format!("// Generated by psio-tool from {}\n", source_label));
    out.push_str("#pragma once\n");
    out.push_str("#include <cstdint>\n");
    out.push_str("#include <string>\n");
    out.push_str("#include <vector>\n");
    out.push_str("#include <optional>\n");
    out.push_str("#include <psio/reflect.hpp>\n");
    out.push('\n');

    out.push_str(&format!("struct {} {{\n", schema.name));
    for (name, ty, _, _) in &schema.fields {
        let cpp_type = dynamic_type_to_cpp(*ty);
        if is_scalar_type(*ty) {
            out.push_str(&format!("    {} {}{{}};\n", cpp_type, name));
        } else {
            out.push_str(&format!("    {} {};\n", cpp_type, name));
        }
    }
    out.push_str("};\n");
    out.push_str(&format!("PSIO_REFLECT({}", schema.name));
    for (name, _, _, _) in &schema.fields {
        out.push_str(&format!(", {}", name));
    }
    out.push_str(")\n");

    out
}

/// Generate Rust code from an ExampleSchema (schema-driven codegen).
fn codegen_rust(schema: &ExampleSchema, source_label: &str) -> String {
    let mut out = String::new();
    out.push_str(&format!("// Generated by psio-tool from {}\n", source_label));
    out.push_str("use psio::{Pack, Unpack};\n");
    out.push('\n');

    out.push_str("#[derive(Pack, Unpack, Clone, Debug, PartialEq)]\n");
    out.push_str("#[fracpack(fracpack_mod = \"psio\")]\n");
    out.push_str(&format!("pub struct {} {{\n", schema.name));
    for (name, ty, _, _) in &schema.fields {
        let rust_type = dynamic_type_to_rust(*ty);
        let rust_name = to_snake_case(name);
        out.push_str(&format!("    pub {}: {},\n", rust_name, rust_type));
    }
    out.push_str("}\n");

    out
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Code generation — Go, TypeScript, Python, Zig
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

fn dynamic_type_to_go(ty: DynamicType) -> &'static str {
    match ty {
        DynamicType::Void => "struct{}",
        DynamicType::Bool => "bool",
        DynamicType::I8 => "int8",
        DynamicType::I16 => "int16",
        DynamicType::I32 => "int32",
        DynamicType::I64 => "int64",
        DynamicType::U8 => "uint8",
        DynamicType::U16 => "uint16",
        DynamicType::U32 => "uint32",
        DynamicType::U64 => "uint64",
        DynamicType::F32 => "float32",
        DynamicType::F64 => "float64",
        DynamicType::Text => "string",
        DynamicType::Data => "[]byte",
        _ => "interface{}",
    }
}

fn dynamic_type_to_ts(ty: DynamicType) -> &'static str {
    match ty {
        DynamicType::Void => "void",
        DynamicType::Bool => "boolean",
        DynamicType::I8 | DynamicType::I16 | DynamicType::I32 => "number",
        DynamicType::U8 | DynamicType::U16 | DynamicType::U32 => "number",
        DynamicType::F32 | DynamicType::F64 => "number",
        DynamicType::I64 => "number", // BigInt comment added at call site
        DynamicType::U64 => "number", // BigInt comment added at call site
        DynamicType::Text => "string",
        DynamicType::Data => "Uint8Array",
        _ => "unknown",
    }
}

fn dynamic_type_ts_comment(ty: DynamicType) -> &'static str {
    match ty {
        DynamicType::I64 => "  // int64 - use BigInt for values > 2^53",
        DynamicType::U64 => "  // uint64 - use BigInt for values > 2^53",
        _ => "",
    }
}

fn dynamic_type_to_python(ty: DynamicType) -> &'static str {
    match ty {
        DynamicType::Void => "None",
        DynamicType::Bool => "bool",
        DynamicType::I8 | DynamicType::I16 | DynamicType::I32 | DynamicType::I64 => "int",
        DynamicType::U8 | DynamicType::U16 | DynamicType::U32 | DynamicType::U64 => "int",
        DynamicType::F32 | DynamicType::F64 => "float",
        DynamicType::Text => "str",
        DynamicType::Data => "bytes",
        _ => "object",
    }
}

fn python_default(ty: DynamicType) -> &'static str {
    match ty {
        DynamicType::Bool => "False",
        DynamicType::I8 | DynamicType::I16 | DynamicType::I32 | DynamicType::I64 => "0",
        DynamicType::U8 | DynamicType::U16 | DynamicType::U32 | DynamicType::U64 => "0",
        DynamicType::F32 | DynamicType::F64 => "0.0",
        DynamicType::Text => "\"\"",
        DynamicType::Data => "b\"\"",
        _ => "None",
    }
}

fn dynamic_type_to_zig(ty: DynamicType) -> &'static str {
    match ty {
        DynamicType::Void => "void",
        DynamicType::Bool => "bool",
        DynamicType::I8 => "i8",
        DynamicType::I16 => "i16",
        DynamicType::I32 => "i32",
        DynamicType::I64 => "i64",
        DynamicType::U8 => "u8",
        DynamicType::U16 => "u16",
        DynamicType::U32 => "u32",
        DynamicType::U64 => "u64",
        DynamicType::F32 => "f32",
        DynamicType::F64 => "f64",
        DynamicType::Text => "[]const u8",
        DynamicType::Data => "[]const u8",
        _ => "void",
    }
}

fn zig_default(ty: DynamicType) -> &'static str {
    match ty {
        DynamicType::Bool => "false",
        DynamicType::I8 | DynamicType::I16 | DynamicType::I32 | DynamicType::I64 => "0",
        DynamicType::U8 | DynamicType::U16 | DynamicType::U32 | DynamicType::U64 => "0",
        DynamicType::F32 | DynamicType::F64 => "0.0",
        DynamicType::Text | DynamicType::Data => "\"\"",
        _ => "undefined",
    }
}

/// PascalCase helper: capitalize first letter of an identifier.
fn to_pascal_case(s: &str) -> String {
    let mut chars = s.chars();
    match chars.next() {
        None => String::new(),
        Some(c) => c.to_uppercase().collect::<String>() + chars.as_str(),
    }
}

/// Generate Go code from an ExampleSchema.
fn codegen_go(schema: &ExampleSchema, source_label: &str) -> String {
    let mut out = String::new();
    out.push_str(&format!("// Generated by psio-tool from {}\n", source_label));
    out.push_str("package schemas\n\n");

    out.push_str(&format!("type {} struct {{\n", to_pascal_case(schema.name)));
    for (name, ty, _, _) in &schema.fields {
        let go_t = dynamic_type_to_go(*ty);
        let fname = to_pascal_case(name);
        let pad = if fname.len() < 12 { 12 - fname.len() } else { 1 };
        out.push_str(&format!(
            "\t{}{}{} `json:\"{}\"`\n",
            fname,
            " ".repeat(pad),
            go_t,
            name
        ));
    }
    out.push_str("}\n");

    out
}

/// Generate TypeScript code from an ExampleSchema.
fn codegen_typescript(schema: &ExampleSchema, source_label: &str) -> String {
    let mut out = String::new();
    out.push_str(&format!("// Generated by psio-tool from {}\n\n", source_label));

    out.push_str(&format!("export interface {} {{\n", schema.name));
    for (name, ty, _, _) in &schema.fields {
        let ts_t = dynamic_type_to_ts(*ty);
        let comment = dynamic_type_ts_comment(*ty);
        let fname = {
            let mut s = name.to_string();
            if let Some(c) = s.get_mut(0..1) {
                c.make_ascii_lowercase();
            }
            s
        };
        out.push_str(&format!("    {}: {};{}\n", fname, ts_t, comment));
    }
    out.push_str("}\n");

    out
}

/// Generate Python code from an ExampleSchema.
fn codegen_python(schema: &ExampleSchema, source_label: &str) -> String {
    let mut out = String::new();
    out.push_str(&format!("# Generated by psio-tool from {}\n", source_label));
    out.push_str("from dataclasses import dataclass, field\n");
    out.push_str("from typing import Optional\n\n\n");

    out.push_str("@dataclass\n");
    out.push_str(&format!("class {}:\n", schema.name));
    for (name, ty, _, _) in &schema.fields {
        let py_t = dynamic_type_to_python(*ty);
        let fname = to_snake_case(name);
        let defval = python_default(*ty);
        out.push_str(&format!("    {}: {} = {}\n", fname, py_t, defval));
    }
    out.push('\n');

    out
}

/// Generate Zig code from an ExampleSchema.
fn codegen_zig(schema: &ExampleSchema, source_label: &str) -> String {
    let mut out = String::new();
    out.push_str(&format!("// Generated by psio-tool from {}\n", source_label));
    out.push_str("const std = @import(\"std\");\n\n");

    out.push_str(&format!("pub const {} = struct {{\n", schema.name));
    for (name, ty, _, _) in &schema.fields {
        let zig_t = dynamic_type_to_zig(*ty);
        let fname = to_snake_case(name);
        let defval = zig_default(*ty);
        out.push_str(&format!("    {}: {} = {},\n", fname, zig_t, defval));
    }
    out.push_str("};\n");

    out
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Example data generation
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

/// Generate example data for a schema in the given format.
fn generate_example_data(schema: &ExampleSchema, format: Format) -> Vec<u8> {
    match schema.name {
        "Point" => generate_point_data(format),
        "UserProfile" => generate_user_profile_data(format),
        "SensorReading" => generate_sensor_reading_data(format),
        _ => Vec::new(),
    }
}

fn generate_point_data(format: Format) -> Vec<u8> {
    match format {
        Format::Wit => (42i32, -17i32).wit_pack(),
        Format::Fracpack => (42i32, -17i32).packed(),
        _ => {
            eprintln!("Error: {} format not yet supported for example data generation", format.name());
            process::exit(1);
        }
    }
}

fn generate_user_profile_data(format: Format) -> Vec<u8> {
    // UserProfile: id: u64, age: u32, score: f64, active: bool
    // For WIT, we need to compute layout and write manually
    match format {
        Format::Wit => {
            let fields_layout: Vec<(u32, u32)> = vec![(8, 8), (4, 4), (8, 8), (1, 1)];
            let (locs, total, _align) = compute_struct_layout(&fields_layout);
            let mut buf = vec![0u8; total as usize];

            // id = 12345u64
            buf[locs[0].offset as usize..locs[0].offset as usize + 8]
                .copy_from_slice(&12345u64.to_le_bytes());
            // age = 30u32
            buf[locs[1].offset as usize..locs[1].offset as usize + 4]
                .copy_from_slice(&30u32.to_le_bytes());
            // score = 99.5f64
            buf[locs[2].offset as usize..locs[2].offset as usize + 8]
                .copy_from_slice(&99.5f64.to_le_bytes());
            // active = true
            buf[locs[3].offset as usize] = 1;

            buf
        }
        Format::Fracpack => {
            // Fixed layout: [u16 fixed_size] [u64 id] [u32 age] [f64 score] [bool active]
            let total: usize = 2 + 8 + 4 + 8 + 1;
            let mut buf = vec![0u8; total];
            buf[0..2].copy_from_slice(&(total as u16).to_le_bytes());
            buf[2..10].copy_from_slice(&12345u64.to_le_bytes());
            buf[10..14].copy_from_slice(&30u32.to_le_bytes());
            buf[14..22].copy_from_slice(&99.5f64.to_le_bytes());
            buf[22] = 1;
            buf
        }
        _ => {
            eprintln!("Error: {} format not yet supported for example data generation", format.name());
            process::exit(1);
        }
    }
}

fn generate_sensor_reading_data(format: Format) -> Vec<u8> {
    // SensorReading: timestamp: u64, temperature: f32, humidity: f32, device_id: u32
    match format {
        Format::Wit => {
            let fields_layout: Vec<(u32, u32)> = vec![(8, 8), (4, 4), (4, 4), (4, 4)];
            let (locs, total, _align) = compute_struct_layout(&fields_layout);
            let mut buf = vec![0u8; total as usize];

            buf[locs[0].offset as usize..locs[0].offset as usize + 8]
                .copy_from_slice(&1713200000u64.to_le_bytes());
            buf[locs[1].offset as usize..locs[1].offset as usize + 4]
                .copy_from_slice(&23.5f32.to_le_bytes());
            buf[locs[2].offset as usize..locs[2].offset as usize + 4]
                .copy_from_slice(&65.2f32.to_le_bytes());
            buf[locs[3].offset as usize..locs[3].offset as usize + 4]
                .copy_from_slice(&42u32.to_le_bytes());

            buf
        }
        Format::Fracpack => {
            let total: usize = 2 + 8 + 4 + 4 + 4;
            let mut buf = vec![0u8; total];
            buf[0..2].copy_from_slice(&(total as u16).to_le_bytes());
            buf[2..10].copy_from_slice(&1713200000u64.to_le_bytes());
            buf[10..14].copy_from_slice(&23.5f32.to_le_bytes());
            buf[14..18].copy_from_slice(&65.2f32.to_le_bytes());
            buf[18..22].copy_from_slice(&42u32.to_le_bytes());
            buf
        }
        _ => {
            eprintln!("Error: {} format not yet supported for example data generation", format.name());
            process::exit(1);
        }
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// File-based schema loading
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

/// Metadata for a file-based schema: contains the DynamicSchema, type name,
/// and field info (name, DynamicType, alignment, size) in declaration order.
struct FileSchema {
    name: String,
    schema: DynamicSchema,
    fields: Vec<(String, DynamicType, u32, u32)>,
}

// FileSchema methods intentionally minimal — the fields are accessed directly.

/// Infer alignment and size from a DynamicType.
fn dynamic_type_layout(ty: DynamicType) -> (u32, u32) {
    match ty {
        DynamicType::Bool | DynamicType::U8 | DynamicType::I8 => (1, 1),
        DynamicType::U16 | DynamicType::I16 => (2, 2),
        DynamicType::U32 | DynamicType::I32 | DynamicType::F32 => (4, 4),
        DynamicType::U64 | DynamicType::I64 | DynamicType::F64 => (8, 8),
        DynamicType::Text | DynamicType::Data | DynamicType::Vector => (4, 8),
        _ => (1, 0),
    }
}

/// Load a schema from a file path, parsing it according to `schema_format`.
/// `type_name` is the name of the struct/record/table to extract.
fn load_schema_from_file(
    schema_path: &str,
    schema_format: SchemaFormat,
    type_name: &str,
) -> FileSchema {
    let text = std::fs::read_to_string(schema_path).unwrap_or_else(|e| {
        eprintln!("Error reading schema file '{}': {}", schema_path, e);
        process::exit(1);
    });

    let dyn_schema = match schema_format {
        SchemaFormat::Capnp => {
            let parsed = parse_capnp(&text).unwrap_or_else(|e| {
                eprintln!("Error parsing capnp schema: {}", e);
                process::exit(1);
            });
            parsed.to_dynamic_schema(type_name).unwrap_or_else(|| {
                let available: Vec<&str> = parsed.structs.iter().map(|s| s.name.as_str()).collect();
                eprintln!(
                    "Error: struct '{}' not found in capnp schema. Available: {}",
                    type_name,
                    available.join(", ")
                );
                process::exit(1);
            })
        }
        SchemaFormat::Flatbuf => {
            let parsed = parse_fbs(&text).unwrap_or_else(|e| {
                eprintln!("Error parsing fbs schema: {}", e);
                process::exit(1);
            });
            parsed.to_dynamic_schema(type_name).unwrap_or_else(|| {
                let available: Vec<&str> = parsed.types.iter().map(|t| t.name.as_str()).collect();
                eprintln!(
                    "Error: type '{}' not found in fbs schema. Available: {}",
                    type_name,
                    available.join(", ")
                );
                process::exit(1);
            })
        }
        SchemaFormat::Wit => {
            let parsed = parse_wit(&text).unwrap_or_else(|e| {
                eprintln!("Error parsing wit schema: {}", e);
                process::exit(1);
            });
            parsed.to_dynamic_schema(type_name).unwrap_or_else(|| {
                let available: Vec<&str> =
                    parsed.records.iter().map(|r| r.name.as_str()).collect();
                eprintln!(
                    "Error: record '{}' not found in wit schema. Available: {}",
                    type_name,
                    available.join(", ")
                );
                process::exit(1);
            })
        }
    };

    // Extract field info from the DynamicSchema in declaration order
    let fields: Vec<(String, DynamicType, u32, u32)> = dyn_schema
        .fields_ordered()
        .map(|f| {
            let (align, size) = dynamic_type_layout(f.ty);
            (f.name.clone(), f.ty, align, size)
        })
        .collect();

    FileSchema {
        name: type_name.to_string(),
        schema: dyn_schema,
        fields,
    }
}

/// Generate codegen from a FileSchema (using DynamicSchema fields).
fn codegen_from_dynamic(
    file_schema: &FileSchema,
    lang: CodegenLang,
    source_label: &str,
) -> String {
    // Build a temporary ExampleSchema-like fields vec with 'static lifetime
    // by leaking strings (these are small one-shot allocations in a CLI tool)
    let static_fields: Vec<(&'static str, DynamicType, u32, u32)> = file_schema
        .fields
        .iter()
        .map(|(name, ty, align, size)| {
            let leaked: &'static str = Box::leak(name.clone().into_boxed_str());
            (leaked, *ty, *align, *size)
        })
        .collect();

    let leaked_name: &'static str = Box::leak(file_schema.name.clone().into_boxed_str());

    let tmp_schema = ExampleSchema {
        wit_schema: file_schema.schema.clone(),
        fracpack_schema: file_schema.schema.clone(),
        fields: static_fields,
        name: leaked_name,
    };

    match lang {
        CodegenLang::Cpp => codegen_cpp(&tmp_schema, source_label),
        CodegenLang::Rust => codegen_rust(&tmp_schema, source_label),
        CodegenLang::Go => codegen_go(&tmp_schema, source_label),
        CodegenLang::Typescript => codegen_typescript(&tmp_schema, source_label),
        CodegenLang::Python => codegen_python(&tmp_schema, source_label),
        CodegenLang::Zig => codegen_zig(&tmp_schema, source_label),
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Input reading
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

fn read_input(path: &str) -> Vec<u8> {
    if path == "-" {
        let mut buf = Vec::new();
        io::stdin()
            .read_to_end(&mut buf)
            .unwrap_or_else(|e| {
                eprintln!("Error reading stdin: {}", e);
                process::exit(1);
            });
        buf
    } else {
        std::fs::read(path).unwrap_or_else(|e| {
            eprintln!("Error reading '{}': {}", path, e);
            process::exit(1);
        })
    }
}

fn get_schema_for_format<'a>(schema: &'a ExampleSchema, format: Format) -> &'a DynamicSchema {
    match format {
        Format::Wit => &schema.wit_schema,
        Format::Fracpack => &schema.fracpack_schema,
        _ => {
            eprintln!("Error: {} format dynamic views not yet supported (capnp/flatbuf use todo!())", format.name());
            process::exit(1);
        }
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Schema argument parsing
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

/// Parse a schema argument in the form "path/to/file.capnp:TypeName".
/// If no colon separator is found, the file path is the whole string
/// and the type name defaults to the capitalized file stem.
fn parse_schema_arg(arg: &str) -> (String, String) {
    if let Some(pos) = arg.rfind(':') {
        let file_path = arg[..pos].to_string();
        let type_name = arg[pos + 1..].to_string();
        if file_path.is_empty() || type_name.is_empty() {
            eprintln!(
                "Error: invalid schema argument '{}'. Expected 'path/to/file:TypeName'.",
                arg
            );
            process::exit(1);
        }
        (file_path, type_name)
    } else {
        // No colon — use file stem as type name (PascalCase)
        let path = std::path::Path::new(arg);
        let stem = path
            .file_stem()
            .and_then(|s| s.to_str())
            .unwrap_or("Unknown");
        let type_name = to_pascal_case(stem);
        (arg.to_string(), type_name)
    }
}

/// Infer a SchemaFormat from a file extension.
fn infer_schema_format(path: &str) -> Option<SchemaFormat> {
    if path.ends_with(".capnp") {
        Some(SchemaFormat::Capnp)
    } else if path.ends_with(".fbs") {
        Some(SchemaFormat::Flatbuf)
    } else if path.ends_with(".wit") {
        Some(SchemaFormat::Wit)
    } else {
        None
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Validation
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

/// Validate data against a schema. Returns a list of error messages (empty = valid).
fn validate_data(
    data: &[u8],
    schema: &ExampleSchema,
    dyn_schema: &DynamicSchema,
    format: Format,
) -> Vec<String> {
    let mut errors = Vec::new();

    // Check minimum size
    let expected_min = match format {
        Format::Fracpack => {
            let mut size: u32 = 2;
            for (_, _, _, s) in &schema.fields {
                size += s;
            }
            size as usize
        }
        Format::Wit => {
            let layout_pairs: Vec<(u32, u32)> =
                schema.fields.iter().map(|(_, _, a, s)| (*a, *s)).collect();
            let (_, total, _) = compute_struct_layout(&layout_pairs);
            total as usize
        }
        _ => 0,
    };

    if data.len() < expected_min {
        errors.push(format!(
            "Data too short: got {} bytes, expected at least {}",
            data.len(),
            expected_min
        ));
        return errors;
    }

    // For fracpack, validate the fixed-size header
    if matches!(format, Format::Fracpack) && data.len() >= 2 {
        let fixed_size = u16::from_le_bytes([data[0], data[1]]) as usize;
        if fixed_size != expected_min {
            errors.push(format!(
                "Fracpack fixed_size header mismatch: got {}, expected {}",
                fixed_size, expected_min
            ));
        }
    }

    // Try to read each field and check for reasonable values
    let view = DynamicView::new(data, dyn_schema, format.to_wire_format());
    for (name, ty, _, _) in &schema.fields {
        let val = view.field(name);
        if matches!(val, DynamicValue::Void) && *ty != DynamicType::Void {
            errors.push(format!("Field '{}' is void (expected {:?})", name, ty));
        }
    }

    errors
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// Main
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

fn main() {
    let cli = Cli::parse();
    let registry = SchemaRegistry::new();

    match cli.command {
        Commands::Info => {
            println!("psio-tool — Universal Serialization Format Tool");
            println!();
            println!("Supported wire formats:");
            println!("  fracpack  — Psibase binary format (variable-length, forward/backward compat)");
            println!("  wit       — WIT Canonical ABI (WebAssembly Component Model)");
            println!("  capnp     — Cap'n Proto (planned)");
            println!("  flatbuf   — FlatBuffers (planned)");
            println!();
            println!("Dynamic dispatch capabilities:");
            println!("  - Runtime field access by name via DynamicView");
            println!("  - Schema-driven introspection via DynamicSchema");
            println!("  - Format-agnostic value extraction");
            println!("  - Any-format-to-any-format conversion (fracpack, wit currently active)");
            println!();
            println!("Built-in example schemas (use --example flag):");
            for name in registry.names() {
                let schema = registry.get(name).unwrap();
                let field_list: Vec<String> = schema
                    .fields
                    .iter()
                    .map(|(n, t, _, _)| format!("{}: {:?}", n, t))
                    .collect();
                println!("  {} {{ {} }}", schema.name, field_list.join(", "));
            }
            println!();
            println!("Schema export formats:");
            println!("  capnp     — Cap'n Proto IDL (.capnp)");
            println!("  flatbuf   — FlatBuffers IDL (.fbs)");
            println!("  wit       — WebAssembly Interface Types (.wit)");
        }

        Commands::Inspect {
            format,
            schema,
            input,
            example,
        } => {
            if example {
                let ex_schema = registry.get(&schema).unwrap_or_else(|| {
                    eprintln!(
                        "Error: unknown example schema '{}'. Available: {}",
                        schema,
                        registry.names().join(", ")
                    );
                    process::exit(1);
                });

                let data = match input {
                    Some(path) => read_input(&path),
                    None => generate_example_data(ex_schema, format),
                };

                let dyn_schema = get_schema_for_format(ex_schema, format);
                let view = DynamicView::new(&data, dyn_schema, format.to_wire_format());

                println!("Format: {}", format.name());
                println!("Schema: {}", ex_schema.name);
                println!("Size:   {} bytes", data.len());
                println!();
                println!("{}", format_view(&view));
            } else {
                // File-based schema: infer format from file extension or use wire format
                let (file_path, type_name) = parse_schema_arg(&schema);
                let schema_fmt = infer_schema_format(&file_path).unwrap_or_else(|| {
                    // Fall back to the wire format's corresponding schema format
                    match format {
                        Format::Capnp => SchemaFormat::Capnp,
                        Format::Flatbuf => SchemaFormat::Flatbuf,
                        Format::Wit => SchemaFormat::Wit,
                        Format::Fracpack => {
                            eprintln!("Error: cannot infer schema format from file '{}'. Use a .capnp, .fbs, or .wit extension.", file_path);
                            process::exit(1);
                        }
                    }
                });
                let file_schema = load_schema_from_file(&file_path, schema_fmt, &type_name);

                let input_path = input.unwrap_or_else(|| {
                    eprintln!("Error: input file required when using file-based schemas (no example data generation).");
                    process::exit(1);
                });
                let data = read_input(&input_path);

                let view = DynamicView::new(&data, &file_schema.schema, format.to_wire_format());

                println!("Format: {}", format.name());
                println!("Schema: {}", file_schema.name);
                println!("Size:   {} bytes", data.len());
                println!();
                println!("{}", format_view(&view));
            }
        }

        Commands::Validate {
            format,
            schema,
            input,
            example,
        } => {
            if example {
                let ex_schema = registry.get(&schema).unwrap_or_else(|| {
                    eprintln!(
                        "Error: unknown example schema '{}'. Available: {}",
                        schema,
                        registry.names().join(", ")
                    );
                    process::exit(1);
                });

                let data = match input {
                    Some(path) => read_input(&path),
                    None => generate_example_data(ex_schema, format),
                };

                let dyn_schema = get_schema_for_format(ex_schema, format);
                let errors = validate_data(&data, ex_schema, dyn_schema, format);

                if errors.is_empty() {
                    println!("Valid {} data for schema {}", format.name(), ex_schema.name);
                    process::exit(0);
                } else {
                    for err in &errors {
                        eprintln!("Error: {}", err);
                    }
                    process::exit(1);
                }
            } else {
                // File-based schema: basic structural validation
                let (file_path, type_name) = parse_schema_arg(&schema);
                let schema_fmt = infer_schema_format(&file_path).unwrap_or_else(|| {
                    match format {
                        Format::Capnp => SchemaFormat::Capnp,
                        Format::Flatbuf => SchemaFormat::Flatbuf,
                        Format::Wit => SchemaFormat::Wit,
                        Format::Fracpack => {
                            eprintln!("Error: cannot infer schema format from file '{}'. Use a .capnp, .fbs, or .wit extension.", file_path);
                            process::exit(1);
                        }
                    }
                });
                let file_schema = load_schema_from_file(&file_path, schema_fmt, &type_name);

                let input_path = input.unwrap_or_else(|| {
                    eprintln!("Error: input file required when using file-based schemas.");
                    process::exit(1);
                });
                let data = read_input(&input_path);

                // Basic validation: try to read each field via DynamicView
                let view = DynamicView::new(&data, &file_schema.schema, format.to_wire_format());
                let mut errors = Vec::new();
                for (name, ty, _, _) in &file_schema.fields {
                    let val = view.field(name);
                    if matches!(val, DynamicValue::Void) && *ty != DynamicType::Void {
                        errors.push(format!("Field '{}' is void (expected {:?})", name, ty));
                    }
                }

                if errors.is_empty() {
                    println!("Valid {} data for schema {}", format.name(), file_schema.name);
                    process::exit(0);
                } else {
                    for err in &errors {
                        eprintln!("Error: {}", err);
                    }
                    process::exit(1);
                }
            }
        }

        Commands::Convert {
            src_format,
            dst_format,
            schema,
            input,
            output,
            example,
        } => {
            if !example {
                eprintln!("Error: format conversion with file-based schemas is not yet supported.");
                eprintln!("       Use --example for built-in schemas.");
                process::exit(1);
            }

            let ex_schema = registry.get(&schema).unwrap_or_else(|| {
                eprintln!(
                    "Error: unknown example schema '{}'. Available: {}",
                    schema,
                    registry.names().join(", ")
                );
                process::exit(1);
            });

            let data = match input {
                Some(path) => read_input(&path),
                None => generate_example_data(ex_schema, src_format),
            };

            // Read source data using DynamicView
            let src_dyn_schema = get_schema_for_format(ex_schema, src_format);
            let view = DynamicView::new(&data, src_dyn_schema, src_format.to_wire_format());

            // Repack into destination format
            let result = match dst_format {
                Format::Wit => repack_wit(&view, ex_schema),
                Format::Fracpack => repack_fracpack(&view, ex_schema),
                _ => {
                    eprintln!(
                        "Error: {} output format not yet supported for conversion",
                        dst_format.name()
                    );
                    process::exit(1);
                }
            };

            // Write output
            match output {
                Some(path) => {
                    std::fs::write(&path, &result).unwrap_or_else(|e| {
                        eprintln!("Error writing '{}': {}", path, e);
                        process::exit(1);
                    });
                    eprintln!(
                        "Converted {} -> {} ({} -> {} bytes)",
                        src_format.name(),
                        dst_format.name(),
                        data.len(),
                        result.len()
                    );
                }
                None => {
                    io::stdout().write_all(&result).unwrap_or_else(|e| {
                        eprintln!("Error writing to stdout: {}", e);
                        process::exit(1);
                    });
                }
            }
        }

        Commands::Schema {
            format,
            schema,
            example,
        } => {
            if example {
                let ex_schema = registry.get(&schema).unwrap_or_else(|| {
                    eprintln!(
                        "Error: unknown example schema '{}'. Available: {}",
                        schema,
                        registry.names().join(", ")
                    );
                    process::exit(1);
                });

                let idl = match format {
                    SchemaFormat::Capnp => export_capnp_schema(ex_schema),
                    SchemaFormat::Flatbuf => export_fbs_schema(ex_schema),
                    SchemaFormat::Wit => export_wit_schema(ex_schema),
                };

                print!("{}", idl);
            } else {
                // Parse schema from file: "path/to/file.capnp:TypeName"
                // Source format is inferred from file extension; `format` is the target
                let (file_path, type_name) = parse_schema_arg(&schema);
                let source_format = infer_schema_format(&file_path).unwrap_or_else(|| {
                    eprintln!("Error: cannot infer schema format from file extension: {}", file_path);
                    eprintln!("Use a file with .capnp, .fbs, or .wit extension");
                    process::exit(1);
                });
                let file_schema =
                    load_schema_from_file(&file_path, source_format, &type_name);

                let idl = match format {
                    SchemaFormat::Capnp => {
                        schema_export::to_capnp_schema_from_dynamic(
                            &file_schema.name,
                            &file_schema.schema,
                        )
                    }
                    SchemaFormat::Flatbuf => {
                        schema_export::to_fbs_schema_from_dynamic(
                            &file_schema.name,
                            &file_schema.schema,
                        )
                    }
                    SchemaFormat::Wit => {
                        schema_export::to_wit_schema(
                            &file_schema.name,
                            &file_schema.schema,
                        )
                    }
                };

                print!("{}", idl);
            }
        }

        Commands::Codegen {
            format,
            schema,
            lang,
            output,
            example,
        } => {
            let code = if example {
                let ex_schema = registry.get(&schema).unwrap_or_else(|| {
                    eprintln!(
                        "Error: unknown example schema '{}'. Available: {}",
                        schema,
                        registry.names().join(", ")
                    );
                    process::exit(1);
                });

                let source_label = format!("{} (built-in example)", schema);
                match lang {
                    CodegenLang::Cpp => codegen_cpp(ex_schema, &source_label),
                    CodegenLang::Rust => codegen_rust(ex_schema, &source_label),
                    CodegenLang::Go => codegen_go(ex_schema, &source_label),
                    CodegenLang::Typescript => codegen_typescript(ex_schema, &source_label),
                    CodegenLang::Python => codegen_python(ex_schema, &source_label),
                    CodegenLang::Zig => codegen_zig(ex_schema, &source_label),
                }
            } else {
                // Parse schema from file: "path/to/file.capnp:TypeName"
                let (file_path, type_name) = parse_schema_arg(&schema);
                let file_schema =
                    load_schema_from_file(&file_path, format, &type_name);
                let source_label = format!("{}:{}", file_path, type_name);
                codegen_from_dynamic(&file_schema, lang, &source_label)
            };

            match output {
                Some(path) => {
                    std::fs::write(&path, &code).unwrap_or_else(|e| {
                        eprintln!("Error writing '{}': {}", path, e);
                        process::exit(1);
                    });
                    eprintln!("wrote {}", path);
                }
                None => {
                    print!("{}", code);
                }
            }
        }
    }
}
