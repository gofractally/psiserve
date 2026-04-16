//! Integration tests for multi-format derive macros: CapnpPack, CapnpUnpack,
//! FbPack, FbUnpack, WitPack, WitUnpack.
//!
//! Tests round-trip serialization through each format and cross-format
//! consistency (same struct values survive capnp -> unpack -> fb -> unpack).

use psio::capnp::pack::CapnpPack;
use psio::capnp::unpack::CapnpUnpack;
use psio::flatbuf::builder::FbPack;
use psio::flatbuf::unpack::FbUnpack;
use psio::wit::layout::WitLayout;
use psio::wit::pack::WitPack;
use psio::wit::view::WitUnpack;

// ── Test struct: scalars only ───────────────────────────────────────────

#[derive(Debug, Clone, PartialEq, psio::CapnpPack, psio::CapnpUnpack, psio::FbPack, psio::FbUnpack, psio::WitPack, psio::WitUnpack)]
#[fracpack(fracpack_mod = "psio")]
struct ScalarsOnly {
    a: u32,
    b: u64,
    c: bool,
    d: i16,
    e: f32,
}

#[test]
fn capnp_roundtrip_scalars() {
    let val = ScalarsOnly { a: 42, b: 0xDEAD_BEEF, c: true, d: -100, e: 3.14 };
    let msg = val.capnp_pack();
    let restored = ScalarsOnly::capnp_unpack(&msg).unwrap();
    assert_eq!(restored.a, 42);
    assert_eq!(restored.b, 0xDEAD_BEEF);
    assert_eq!(restored.c, true);
    assert_eq!(restored.d, -100);
    assert_eq!(restored.e, 3.14f32);
}

#[test]
fn fb_roundtrip_scalars() {
    let val = ScalarsOnly { a: 42, b: 0xDEAD_BEEF, c: true, d: -100, e: 3.14 };
    let data = val.fb_pack();
    let restored = ScalarsOnly::fb_unpack(&data).unwrap();
    assert_eq!(restored.a, 42);
    assert_eq!(restored.b, 0xDEAD_BEEF);
    assert_eq!(restored.c, true);
    assert_eq!(restored.d, -100);
    assert_eq!(restored.e, 3.14f32);
}

#[test]
fn wit_roundtrip_scalars() {
    let val = ScalarsOnly { a: 42, b: 0xDEAD_BEEF, c: true, d: -100, e: 3.14 };
    let buf = val.wit_pack();
    let restored = ScalarsOnly::wit_unpack(&buf).unwrap();
    assert_eq!(restored.a, 42);
    assert_eq!(restored.b, 0xDEAD_BEEF);
    assert_eq!(restored.c, true);
    assert_eq!(restored.d, -100);
    assert_eq!(restored.e, 3.14f32);
}

// ── Test struct: with string and vec ────────────────────────────────────

#[derive(Debug, Clone, PartialEq, psio::CapnpPack, psio::CapnpUnpack, psio::FbPack, psio::FbUnpack, psio::WitPack, psio::WitUnpack)]
#[fracpack(fracpack_mod = "psio")]
struct WithStringsAndVecs {
    id: u64,
    name: String,
    items: Vec<u32>,
}

#[test]
fn capnp_roundtrip_strings_vecs() {
    let val = WithStringsAndVecs {
        id: 12345,
        name: "hello capnp".to_string(),
        items: vec![10, 20, 30, 40],
    };
    let msg = val.capnp_pack();
    let restored = WithStringsAndVecs::capnp_unpack(&msg).unwrap();
    assert_eq!(restored.id, 12345);
    assert_eq!(restored.name, "hello capnp");
    assert_eq!(restored.items, vec![10, 20, 30, 40]);
}

#[test]
fn fb_roundtrip_strings_vecs() {
    let val = WithStringsAndVecs {
        id: 12345,
        name: "hello flatbuf".to_string(),
        items: vec![10, 20, 30, 40],
    };
    let data = val.fb_pack();
    let restored = WithStringsAndVecs::fb_unpack(&data).unwrap();
    assert_eq!(restored.id, 12345);
    assert_eq!(restored.name, "hello flatbuf");
    assert_eq!(restored.items, vec![10, 20, 30, 40]);
}

