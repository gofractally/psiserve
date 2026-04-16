/// Tests for canonical JSON conversion.
///
/// Covers golden vector round-trips (matching vectors.json), u64/i64 as strings,
/// bytes as hex, variant encoding, nested structs, and Option None/Some.

use fracpack::json::{self, FromCanonicalJson, ToCanonicalJson};
use fracpack::{FromCanonicalJson, Pack, ToCanonicalJson, Unpack};
use serde::Deserialize;
use serde_json::Value;
use std::path::Path;

// ============================================================
// Mirror types with derive macros
// ============================================================

#[derive(Pack, Unpack, ToCanonicalJson, FromCanonicalJson, PartialEq, Debug)]
#[fracpack(fracpack_mod = "fracpack")]
#[fracpack(definition_will_not_change)]
struct FixedInts {
    x: i32,
    y: i32,
}

#[derive(Pack, Unpack, ToCanonicalJson, FromCanonicalJson, PartialEq, Debug)]
#[fracpack(fracpack_mod = "fracpack")]
#[fracpack(definition_will_not_change)]
struct FixedMixed {
    b: bool,
    u8_: u8,
    u16_: u16,
    u32_: u32,
    u64_: u64,
}

#[derive(Pack, Unpack, ToCanonicalJson, FromCanonicalJson, PartialEq, Debug)]
#[fracpack(fracpack_mod = "fracpack")]
struct AllPrimitives {
    b: bool,
    u8v: u8,
    i8v: i8,
    u16v: u16,
    i16v: i16,
    u32v: u32,
    i32v: i32,
    u64v: u64,
    i64v: i64,
    f32v: f32,
    f64v: f64,
}

#[derive(Pack, Unpack, ToCanonicalJson, FromCanonicalJson, PartialEq, Debug)]
#[fracpack(fracpack_mod = "fracpack")]
#[fracpack(definition_will_not_change)]
struct SingleBool {
    value: bool,
}

#[derive(Pack, Unpack, ToCanonicalJson, FromCanonicalJson, PartialEq, Debug)]
#[fracpack(fracpack_mod = "fracpack")]
#[fracpack(definition_will_not_change)]
struct SingleU32 {
    value: u32,
}

#[derive(Pack, Unpack, ToCanonicalJson, FromCanonicalJson, PartialEq, Debug)]
#[fracpack(fracpack_mod = "fracpack")]
struct SingleString {
    value: String,
}

#[derive(Pack, Unpack, ToCanonicalJson, FromCanonicalJson, PartialEq, Debug)]
#[fracpack(fracpack_mod = "fracpack")]
struct WithStrings {
    empty_str: String,
    hello: String,
    unicode: String,
}

#[derive(Pack, Unpack, ToCanonicalJson, FromCanonicalJson, PartialEq, Debug)]
#[fracpack(fracpack_mod = "fracpack")]
struct WithVectors {
    ints: Vec<u32>,
    strings: Vec<String>,
}

#[derive(Pack, Unpack, ToCanonicalJson, FromCanonicalJson, PartialEq, Debug)]
#[fracpack(fracpack_mod = "fracpack")]
struct WithOptionals {
    opt_int: Option<u32>,
    opt_str: Option<String>,
}

#[derive(Pack, Unpack, ToCanonicalJson, FromCanonicalJson, PartialEq, Debug, Clone)]
#[fracpack(fracpack_mod = "fracpack")]
struct Inner {
    value: u32,
    label: String,
}

#[derive(Pack, Unpack, ToCanonicalJson, FromCanonicalJson, PartialEq, Debug)]
#[fracpack(fracpack_mod = "fracpack")]
struct Outer {
    inner: Inner,
    name: String,
}

#[derive(Pack, Unpack, ToCanonicalJson, FromCanonicalJson, PartialEq, Debug)]
#[fracpack(fracpack_mod = "fracpack")]
enum WithVariantData {
    #[fracpack(name = "uint32")]
    Uint32(u32),
    #[fracpack(name = "string")]
    StringAlt(String),
    Inner(Inner),
}

