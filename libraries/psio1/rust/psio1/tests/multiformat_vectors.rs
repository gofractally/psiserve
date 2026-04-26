//! Cross-format wire format test vectors.
//!
//! Verifies that the same logical values produce the expected binary encoding
//! in each format (fracpack, WIT Canonical ABI, Cap'n Proto, FlatBuffers)
//! and that the encodings can be decoded back to the original values.

use psio1::capnp::layout::{FieldKind, MemberDesc};
use psio1::capnp::pack::{pack_data_field, CapnpPack, WordBuf};
use psio1::capnp::unpack::CapnpUnpack;
use psio1::capnp::view::CapnpView;
use psio1::dynamic_schema::{DynamicType, SchemaBuilder};
use psio1::dynamic_view::{DynamicValue, DynamicView, WireFormat};
use psio1::flatbuf::builder::{FbBuilder, FbPack, FbTableWriter};
use psio1::flatbuf::layout::FbLayout;
use psio1::flatbuf::unpack::{FbUnpack, FbUnpackError};
use psio1::flatbuf::view::FbView;
use psio1::schema_export;
use psio1::wit::pack::WitPack;
use psio1::wit::view::WitUnpack;
use psio1::{Pack, Unpack};
use serde::Deserialize;
use std::path::Path;

// ============================================================
// Test structs -- the canonical types used across all formats
// ============================================================

// For fracpack:
#[derive(Pack, Unpack, PartialEq, Debug, Clone)]
#[fracpack(fracpack_mod = "psio")]
#[fracpack(definition_will_not_change)]
#[allow(dead_code)]
struct FracPoint {
    x: i32,
    y: i32,
}

// For capnp:
#[derive(Debug, Clone, PartialEq)]
struct CapnpPoint {
    x: i32,
    y: i32,
}

impl CapnpPack for CapnpPoint {
    fn member_descs() -> Vec<MemberDesc> {
        vec![
            MemberDesc::Simple(FieldKind::Scalar(4)),
            MemberDesc::Simple(FieldKind::Scalar(4)),
        ]
    }

    fn pack_into(&self, buf: &mut WordBuf, data_start: u32, _ptrs_start: u32) {
        let layout = Self::capnp_layout();
        pack_data_field(buf, data_start, &layout.fields[0], self.x);
        pack_data_field(buf, data_start, &layout.fields[1], self.y);
    }
}

impl CapnpUnpack for CapnpPoint {
    fn unpack_from(view: &CapnpView) -> Self {
        let layout = <Self as CapnpPack>::capnp_layout();
        CapnpPoint {
            x: view.read_i32(&layout.fields[0]),
            y: view.read_i32(&layout.fields[1]),
        }
    }
}

// For flatbuf:
#[derive(Debug, Clone, PartialEq)]
struct FbPoint {
    x: i32,
    y: i32,
}

fn fb_point_layout() -> FbLayout {
    FbLayout::simple(2)
}

impl FbPack for FbPoint {
    fn fb_write(&self, b: &mut FbBuilder) -> u32 {
        let layout = fb_point_layout();
        let mut tw = FbTableWriter::new(b, &layout);
        tw.add_i32(0, self.x, 0);
        tw.add_i32(1, self.y, 0);
        tw.finish()
    }
}

impl FbUnpack for FbPoint {
    fn fb_unpack(data: &[u8]) -> Result<Self, FbUnpackError> {
        let view = FbView::from_buffer(data)
            .map_err(|e| FbUnpackError::InvalidBuffer(e.to_string()))?;
        Self::fb_unpack_view(&view)
    }

    fn fb_unpack_view(view: &FbView<'_>) -> Result<Self, FbUnpackError> {
        let x = view.read_i32(0, 0);
        let y = view.read_i32(1, 0);
        Ok(FbPoint { x, y })
    }
}

// ============================================================
// Helpers
// ============================================================

fn to_hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{:02X}", b)).collect::<String>()
}

fn from_hex(s: &str) -> Vec<u8> {
    (0..s.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap())
        .collect()
}

