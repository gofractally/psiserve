//! Schema roundtrip verification test.
//!
//! Defines structs in Rust, exports their Cap'n Proto schema using
//! `psio::capnp::schema`, and verifies the output is valid `.capnp` syntax
//! by checking structural properties (field ordinals, type names, brackets).

use psio::capnp::schema::{to_capnp_schema, CapnpSchema, SchemaField, SchemaStruct};

// ============================================================================
// Test struct definitions with manual CapnpSchema impls
// ============================================================================

struct Point {
    #[allow(dead_code)]
    x: f32,
    #[allow(dead_code)]
    y: f32,
}

impl CapnpSchema for Point {
    fn capnp_schema() -> SchemaStruct {
        SchemaStruct {
            name: "Point".into(),
            fields: vec![
                SchemaField {
                    name: "x".into(),
                    capnp_type: "Float32".into(),
                    ordinal: 0,
                },
                SchemaField {
                    name: "y".into(),
                    capnp_type: "Float32".into(),
                    ordinal: 1,
                },
            ],
        }
    }
}

struct UserProfile {
    #[allow(dead_code)]
    id: u64,
    #[allow(dead_code)]
    name: String,
    #[allow(dead_code)]
    age: u32,
    #[allow(dead_code)]
    active: bool,
    #[allow(dead_code)]
    score: f64,
}

impl CapnpSchema for UserProfile {
    fn capnp_schema() -> SchemaStruct {
        SchemaStruct {
            name: "UserProfile".into(),
            fields: vec![
                SchemaField {
                    name: "id".into(),
                    capnp_type: "UInt64".into(),
                    ordinal: 0,
                },
                SchemaField {
                    name: "name".into(),
                    capnp_type: "Text".into(),
                    ordinal: 1,
                },
                SchemaField {
                    name: "age".into(),
                    capnp_type: "UInt32".into(),
                    ordinal: 2,
                },
                SchemaField {
                    name: "active".into(),
                    capnp_type: "Bool".into(),
                    ordinal: 3,
                },
                SchemaField {
                    name: "score".into(),
                    capnp_type: "Float64".into(),
                    ordinal: 4,
                },
            ],
        }
    }
}

struct Order {
    #[allow(dead_code)]
    order_id: u64,
    #[allow(dead_code)]
    items: Vec<String>,
    #[allow(dead_code)]
    total: f64,
}

impl CapnpSchema for Order {
    fn capnp_schema() -> SchemaStruct {
        SchemaStruct {
            name: "Order".into(),
            fields: vec![
                SchemaField {
                    name: "orderId".into(),
                    capnp_type: "UInt64".into(),
                    ordinal: 0,
                },
                SchemaField {
                    name: "items".into(),
                    capnp_type: "List(Text)".into(),
                    ordinal: 1,
                },
                SchemaField {
                    name: "total".into(),
                    capnp_type: "Float64".into(),
                    ordinal: 2,
                },
            ],
        }
    }
}

// ============================================================================
// Validator: checks that generated .capnp text is structurally valid
// ============================================================================

/// Validates that a .capnp IDL string has correct syntax structure.
/// This is a lightweight check, not a full parser, but catches common errors.
fn validate_capnp_syntax(idl: &str, expected_name: &str, expected_field_count: usize) {
    let lines: Vec<&str> = idl.lines().collect();
    assert!(
        !lines.is_empty(),
        "schema IDL should not be empty for {}",
        expected_name
    );

    // First line: "struct <Name> {"
    let first = lines[0].trim();
    assert!(
        first.starts_with("struct "),
        "first line should start with 'struct', got: {}",
        first
    );
    assert!(
        first.contains(expected_name),
        "first line should contain struct name '{}', got: {}",
        expected_name,
        first
    );
    assert!(
        first.ends_with('{'),
        "first line should end with '{{', got: {}",
        first
    );

    // Last line: "}"
    let last = lines.last().unwrap().trim();
    assert_eq!(last, "}", "last line should be '}}', got: {}", last);

    // Count field lines (lines matching "  <name> @<ordinal> :<Type>;")
    let field_lines: Vec<&&str> = lines
        .iter()
        .filter(|l| {
            let trimmed = l.trim();
            trimmed.contains('@') && trimmed.ends_with(';')
        })
        .collect();
    assert_eq!(
        field_lines.len(),
        expected_field_count,
        "expected {} field lines for {}, got {}",
        expected_field_count,
        expected_name,
        field_lines.len()
    );

    // Verify ordinals are sequential starting from 0
    for (i, line) in field_lines.iter().enumerate() {
        let expected_ordinal = format!("@{}", i);
        assert!(
            line.contains(&expected_ordinal),
            "field {} should contain ordinal '{}', got: {}",
            i,
            expected_ordinal,
            line
        );
    }

    // Verify each field line has a colon-separated type
    for line in &field_lines {
        let trimmed = line.trim();
        assert!(
            trimmed.contains(':'),
            "field line should contain ':' for type, got: {}",
            trimmed
        );
        // Extract type name (after the last ':' before ';')
        let after_colon = trimmed.rsplit(':').next().unwrap();
        let type_name = after_colon.trim().trim_end_matches(';');
        assert!(
            !type_name.is_empty(),
            "type name should not be empty in: {}",
            trimmed
        );
        // Verify the type is a known Cap'n Proto type
        let valid_types = [
            "Void", "Bool", "Int8", "Int16", "Int32", "Int64", "UInt8", "UInt16", "UInt32",
            "UInt64", "Float32", "Float64", "Text", "Data",
        ];
        let is_known = valid_types.iter().any(|t| type_name == *t)
            || type_name.starts_with("List(");
        assert!(
            is_known,
            "unrecognized capnp type '{}' in: {}",
            type_name, trimmed
        );
    }
}