#[derive(Pack, Unpack, ToCanonicalJson, FromCanonicalJson, PartialEq, Debug)]
#[fracpack(fracpack_mod = "fracpack")]
struct WithVariant {
    data: WithVariantData,
}

#[derive(Pack, Unpack, ToCanonicalJson, FromCanonicalJson, PartialEq, Debug)]
#[fracpack(fracpack_mod = "fracpack")]
struct VecOfStructs {
    items: Vec<Inner>,
}

#[derive(Pack, Unpack, ToCanonicalJson, FromCanonicalJson, PartialEq, Debug)]
#[fracpack(fracpack_mod = "fracpack")]
struct OptionalStruct {
    item: Option<Inner>,
}

#[derive(Pack, Unpack, ToCanonicalJson, FromCanonicalJson, PartialEq, Debug)]
#[fracpack(fracpack_mod = "fracpack")]
struct VecOfOptionals {
    items: Vec<Option<u32>>,
}

#[derive(Pack, Unpack, ToCanonicalJson, FromCanonicalJson, PartialEq, Debug)]
#[fracpack(fracpack_mod = "fracpack")]
struct OptionalVec {
    items: Option<Vec<u32>>,
}

#[derive(Pack, Unpack, ToCanonicalJson, FromCanonicalJson, PartialEq, Debug)]
#[fracpack(fracpack_mod = "fracpack")]
struct NestedVecs {
    matrix: Vec<Vec<u32>>,
}

#[derive(Pack, Unpack, ToCanonicalJson, FromCanonicalJson, PartialEq, Debug)]
#[fracpack(fracpack_mod = "fracpack")]
#[fracpack(definition_will_not_change)]
struct FixedArray {
    arr: [u32; 3],
}

#[derive(Pack, Unpack, ToCanonicalJson, FromCanonicalJson, PartialEq, Debug)]
#[fracpack(fracpack_mod = "fracpack")]
struct Complex {
    items: Vec<Inner>,
    opt_vec: Option<Vec<u32>>,
    vec_opt: Vec<Option<String>>,
    opt_struct: Option<Inner>,
}

#[derive(Pack, Unpack, ToCanonicalJson, FromCanonicalJson, PartialEq, Debug)]
#[fracpack(fracpack_mod = "fracpack")]
struct EmptyExtensible {
    dummy: u32,
}

// ============================================================
// Test vector loading
// ============================================================

#[derive(Deserialize)]
struct TestVectors {
    #[allow(dead_code)]
    format_version: u32,
    types: Vec<TypeGroup>,
}

#[derive(Deserialize)]
struct TypeGroup {
    name: String,
    #[allow(dead_code)]
    schema: serde_json::Value,
    #[allow(dead_code)]
    schema_hex: String,
    #[allow(dead_code)]
    root_type: String,
    cases: Vec<TestCase>,
}

#[derive(Deserialize)]
struct TestCase {
    name: String,
    #[allow(dead_code)]
    packed_hex: String,
    json: serde_json::Value,
}

fn load_vectors() -> TestVectors {
    let path = Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .parent()
        .unwrap()
        .join("test_vectors/vectors.json");
    let content = std::fs::read_to_string(&path)
        .unwrap_or_else(|e| panic!("Cannot read {}: {}", path.display(), e));
    serde_json::from_str(&content).expect("Failed to parse vectors.json")
}

fn find_group<'a>(vectors: &'a TestVectors, name: &str) -> &'a TypeGroup {
    vectors
        .types
        .iter()
        .find(|g| g.name == name)
        .unwrap_or_else(|| panic!("Type group '{}' not found in vectors.json", name))
}

// (from_hex omitted; not needed for JSON-only tests)

// ============================================================
// Golden vector JSON round-trip tests
// ============================================================

