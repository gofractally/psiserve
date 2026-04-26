//! Integration tests for the ToDynamicSchema derive macro and dynamic view features.

use psio1::dynamic_schema::DynamicType;
use psio1::dynamic_view::{DynamicView, FromDynamicView, HashedPath, ToDynamicSchema, WireFormat};
use psio1::wit::pack::WitPack;

// ── Test struct with the derive macro ──────────────────────────────────

#[derive(psio1::ToDynamicSchema)]
struct Point {
    x: i32,
    y: i32,
}

#[test]
fn dynamic_schema_derive_basic() {
    let schema = Point::dynamic_schema();
    assert_eq!(schema.field_count(), 2);

    let fx = schema.find_by_name("x").unwrap();
    assert_eq!(fx.ty, DynamicType::I32);
    assert_eq!(fx.offset, 0);

    let fy = schema.find_by_name("y").unwrap();
    assert_eq!(fy.ty, DynamicType::I32);
    assert_eq!(fy.offset, 4);
}

#[test]
fn dynamic_schema_derive_with_wit_data() {
    let schema = Point::dynamic_schema();

    // Pack as WIT (i32, i32) matches the layout produced by the derive
    let buf = (10i32, 20i32).wit_pack();

    let view = DynamicView::new(&buf, &schema, WireFormat::Wit);
    assert_eq!(view.field("x").as_i32(), Some(10));
    assert_eq!(view.field("y").as_i32(), Some(20));
}

// ── Multi-type struct ──────────────────────────────────────────────────

#[derive(psio1::ToDynamicSchema)]
struct MixedTypes {
    flag: bool,
    small: u8,
    medium: u16,
    count: u32,
    big: u64,
    ratio: f32,
    precise: f64,
}

#[test]
fn dynamic_schema_derive_mixed_types() {
    let schema = MixedTypes::dynamic_schema();
    assert_eq!(schema.field_count(), 7);

    // Verify types
    assert_eq!(
        schema.find_by_name("flag").unwrap().ty,
        DynamicType::Bool
    );
    assert_eq!(
        schema.find_by_name("small").unwrap().ty,
        DynamicType::U8
    );
    assert_eq!(
        schema.find_by_name("medium").unwrap().ty,
        DynamicType::U16
    );
    assert_eq!(
        schema.find_by_name("count").unwrap().ty,
        DynamicType::U32
    );
    assert_eq!(
        schema.find_by_name("big").unwrap().ty,
        DynamicType::U64
    );
    assert_eq!(
        schema.find_by_name("ratio").unwrap().ty,
        DynamicType::F32
    );
    assert_eq!(
        schema.find_by_name("precise").unwrap().ty,
        DynamicType::F64
    );
}

// ── Derive + duck-typed extraction ─────────────────────────────────────

#[derive(psio1::ToDynamicSchema, Debug, PartialEq)]
struct SimpleRecord {
    id: u32,
    value: i64,
}

impl FromDynamicView for SimpleRecord {
    fn from_dynamic_view(view: &DynamicView) -> Option<Self> {
        Some(SimpleRecord {
            id: view.field("id").as_u32()?,
            value: view.field("value").as_i64()?,
        })
    }
}

#[test]
fn dynamic_schema_derive_with_extraction() {
    let schema = SimpleRecord::dynamic_schema();

    // WIT layout for { id: u32, value: i64 } matches derive:
    // id at offset 0 (4 bytes), pad to 8, value at offset 8
    let buf = (42u32, -7i64).wit_pack();

    let view = DynamicView::new(&buf, &schema, WireFormat::Wit);
    let record: SimpleRecord = view.as_type().unwrap();
    assert_eq!(record, SimpleRecord { id: 42, value: -7 });
}

// ── Derive + HashedPath ────────────────────────────────────────────────

#[test]
fn dynamic_schema_derive_with_hashed_path() {
    let schema = Point::dynamic_schema();
    let buf = (100i32, 200i32).wit_pack();
    let view = DynamicView::new(&buf, &schema, WireFormat::Wit);

    let path_x = HashedPath::new("x");
    let path_y = HashedPath::new("y");

    assert_eq!(view.eval(&path_x).as_i32(), Some(100));
    assert_eq!(view.eval(&path_y).as_i32(), Some(200));
}

// ── Schema with string field ───────────────────────────────────────────

#[derive(psio1::ToDynamicSchema)]
struct WithString {
    name: String,
    age: u32,
}

#[test]
fn dynamic_schema_derive_with_string_field() {
    let schema = WithString::dynamic_schema();
    assert_eq!(schema.field_count(), 2);

    let fn_name = schema.find_by_name("name").unwrap();
    assert_eq!(fn_name.ty, DynamicType::Text);

    let fn_age = schema.find_by_name("age").unwrap();
    assert_eq!(fn_age.ty, DynamicType::U32);
}