// ============================================================================
// Tests
// ============================================================================

#[test]
fn schema_roundtrip_point() {
    let schema = Point::capnp_schema();
    let idl = to_capnp_schema(&schema);
    eprintln!("--- Point schema ---\n{}", idl);
    validate_capnp_syntax(&idl, "Point", 2);

    // Verify specific field types
    assert!(idl.contains("x @0 :Float32;"));
    assert!(idl.contains("y @1 :Float32;"));
}

#[test]
fn schema_roundtrip_user_profile() {
    let schema = UserProfile::capnp_schema();
    let idl = to_capnp_schema(&schema);
    eprintln!("--- UserProfile schema ---\n{}", idl);
    validate_capnp_syntax(&idl, "UserProfile", 5);

    assert!(idl.contains("id @0 :UInt64;"));
    assert!(idl.contains("name @1 :Text;"));
    assert!(idl.contains("age @2 :UInt32;"));
    assert!(idl.contains("active @3 :Bool;"));
    assert!(idl.contains("score @4 :Float64;"));
}

#[test]
fn schema_roundtrip_order_with_list() {
    let schema = Order::capnp_schema();
    let idl = to_capnp_schema(&schema);
    eprintln!("--- Order schema ---\n{}", idl);
    validate_capnp_syntax(&idl, "Order", 3);

    assert!(idl.contains("orderId @0 :UInt64;"));
    assert!(idl.contains("items @1 :List(Text);"));
    assert!(idl.contains("total @2 :Float64;"));
}

#[test]
fn schema_all_primitive_types() {
    let schema = SchemaStruct {
        name: "AllTypes".into(),
        fields: vec![
            SchemaField { name: "v".into(), capnp_type: "Void".into(), ordinal: 0 },
            SchemaField { name: "b".into(), capnp_type: "Bool".into(), ordinal: 1 },
            SchemaField { name: "i8".into(), capnp_type: "Int8".into(), ordinal: 2 },
            SchemaField { name: "i16".into(), capnp_type: "Int16".into(), ordinal: 3 },
            SchemaField { name: "i32".into(), capnp_type: "Int32".into(), ordinal: 4 },
            SchemaField { name: "i64".into(), capnp_type: "Int64".into(), ordinal: 5 },
            SchemaField { name: "u8".into(), capnp_type: "UInt8".into(), ordinal: 6 },
            SchemaField { name: "u16".into(), capnp_type: "UInt16".into(), ordinal: 7 },
            SchemaField { name: "u32".into(), capnp_type: "UInt32".into(), ordinal: 8 },
            SchemaField { name: "u64".into(), capnp_type: "UInt64".into(), ordinal: 9 },
            SchemaField { name: "f32".into(), capnp_type: "Float32".into(), ordinal: 10 },
            SchemaField { name: "f64".into(), capnp_type: "Float64".into(), ordinal: 11 },
            SchemaField { name: "t".into(), capnp_type: "Text".into(), ordinal: 12 },
            SchemaField { name: "d".into(), capnp_type: "Data".into(), ordinal: 13 },
        ],
    };
    let idl = to_capnp_schema(&schema);
    eprintln!("--- AllTypes schema ---\n{}", idl);
    validate_capnp_syntax(&idl, "AllTypes", 14);
}