// ============================================================
// JSON test vector schema
// ============================================================

#[derive(Deserialize)]
struct MultiFormatVectors {
    #[allow(dead_code)]
    description: String,
    types: std::collections::HashMap<String, TypeEntry>,
}

#[derive(Deserialize)]
struct TypeEntry {
    #[allow(dead_code)]
    description: String,
    #[allow(dead_code)]
    fields: std::collections::HashMap<String, String>,
    cases: Vec<CaseEntry>,
}

#[derive(Deserialize)]
struct CaseEntry {
    name: String,
    values: serde_json::Value,
    #[serde(default)]
    #[allow(dead_code)]
    fracpack_hex: Option<String>,
    #[serde(default)]
    wit_hex: Option<String>,
    #[serde(default)]
    capnp_hex: Option<String>,
    #[serde(default)]
    flatbuf_hex: Option<String>,
}

fn load_wire_vectors() -> MultiFormatVectors {
    let path = Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .join("test_vectors/wire_format_vectors.json");
    let content = std::fs::read_to_string(&path)
        .unwrap_or_else(|e| panic!("Cannot read {}: {}", path.display(), e));
    serde_json::from_str(&content).expect("Failed to parse wire_format_vectors.json")
}

// ============================================================
// WIT format round-trip tests
// ============================================================

#[test]
fn wit_point_roundtrip() {
    let packed = (42i32, -17i32).wit_pack();
    let hex = to_hex(&packed);
    eprintln!("WIT Point(42, -17) hex: {}", hex);

    // Verify unpack
    let (x, y) = <(i32, i32)>::wit_unpack(&packed).unwrap();
    assert_eq!(x, 42);
    assert_eq!(y, -17);
}

#[test]
fn wit_point_zeros() {
    let packed = (0i32, 0i32).wit_pack();
    let hex = to_hex(&packed);
    eprintln!("WIT Point(0, 0) hex: {}", hex);
    assert_eq!(hex, "0000000000000000");
}

#[test]
fn wit_point_positive() {
    let packed = (42i32, 100i32).wit_pack();
    let hex = to_hex(&packed);
    eprintln!("WIT Point(42, 100) hex: {}", hex);
    assert_eq!(hex, "2A00000064000000");
}

// ============================================================
// Cap'n Proto format round-trip tests
// ============================================================

#[test]
fn capnp_point_roundtrip() {
    let p = CapnpPoint { x: 42, y: -17 };
    let packed = p.capnp_pack();
    let hex = to_hex(&packed);
    eprintln!("Capnp Point(42, -17) hex: {}", hex);

    // Unpack
    let restored = CapnpPoint::capnp_unpack(&packed).unwrap();
    assert_eq!(restored, p);
}

#[test]
fn capnp_point_zeros() {
    let p = CapnpPoint { x: 0, y: 0 };
    let packed = p.capnp_pack();
    let hex = to_hex(&packed);
    eprintln!("Capnp Point(0, 0) hex: {}", hex);

    let restored = CapnpPoint::capnp_unpack(&packed).unwrap();
    assert_eq!(restored, p);
}

// ============================================================
// FlatBuffer format round-trip tests
// ============================================================

#[test]
fn flatbuf_point_roundtrip() {
    let p = FbPoint { x: 42, y: -17 };
    let packed = p.fb_pack();
    let hex = to_hex(&packed);
    eprintln!("FlatBuf Point(42, -17) hex: {}", hex);

    let view = FbView::from_buffer(&packed).unwrap();
    let restored = FbPoint::fb_unpack_view(&view).unwrap();
    assert_eq!(restored, p);
}

#[test]
fn flatbuf_point_zeros() {
    let p = FbPoint { x: 0, y: 0 };
    let packed = p.fb_pack();
    let hex = to_hex(&packed);
    eprintln!("FlatBuf Point(0, 0) hex: {}", hex);

    let view = FbView::from_buffer(&packed).unwrap();
    let restored = FbPoint::fb_unpack_view(&view).unwrap();
    assert_eq!(restored, p);
}