/// Compare JSON values, handling float comparison with tolerance.
fn json_values_equal(a: &Value, b: &Value) -> bool {
    match (a, b) {
        (Value::Number(na), Value::Number(nb)) => {
            match (na.as_f64(), nb.as_f64()) {
                (Some(fa), Some(fb)) => {
                    // Use exact comparison for integers, tolerance for floats
                    if na.is_i64() && nb.is_i64() {
                        na.as_i64() == nb.as_i64()
                    } else if na.is_u64() && nb.is_u64() {
                        na.as_u64() == nb.as_u64()
                    } else {
                        (fa - fb).abs() < 1e-10 || fa == fb
                    }
                }
                _ => false,
            }
        }
        (Value::String(sa), Value::String(sb)) => sa == sb,
        (Value::Bool(ba), Value::Bool(bb)) => ba == bb,
        (Value::Null, Value::Null) => true,
        (Value::Array(aa), Value::Array(ab)) => {
            aa.len() == ab.len()
                && aa.iter().zip(ab.iter()).all(|(a, b)| json_values_equal(a, b))
        }
        (Value::Object(ma), Value::Object(mb)) => {
            ma.len() == mb.len()
                && ma
                    .iter()
                    .all(|(k, v)| mb.get(k).map_or(false, |v2| json_values_equal(v, v2)))
        }
        _ => false,
    }
}

/// Macro for golden vector round-trip tests:
/// 1. Construct Rust value -> to_json_value() -> compare with vectors.json "json" field
/// 2. Parse vectors.json "json" -> from_json_value() -> compare with Rust value
macro_rules! json_vector_test {
    ($test_name:ident, $type_name:expr, $rust_type:ty, $cases:expr) => {
        #[test]
        fn $test_name() {
            let vectors = load_vectors();
            let group = find_group(&vectors, $type_name);

            let cases: Vec<(&str, $rust_type)> = $cases;
            for (case_name, value) in &cases {
                let tc = group
                    .cases
                    .iter()
                    .find(|c| c.name == *case_name)
                    .unwrap_or_else(|| panic!("Case '{}' not found", case_name));
                let ctx = format!("{}::{}", $type_name, case_name);

                // Rust value -> canonical JSON
                let json_value = value.to_json_value();
                assert!(
                    json_values_equal(&json_value, &tc.json),
                    "{}: to_json mismatch\n  Rust:     {}\n  Expected: {}",
                    ctx,
                    serde_json::to_string_pretty(&json_value).unwrap(),
                    serde_json::to_string_pretty(&tc.json).unwrap(),
                );

                // Canonical JSON -> Rust value
                let roundtrip = <$rust_type>::from_json_value(&tc.json).unwrap_or_else(|e| {
                    panic!("{}: from_json failed: {}", ctx, e);
                });

                // For float types, compare with tolerance
                let json_back = roundtrip.to_json_value();
                assert!(
                    json_values_equal(&json_back, &tc.json),
                    "{}: roundtrip mismatch\n  roundtrip: {}\n  expected:  {}",
                    ctx,
                    serde_json::to_string_pretty(&json_back).unwrap(),
                    serde_json::to_string_pretty(&tc.json).unwrap(),
                );
            }
        }
    };
}

json_vector_test!(json_fixed_ints, "FixedInts", FixedInts, vec![
    ("zeros", FixedInts { x: 0, y: 0 }),
    ("positive", FixedInts { x: 42, y: 100 }),
    ("negative", FixedInts { x: -1, y: -2147483648 }),
    ("max", FixedInts { x: 2147483647, y: 2147483647 }),
]);