#[test]
fn wit_roundtrip_strings_vecs() {
    let val = WithStringsAndVecs {
        id: 12345,
        name: "hello wit".to_string(),
        items: vec![10, 20, 30, 40],
    };
    let buf = val.wit_pack();
    let restored = WithStringsAndVecs::wit_unpack(&buf).unwrap();
    assert_eq!(restored.id, 12345);
    assert_eq!(restored.name, "hello wit");
    assert_eq!(restored.items, vec![10, 20, 30, 40]);
}

// ── Test: empty strings and vecs ────────────────────────────────────────

#[test]
fn capnp_roundtrip_empty() {
    let val = WithStringsAndVecs { id: 0, name: String::new(), items: vec![] };
    let msg = val.capnp_pack();
    let restored = WithStringsAndVecs::capnp_unpack(&msg).unwrap();
    assert_eq!(restored.id, 0);
    assert_eq!(restored.name, "");
    assert_eq!(restored.items, Vec::<u32>::new());
}

#[test]
fn fb_roundtrip_empty() {
    let val = WithStringsAndVecs { id: 0, name: String::new(), items: vec![] };
    let data = val.fb_pack();
    let restored = WithStringsAndVecs::fb_unpack(&data).unwrap();
    assert_eq!(restored.id, 0);
    assert_eq!(restored.name, "");
    assert_eq!(restored.items, Vec::<u32>::new());
}

#[test]
fn wit_roundtrip_empty() {
    let val = WithStringsAndVecs { id: 0, name: String::new(), items: vec![] };
    let buf = val.wit_pack();
    let restored = WithStringsAndVecs::wit_unpack(&buf).unwrap();
    assert_eq!(restored.id, 0);
    assert_eq!(restored.name, "");
    assert_eq!(restored.items, Vec::<u32>::new());
}

// ── Cross-format consistency ────────────────────────────────────────────

#[test]
fn cross_format_consistency() {
    let original = WithStringsAndVecs {
        id: 9999,
        name: "cross-format".to_string(),
        items: vec![1, 2, 3],
    };

    // capnp round-trip
    let capnp_bytes = original.capnp_pack();
    let from_capnp = WithStringsAndVecs::capnp_unpack(&capnp_bytes).unwrap();
    assert_eq!(from_capnp, original);

    // fb round-trip from capnp result
    let fb_bytes = from_capnp.fb_pack();
    let from_fb = WithStringsAndVecs::fb_unpack(&fb_bytes).unwrap();
    assert_eq!(from_fb, original);

    // wit round-trip from fb result
    let wit_bytes = from_fb.wit_pack();
    let from_wit = WithStringsAndVecs::wit_unpack(&wit_bytes).unwrap();
    assert_eq!(from_wit, original);
}

// ── Test struct: all scalar types ───────────────────────────────────────

#[derive(Debug, Clone, PartialEq, psio::CapnpPack, psio::CapnpUnpack, psio::FbPack, psio::FbUnpack, psio::WitPack, psio::WitUnpack)]
#[fracpack(fracpack_mod = "psio")]
struct AllScalars {
    a_bool: bool,
    a_u8: u8,
    a_i8: i8,
    a_u16: u16,
    a_i16: i16,
    a_u32: u32,
    a_i32: i32,
    a_u64: u64,
    a_i64: i64,
    a_f32: f32,
    a_f64: f64,
}

#[test]
fn all_scalars_capnp_roundtrip() {
    let val = AllScalars {
        a_bool: true, a_u8: 255, a_i8: -128, a_u16: 65535, a_i16: -32768,
        a_u32: 0xDEAD_BEEF, a_i32: -12345, a_u64: 0xCAFE_BABE_DEAD_BEEF,
        a_i64: -9876543210, a_f32: 1.5, a_f64: std::f64::consts::PI,
    };
    let msg = val.capnp_pack();
    let restored = AllScalars::capnp_unpack(&msg).unwrap();
    assert_eq!(restored, val);
}