// ============================================================
// Multi-format wire vector verification from JSON
// ============================================================

#[test]
fn verify_point_wit_vectors() {
    let vectors = load_wire_vectors();
    let point = vectors.types.get("Point").expect("Point type not found");

    for case in &point.cases {
        if let Some(ref wit_hex) = case.wit_hex {
            let expected_bytes = from_hex(wit_hex);
            let x = case.values["x"].as_i64().unwrap() as i32;
            let y = case.values["y"].as_i64().unwrap() as i32;

            let packed = (x, y).wit_pack();
            let actual_hex = to_hex(&packed);

            assert_eq!(
                actual_hex, *wit_hex,
                "Point::{} WIT mismatch: expected {}, got {}",
                case.name, wit_hex, actual_hex
            );

            // Verify decode
            let (dx, dy) = <(i32, i32)>::wit_unpack(&expected_bytes).unwrap();
            assert_eq!(dx, x, "Point::{} WIT decode x mismatch", case.name);
            assert_eq!(dy, y, "Point::{} WIT decode y mismatch", case.name);
        }
    }
}

#[test]
fn verify_point_capnp_vectors() {
    let vectors = load_wire_vectors();
    let point = vectors.types.get("Point").expect("Point type not found");

    for case in &point.cases {
        if let Some(ref capnp_hex) = case.capnp_hex {
            let expected_bytes = from_hex(capnp_hex);
            let x = case.values["x"].as_i64().unwrap() as i32;
            let y = case.values["y"].as_i64().unwrap() as i32;

            let p = CapnpPoint { x, y };
            let packed = p.capnp_pack();
            let actual_hex = to_hex(&packed);

            assert_eq!(
                actual_hex, *capnp_hex,
                "Point::{} Capnp mismatch: expected {}, got {}",
                case.name, capnp_hex, actual_hex
            );

            // Verify decode
            let restored = CapnpPoint::capnp_unpack(&expected_bytes).unwrap();
            assert_eq!(restored.x, x, "Point::{} Capnp decode x mismatch", case.name);
            assert_eq!(restored.y, y, "Point::{} Capnp decode y mismatch", case.name);
        }
    }
}

#[test]
fn verify_point_flatbuf_vectors() {
    let vectors = load_wire_vectors();
    let point = vectors.types.get("Point").expect("Point type not found");

    for case in &point.cases {
        if let Some(ref fb_hex) = case.flatbuf_hex {
            let expected_bytes = from_hex(fb_hex);
            let x = case.values["x"].as_i64().unwrap() as i32;
            let y = case.values["y"].as_i64().unwrap() as i32;

            let p = FbPoint { x, y };
            let packed = p.fb_pack();
            let actual_hex = to_hex(&packed);

            assert_eq!(
                actual_hex, *fb_hex,
                "Point::{} FlatBuf mismatch: expected {}, got {}",
                case.name, fb_hex, actual_hex
            );

            // Verify decode
            let view = FbView::from_buffer(&expected_bytes).unwrap();
            let restored = FbPoint::fb_unpack_view(&view).unwrap();
            assert_eq!(restored.x, x, "Point::{} FlatBuf decode x mismatch", case.name);
            assert_eq!(restored.y, y, "Point::{} FlatBuf decode y mismatch", case.name);
        }
    }
}

// ============================================================
// Schema export golden file tests
// ============================================================

#[test]
fn schema_export_point_capnp() {
    let schema = SchemaBuilder::new()
        .field_scalar("x", DynamicType::I32, 0)
        .field_scalar("y", DynamicType::I32, 4)
        .data_words(1)
        .build();

    let capnp = schema_export::to_capnp_schema_from_dynamic("Point", &schema);

    let expected = "\
struct Point {
  x @0 :Int32;
  y @1 :Int32;
}
";
    assert_eq!(capnp, expected, "capnp schema export mismatch");
}