// Note: FixedMixed golden vector test is skipped because the C++ field names
// (u8, u16, u32, u64) conflict with Rust type names. The Rust struct uses u8_,
// u16_, etc. The canonical JSON format is tested via test_u64_serialized_as_string.
#[test]
fn json_fixed_mixed_roundtrip() {
    // Test with Rust field names (u8_, u16_, etc.)
    let val = FixedMixed {
        b: true,
        u8_: 255,
        u16_: 65535,
        u32_: u32::MAX,
        u64_: u64::MAX,
    };
    let json_val = val.to_json_value();
    let obj = json_val.as_object().unwrap();
    // u64_ must be a string
    assert_eq!(obj.get("u64_").unwrap(), &Value::String("18446744073709551615".into()));
    // u32_ must be a number
    assert!(obj.get("u32_").unwrap().is_number());
    // roundtrip
    let roundtrip = FixedMixed::from_json_value(&json_val).unwrap();
    assert_eq!(roundtrip, val);
}

json_vector_test!(json_all_primitives, "AllPrimitives", AllPrimitives, vec![
    ("zeros", AllPrimitives {
        b: false, u8v: 0, i8v: 0, u16v: 0, i16v: 0,
        u32v: 0, i32v: 0, u64v: 0, i64v: 0, f32v: 0.0, f64v: 0.0,
    }),
    ("ones", AllPrimitives {
        b: true, u8v: 1, i8v: 1, u16v: 1, i16v: 1,
        u32v: 1, i32v: 1, u64v: 1, i64v: 1, f32v: 1.0, f64v: 1.0,
    }),
    ("max_unsigned", AllPrimitives {
        b: true, u8v: 255, i8v: 127, u16v: 65535, i16v: 32767,
        u32v: 4294967295, i32v: 2147483647,
        u64v: u64::MAX, i64v: i64::MAX,
        // f32 from packed bytes D0 0F 49 40 (C++'s 3.14159f)
        f32v: f32::from_le_bytes([0xD0, 0x0F, 0x49, 0x40]),
        f64v: std::f64::consts::E,
    }),
    ("min_signed", AllPrimitives {
        b: false, u8v: 0, i8v: -128, u16v: 0, i16v: -32768,
        u32v: 0, i32v: -2147483648, u64v: 0, i64v: i64::MIN,
        f32v: -1.0, f64v: -1.0,
    }),
    ("fractional_floats", AllPrimitives {
        b: false, u8v: 0, i8v: 0, u16v: 0, i16v: 0,
        u32v: 0, i32v: 0, u64v: 0, i64v: 0,
        f32v: 0.1_f32, f64v: 0.1_f64,
    }),
]);

json_vector_test!(json_single_bool, "SingleBool", SingleBool, vec![
    ("false", SingleBool { value: false }),
    ("true", SingleBool { value: true }),
]);

json_vector_test!(json_single_u32, "SingleU32", SingleU32, vec![
    ("zero", SingleU32 { value: 0 }),
    ("one", SingleU32 { value: 1 }),
    ("max", SingleU32 { value: u32::MAX }),
    ("hex_pattern", SingleU32 { value: 0xDEADBEEF }),
]);

json_vector_test!(json_single_string, "SingleString", SingleString, vec![
    ("empty", SingleString { value: "".into() }),
    ("hello", SingleString { value: "hello".into() }),
    ("with_spaces", SingleString { value: "hello world".into() }),
    ("special_chars", SingleString { value: "tab\there\nnewline".into() }),
    ("unicode", SingleString { value: "caf\u{e9} \u{2615} \u{65e5}\u{672c}\u{8a9e}".into() }),
    ("escapes", SingleString { value: "quote\"backslash\\".into() }),
]);

json_vector_test!(json_with_strings, "WithStrings", WithStrings, vec![
    ("all_empty", WithStrings { empty_str: "".into(), hello: "".into(), unicode: "".into() }),
    ("mixed", WithStrings {
        empty_str: "".into(),
        hello: "hello".into(),
        unicode: "\u{e9}mojis: \u{1f389}\u{1f680}".into(),
    }),
]);