// ============================================================================
// WIT schema export tests (via schema_export::to_wit_schema)
// ============================================================================

#[test]
fn schema_roundtrip_wit_point() {
    use psio::dynamic_schema::{SchemaBuilder, DynamicType};
    use psio::schema_export::to_wit_schema;

    let schema = SchemaBuilder::new()
        .field_scalar("x", DynamicType::F32, 0)
        .field_scalar("y", DynamicType::F32, 4)
        .data_words(1)
        .build();

    let wit = to_wit_schema("Point", &schema);
    eprintln!("--- Point WIT schema ---\n{}", wit);

    assert!(wit.contains("record point {"));
    assert!(wit.contains("  x: f32,"));
    assert!(wit.contains("  y: f32,"));
    assert!(wit.ends_with("}\n"));
}

#[test]
fn schema_roundtrip_wit_user_profile() {
    use psio::dynamic_schema::{SchemaBuilder, DynamicType};
    use psio::schema_export::to_wit_schema;

    let schema = SchemaBuilder::new()
        .field_scalar("id", DynamicType::U64, 0)
        .field_pointer("name", DynamicType::Text, 0)
        .field_scalar("age", DynamicType::U32, 8)
        .field_scalar("active", DynamicType::Bool, 12)
        .field_scalar("score", DynamicType::F64, 16)
        .data_words(3)
        .ptr_count(1)
        .build();

    let wit = to_wit_schema("UserProfile", &schema);
    eprintln!("--- UserProfile WIT schema ---\n{}", wit);

    assert!(wit.contains("record user-profile {"));
    assert!(wit.contains("  id: u64,"));
    assert!(wit.contains("  name: string,"));
    assert!(wit.contains("  age: u32,"));
    assert!(wit.contains("  active: bool,"));
    assert!(wit.contains("  score: f64,"));
}

#[test]
fn schema_roundtrip_wit_all_scalars() {
    use psio::dynamic_schema::{SchemaBuilder, DynamicType};
    use psio::schema_export::to_wit_schema;

    let schema = SchemaBuilder::new()
        .field_scalar("a_bool", DynamicType::Bool, 0)
        .field_scalar("a_u8", DynamicType::U8, 1)
        .field_scalar("a_i8", DynamicType::I8, 2)
        .field_scalar("a_u16", DynamicType::U16, 4)
        .field_scalar("a_i16", DynamicType::I16, 6)
        .field_scalar("a_u32", DynamicType::U32, 8)
        .field_scalar("a_i32", DynamicType::I32, 12)
        .field_scalar("a_u64", DynamicType::U64, 16)
        .field_scalar("a_i64", DynamicType::I64, 24)
        .field_scalar("a_f32", DynamicType::F32, 32)
        .field_scalar("a_f64", DynamicType::F64, 40)
        .data_words(6)
        .build();

    let wit = to_wit_schema("AllScalars", &schema);
    eprintln!("--- AllScalars WIT schema ---\n{}", wit);

    // Validate WIT type mapping for every scalar
    assert!(wit.contains("  a-bool: bool,"));
    assert!(wit.contains("  a-u8: u8,"));
    assert!(wit.contains("  a-i8: s8,"));
    assert!(wit.contains("  a-u16: u16,"));
    assert!(wit.contains("  a-i16: s16,"));
    assert!(wit.contains("  a-u32: u32,"));
    assert!(wit.contains("  a-i32: s32,"));
    assert!(wit.contains("  a-u64: u64,"));
    assert!(wit.contains("  a-i64: s64,"));
    assert!(wit.contains("  a-f32: f32,"));
    assert!(wit.contains("  a-f64: f64,"));
}

#[test]
fn schema_with_nested_list() {
    let schema = SchemaStruct {
        name: "Nested".into(),
        fields: vec![
            SchemaField { name: "tags".into(), capnp_type: "List(Text)".into(), ordinal: 0 },
            SchemaField { name: "ids".into(), capnp_type: "List(UInt64)".into(), ordinal: 1 },
            SchemaField { name: "scores".into(), capnp_type: "List(Float64)".into(), ordinal: 2 },
        ],
    };
    let idl = to_capnp_schema(&schema);
    eprintln!("--- Nested schema ---\n{}", idl);
    validate_capnp_syntax(&idl, "Nested", 3);

    assert!(idl.contains("tags @0 :List(Text);"));
    assert!(idl.contains("ids @1 :List(UInt64);"));
    assert!(idl.contains("scores @2 :List(Float64);"));
}