#[test]
fn schema_export_point_fbs() {
    let schema = SchemaBuilder::new()
        .field_scalar("x", DynamicType::I32, 0)
        .field_scalar("y", DynamicType::I32, 4)
        .data_words(1)
        .build();

    let fbs = schema_export::to_fbs_schema_from_dynamic("Point", &schema);

    let expected = "\
table Point {
  x:int;
  y:int;
}
";
    assert_eq!(fbs, expected, "fbs schema export mismatch");
}

#[test]
fn schema_export_point_wit() {
    let schema = SchemaBuilder::new()
        .field_scalar("x", DynamicType::I32, 0)
        .field_scalar("y", DynamicType::I32, 4)
        .data_words(1)
        .build();

    let wit = schema_export::to_wit_schema("Point", &schema);

    let expected = "\
record point {
  x: s32,
  y: s32,
}
";
    assert_eq!(wit, expected, "wit schema export mismatch");
}

// ============================================================
// Schema export golden file verification from JSON
// ============================================================

#[derive(Deserialize)]
struct SchemaExportGolden {
    #[allow(dead_code)]
    description: String,
    capnp: String,
    fbs: String,
    wit: String,
}

#[test]
fn verify_schema_export_golden_files() {
    let path = Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .join("test_vectors/wire_format_vectors.json");
    let content = std::fs::read_to_string(&path).unwrap();
    let json: serde_json::Value = serde_json::from_str(&content).unwrap();

    let schema_export = json.get("schema_export").expect("schema_export section missing");
    let point = schema_export.get("Point").expect("Point golden missing");
    let golden: SchemaExportGolden = serde_json::from_value(point.clone()).unwrap();

    // Build the DynamicSchema for Point
    let schema = SchemaBuilder::new()
        .field_scalar("x", DynamicType::I32, 0)
        .field_scalar("y", DynamicType::I32, 4)
        .data_words(1)
        .build();

    let capnp = schema_export::to_capnp_schema_from_dynamic("Point", &schema);
    assert_eq!(capnp, golden.capnp, "capnp golden mismatch");

    let fbs = schema_export::to_fbs_schema_from_dynamic("Point", &schema);
    assert_eq!(fbs, golden.fbs, "fbs golden mismatch");

    let wit = schema_export::to_wit_schema("Point", &schema);
    assert_eq!(wit, golden.wit, "wit golden mismatch");
}

// ============================================================
// Verify fracpack vectors still work (existing format)
// ============================================================

#[test]
fn verify_point_fracpack_vectors() {
    let vectors = load_wire_vectors();
    let point = vectors.types.get("Point").expect("Point type not found");

    for case in &point.cases {
        if let Some(ref fp_hex) = case.fracpack_hex {
            let x = case.values["x"].as_i64().unwrap() as i32;
            let y = case.values["y"].as_i64().unwrap() as i32;

            let p = FracPoint { x, y };
            let packed = p.packed();
            let actual_hex = to_hex(&packed);

            assert_eq!(
                actual_hex, *fp_hex,
                "Point::{} fracpack mismatch: expected {}, got {}",
                case.name, fp_hex, actual_hex
            );

            // Verify decode
            let restored = FracPoint::unpacked(&from_hex(fp_hex)).unwrap();
            assert_eq!(restored.x, x, "Point::{} fracpack decode x mismatch", case.name);
            assert_eq!(restored.y, y, "Point::{} fracpack decode y mismatch", case.name);
        }
    }
}

// ============================================================
// Verify DynamicView reads multi-format data correctly
// ============================================================

#[test]
fn dynamic_view_wit_point() {
    let schema = SchemaBuilder::new()
        .field_scalar("x", DynamicType::I32, 0)
        .field_scalar("y", DynamicType::I32, 4)
        .data_words(1)
        .build();

    let data = (42i32, -17i32).wit_pack();
    let view = DynamicView::new(&data, &schema, WireFormat::Wit);

    match view.field("x") {
        DynamicValue::I32(v) => assert_eq!(v, 42),
        other => panic!("expected I32, got {:?}", other),
    }
    match view.field("y") {
        DynamicValue::I32(v) => assert_eq!(v, -17),
        other => panic!("expected I32, got {:?}", other),
    }
}