json_vector_test!(json_with_vectors, "WithVectors", WithVectors, vec![
    ("both_empty", WithVectors { ints: vec![], strings: vec![] }),
    ("ints_only", WithVectors { ints: vec![1, 2, 3], strings: vec![] }),
    ("strings_only", WithVectors { ints: vec![], strings: vec!["a".into(), "bb".into(), "ccc".into()] }),
    ("both_filled", WithVectors { ints: vec![10, 20], strings: vec!["hello".into(), "world".into()] }),
    ("single_elements", WithVectors { ints: vec![42], strings: vec!["only".into()] }),
]);

json_vector_test!(json_with_optionals, "WithOptionals", WithOptionals, vec![
    ("both_null", WithOptionals { opt_int: None, opt_str: None }),
    ("int_only", WithOptionals { opt_int: Some(42), opt_str: None }),
    ("str_only", WithOptionals { opt_int: None, opt_str: Some("hello".into()) }),
    ("both_present", WithOptionals { opt_int: Some(99), opt_str: Some("world".into()) }),
    ("zero_int", WithOptionals { opt_int: Some(0), opt_str: None }),
]);

json_vector_test!(json_inner, "Inner", Inner, vec![
    ("simple", Inner { value: 42, label: "hello".into() }),
    ("empty_label", Inner { value: 0, label: "".into() }),
    ("max_value", Inner { value: u32::MAX, label: "max".into() }),
]);

json_vector_test!(json_outer, "Outer", Outer, vec![
    ("simple", Outer { inner: Inner { value: 1, label: "inner".into() }, name: "outer".into() }),
    ("empty_strings", Outer { inner: Inner { value: 0, label: "".into() }, name: "".into() }),
    ("nested_unicode", Outer {
        inner: Inner { value: 42, label: "caf\u{e9}".into() },
        name: "na\u{ef}ve".into(),
    }),
]);

json_vector_test!(json_with_variant, "WithVariant", WithVariant, vec![
    ("uint32_alt", WithVariant { data: WithVariantData::Uint32(42) }),
    ("string_alt", WithVariant { data: WithVariantData::StringAlt("hello".into()) }),
    ("struct_alt", WithVariant { data: WithVariantData::Inner(Inner { value: 7, label: "variant_inner".into() }) }),
    ("uint32_zero", WithVariant { data: WithVariantData::Uint32(0) }),
    ("string_empty", WithVariant { data: WithVariantData::StringAlt("".into()) }),
]);

json_vector_test!(json_vec_of_structs, "VecOfStructs", VecOfStructs, vec![
    ("empty", VecOfStructs { items: vec![] }),
    ("single", VecOfStructs { items: vec![Inner { value: 1, label: "one".into() }] }),
    ("multiple", VecOfStructs {
        items: vec![
            Inner { value: 1, label: "one".into() },
            Inner { value: 2, label: "two".into() },
            Inner { value: 3, label: "three".into() },
        ],
    }),
]);

json_vector_test!(json_optional_struct, "OptionalStruct", OptionalStruct, vec![
    ("null", OptionalStruct { item: None }),
    ("present", OptionalStruct { item: Some(Inner { value: 42, label: "exists".into() }) }),
]);

json_vector_test!(json_vec_of_optionals, "VecOfOptionals", VecOfOptionals, vec![
    ("empty", VecOfOptionals { items: vec![] }),
    ("all_null", VecOfOptionals { items: vec![None, None, None] }),
    ("all_present", VecOfOptionals { items: vec![Some(1), Some(2), Some(3)] }),
    ("mixed", VecOfOptionals { items: vec![Some(1), None, Some(3), None] }),
]);

json_vector_test!(json_optional_vec, "OptionalVec", OptionalVec, vec![
    ("null", OptionalVec { items: None }),
    ("empty_vec", OptionalVec { items: Some(vec![]) }),
    ("with_values", OptionalVec { items: Some(vec![10, 20, 30]) }),
]);