#[test]
fn all_scalars_fb_roundtrip() {
    let val = AllScalars {
        a_bool: true, a_u8: 255, a_i8: -128, a_u16: 65535, a_i16: -32768,
        a_u32: 0xDEAD_BEEF, a_i32: -12345, a_u64: 0xCAFE_BABE_DEAD_BEEF,
        a_i64: -9876543210, a_f32: 1.5, a_f64: std::f64::consts::PI,
    };
    let data = val.fb_pack();
    let restored = AllScalars::fb_unpack(&data).unwrap();
    assert_eq!(restored, val);
}

#[test]
fn all_scalars_wit_roundtrip() {
    let val = AllScalars {
        a_bool: true, a_u8: 255, a_i8: -128, a_u16: 65535, a_i16: -32768,
        a_u32: 0xDEAD_BEEF, a_i32: -12345, a_u64: 0xCAFE_BABE_DEAD_BEEF,
        a_i64: -9876543210, a_f32: 1.5, a_f64: std::f64::consts::PI,
    };
    let buf = val.wit_pack();
    let restored = AllScalars::wit_unpack(&buf).unwrap();
    assert_eq!(restored, val);
}

// ── WIT layout sanity ───────────────────────────────────────────────────

#[test]
fn wit_layout_sanity() {
    // ScalarsOnly: u32(4,4) u64(8,8) bool(1,1) i16(2,2) f32(4,4)
    // Expected: alignment 8, size = align_up(4+pad4+8+1+pad1+2+pad2+4, 8)
    // Actually computed by compute_struct_layout
    assert!(ScalarsOnly::wit_alignment() >= 8);
    assert!(ScalarsOnly::wit_size() >= 20);
}

// ── Test: Vec<bool> ─────────────────────────────────────────────────────

#[derive(Debug, Clone, PartialEq, psio::CapnpPack, psio::CapnpUnpack)]
#[fracpack(fracpack_mod = "psio")]
struct WithBoolVec {
    flags: Vec<bool>,
}

#[test]
fn capnp_bool_vec_roundtrip() {
    let val = WithBoolVec { flags: vec![true, false, true, true, false] };
    let msg = val.capnp_pack();
    let restored = WithBoolVec::capnp_unpack(&msg).unwrap();
    assert_eq!(restored.flags, vec![true, false, true, true, false]);
}

// ── Test: Vec<String> ───────────────────────────────────────────────────

#[derive(Debug, Clone, PartialEq, psio::CapnpPack, psio::CapnpUnpack)]
#[fracpack(fracpack_mod = "psio")]
struct WithStringVec {
    names: Vec<String>,
}

#[test]
fn capnp_string_vec_roundtrip() {
    let val = WithStringVec { names: vec!["alpha".into(), "beta".into(), "gamma".into()] };
    let msg = val.capnp_pack();
    let restored = WithStringVec::capnp_unpack(&msg).unwrap();
    assert_eq!(restored.names, vec!["alpha", "beta", "gamma"]);
}

// ── FlatBuffers default value behavior ──────────────────────────────────

#[test]
fn fb_zero_values_roundtrip() {
    // FlatBuffers omits fields equal to default (0), so verify they come back as 0
    let val = ScalarsOnly { a: 0, b: 0, c: false, d: 0, e: 0.0 };
    let data = val.fb_pack();
    let restored = ScalarsOnly::fb_unpack(&data).unwrap();
    assert_eq!(restored, val);
}

// ── Large data round-trip ───────────────────────────────────────────────

#[test]
fn large_vec_roundtrip_all_formats() {
    let items: Vec<u32> = (0..1000).collect();
    let val = WithStringsAndVecs {
        id: u64::MAX,
        name: "x".repeat(500),
        items: items.clone(),
    };

    // capnp
    let msg = val.capnp_pack();
    let r = WithStringsAndVecs::capnp_unpack(&msg).unwrap();
    assert_eq!(r.items.len(), 1000);
    assert_eq!(r.name.len(), 500);
    assert_eq!(r, val);

    // flatbuf
    let data = val.fb_pack();
    let r = WithStringsAndVecs::fb_unpack(&data).unwrap();
    assert_eq!(r, val);

    // wit
    let buf = val.wit_pack();
    let r = WithStringsAndVecs::wit_unpack(&buf).unwrap();
    assert_eq!(r, val);
}