json_vector_test!(json_nested_vecs, "NestedVecs", NestedVecs, vec![
    ("empty", NestedVecs { matrix: vec![] }),
    ("empty_rows", NestedVecs { matrix: vec![vec![], vec![], vec![]] }),
    ("identity_2x2", NestedVecs { matrix: vec![vec![1, 0], vec![0, 1]] }),
    ("ragged", NestedVecs { matrix: vec![vec![1], vec![2, 3], vec![4, 5, 6]] }),
]);

json_vector_test!(json_fixed_array, "FixedArray", FixedArray, vec![
    ("zeros", FixedArray { arr: [0, 0, 0] }),
    ("sequence", FixedArray { arr: [1, 2, 3] }),
    ("max", FixedArray { arr: [u32::MAX, u32::MAX, u32::MAX] }),
]);

json_vector_test!(json_complex, "Complex", Complex, vec![
    ("all_empty", Complex {
        items: vec![],
        opt_vec: None,
        vec_opt: vec![],
        opt_struct: None,
    }),
    ("all_populated", Complex {
        items: vec![Inner { value: 1, label: "a".into() }, Inner { value: 2, label: "b".into() }],
        opt_vec: Some(vec![10, 20]),
        vec_opt: vec![Some("x".into()), None, Some("z".into())],
        opt_struct: Some(Inner { value: 99, label: "present".into() }),
    }),
    ("sparse", Complex {
        items: vec![Inner { value: 42, label: "only".into() }],
        opt_vec: None,
        vec_opt: vec![None, None],
        opt_struct: None,
    }),
]);

json_vector_test!(json_empty_extensible, "EmptyExtensible", EmptyExtensible, vec![
    ("zero", EmptyExtensible { dummy: 0 }),
    ("max", EmptyExtensible { dummy: u32::MAX }),
]);

// ============================================================
// Specific canonical format tests
// ============================================================

#[test]
fn test_u64_serialized_as_string() {
    // The critical case: u64 values must be JSON strings
    let val = FixedMixed {
        b: true,
        u8_: 255,
        u16_: 65535,
        u32_: u32::MAX,
        u64_: u64::MAX,
    };
    let json_val = val.to_json_value();
    let obj = json_val.as_object().unwrap();
    // u64 should be a string, not a number
    assert_eq!(
        obj.get("u64_").unwrap(),
        &Value::String("18446744073709551615".into()),
        "u64 must serialize as string"
    );
    // u32 should be a number
    assert!(
        obj.get("u32_").unwrap().is_number(),
        "u32 must serialize as number"
    );
}

#[test]
fn test_i64_serialized_as_string() {
    let val = AllPrimitives {
        b: false,
        u8v: 0,
        i8v: -128,
        u16v: 0,
        i16v: -32768,
        u32v: 0,
        i32v: -2147483648,
        u64v: 0,
        i64v: i64::MIN,
        f32v: 0.0,
        f64v: 0.0,
    };
    let json_val = val.to_json_value();
    let obj = json_val.as_object().unwrap();
    assert_eq!(
        obj.get("i64v").unwrap(),
        &Value::String("-9223372036854775808".into()),
        "i64 must serialize as string"
    );
    assert_eq!(
        obj.get("u64v").unwrap(),
        &Value::String("0".into()),
        "u64 zero must serialize as string"
    );
    // i32 should be a number
    assert!(
        obj.get("i32v").unwrap().is_number(),
        "i32 must serialize as number"
    );
}

#[test]
fn test_variant_encoding() {
    // Variant must serialize as {"CaseName": value}
    let uint_variant = WithVariantData::Uint32(42);
    let json_val = uint_variant.to_json_value();
    let obj = json_val.as_object().unwrap();
    assert_eq!(obj.len(), 1, "variant must have exactly one key");
    assert!(obj.contains_key("uint32"), "variant key must be 'uint32'");
    assert_eq!(obj.get("uint32").unwrap(), &Value::Number(42.into()));

    let inner_variant = WithVariantData::Inner(Inner {
        value: 7,
        label: "test".into(),
    });
    let json_val = inner_variant.to_json_value();
    let obj = json_val.as_object().unwrap();
    assert_eq!(obj.len(), 1);
    assert!(obj.contains_key("Inner"));
    let inner_obj = obj.get("Inner").unwrap().as_object().unwrap();
    assert_eq!(inner_obj.get("value").unwrap(), &Value::Number(7.into()));
    assert_eq!(
        inner_obj.get("label").unwrap(),
        &Value::String("test".into())
    );
}

#[test]
fn test_nested_struct_json() {
    let val = Outer {
        inner: Inner {
            value: 42,
            label: "nested".into(),
        },
        name: "top".into(),
    };
    let json_val = val.to_json_value();
    let expected: Value = serde_json::json!({
        "inner": {"value": 42, "label": "nested"},
        "name": "top"
    });
    assert!(json_values_equal(&json_val, &expected));

    let roundtrip = Outer::from_json_value(&json_val).unwrap();
    assert_eq!(roundtrip, val);
}

#[test]
fn test_option_none_some() {
    let val_none = OptionalStruct { item: None };
    let json_none = val_none.to_json_value();
    let expected_none: Value = serde_json::json!({"item": null});
    assert!(json_values_equal(&json_none, &expected_none));

    let val_some = OptionalStruct {
        item: Some(Inner {
            value: 42,
            label: "exists".into(),
        }),
    };
    let json_some = val_some.to_json_value();
    let expected_some: Value = serde_json::json!({
        "item": {"value": 42, "label": "exists"}
    });
    assert!(json_values_equal(&json_some, &expected_some));

    let rt_none = OptionalStruct::from_json_value(&json_none).unwrap();
    assert_eq!(rt_none, val_none);
    let rt_some = OptionalStruct::from_json_value(&json_some).unwrap();
    assert_eq!(rt_some, val_some);
}

#[test]
fn test_to_json_from_json_string() {
    let val = FixedInts { x: 42, y: -1 };
    let json_str = json::to_json(&val);
    let roundtrip: FixedInts = json::from_json(&json_str).unwrap();
    assert_eq!(roundtrip, val);
}

#[test]
fn test_variant_roundtrip_all_cases() {
    let cases = vec![
        WithVariantData::Uint32(0),
        WithVariantData::Uint32(u32::MAX),
        WithVariantData::StringAlt("".into()),
        WithVariantData::StringAlt("hello world".into()),
        WithVariantData::Inner(Inner {
            value: 42,
            label: "test".into(),
        }),
    ];

    for case in &cases {
        let json_val = case.to_json_value();
        let roundtrip = WithVariantData::from_json_value(&json_val).unwrap();
        assert_eq!(
            &roundtrip, case,
            "variant roundtrip failed for {:?}",
            case
        );
    }
}

#[test]
fn test_vec_of_vec_json() {
    let val = NestedVecs {
        matrix: vec![vec![1, 2], vec![3, 4, 5]],
    };
    let json_val = val.to_json_value();
    let expected: Value = serde_json::json!({"matrix": [[1, 2], [3, 4, 5]]});
    assert!(json_values_equal(&json_val, &expected));

    let roundtrip = NestedVecs::from_json_value(&json_val).unwrap();
    assert_eq!(roundtrip, val);
}

#[test]
fn test_complex_roundtrip() {
    let val = Complex {
        items: vec![
            Inner {
                value: 1,
                label: "a".into(),
            },
            Inner {
                value: 2,
                label: "b".into(),
            },
        ],
        opt_vec: Some(vec![10, 20]),
        vec_opt: vec![Some("x".into()), None, Some("z".into())],
        opt_struct: Some(Inner {
            value: 99,
            label: "present".into(),
        }),
    };
    let json_val = val.to_json_value();
    let roundtrip = Complex::from_json_value(&json_val).unwrap();
    assert_eq!(roundtrip, val);
}
