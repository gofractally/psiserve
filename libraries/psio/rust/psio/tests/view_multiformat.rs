//! Integration tests for zero-copy View derive macros: CapnpView, FbView, WitView.
//!
//! Tests that derived view structs read fields directly from serialized bytes
//! without deserialization.

use psio::capnp::pack::CapnpPack;
use psio::flatbuf::builder::FbPack;
use psio::wit::pack::WitPack;

// ── Test struct: scalars only ─────────────────────────────────────────

#[derive(Debug, Clone, PartialEq, psio::CapnpPack, psio::CapnpView, psio::FbPack, psio::FbView, psio::WitPack, psio::WitView)]
#[fracpack(fracpack_mod = "psio")]
struct Point {
    x: i32,
    y: i32,
}

#[test]
fn capnp_view_point() {
    let val = Point { x: 42, y: -7 };
    let msg = val.capnp_pack();
    let view = PointCapnpView::from_message(&msg).unwrap();
    assert_eq!(view.x(), 42);
    assert_eq!(view.y(), -7);
}

#[test]
fn fb_view_point() {
    let val = Point { x: 42, y: -7 };
    let data = val.fb_pack();
    let view = PointFbView::from_buffer(&data).unwrap();
    assert_eq!(view.x(), 42);
    assert_eq!(view.y(), -7);
}

#[test]
fn wit_view_point() {
    let val = Point { x: 42, y: -7 };
    let buf = val.wit_pack();
    let view = PointWitView::from_buffer(&buf);
    assert_eq!(view.x(), 42);
    assert_eq!(view.y(), -7);
}

// ── Test struct: all scalar types ─────────────────────────────────────

#[derive(Debug, Clone, PartialEq, psio::CapnpPack, psio::CapnpView, psio::FbPack, psio::FbView, psio::WitPack, psio::WitView)]
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

fn make_all_scalars() -> AllScalars {
    AllScalars {
        a_bool: true,
        a_u8: 255,
        a_i8: -128,
        a_u16: 65535,
        a_i16: -32768,
        a_u32: 0xDEAD_BEEF,
        a_i32: -12345,
        a_u64: 0xCAFE_BABE_DEAD_BEEF,
        a_i64: -9876543210,
        a_f32: 1.5,
        a_f64: std::f64::consts::PI,
    }
}

#[test]
fn capnp_view_all_scalars() {
    let val = make_all_scalars();
    let msg = val.capnp_pack();
    let view = AllScalarsCapnpView::from_message(&msg).unwrap();
    assert_eq!(view.a_bool(), true);
    assert_eq!(view.a_u8(), 255);
    assert_eq!(view.a_i8(), -128);
    assert_eq!(view.a_u16(), 65535);
    assert_eq!(view.a_i16(), -32768);
    assert_eq!(view.a_u32(), 0xDEAD_BEEF);
    assert_eq!(view.a_i32(), -12345);
    assert_eq!(view.a_u64(), 0xCAFE_BABE_DEAD_BEEF);
    assert_eq!(view.a_i64(), -9876543210);
    assert_eq!(view.a_f32(), 1.5);
    assert_eq!(view.a_f64(), std::f64::consts::PI);
}

#[test]
fn fb_view_all_scalars() {
    let val = make_all_scalars();
    let data = val.fb_pack();
    let view = AllScalarsFbView::from_buffer(&data).unwrap();
    assert_eq!(view.a_bool(), true);
    assert_eq!(view.a_u8(), 255);
    assert_eq!(view.a_i8(), -128);
    assert_eq!(view.a_u16(), 65535);
    assert_eq!(view.a_i16(), -32768);
    assert_eq!(view.a_u32(), 0xDEAD_BEEF);
    assert_eq!(view.a_i32(), -12345);
    assert_eq!(view.a_u64(), 0xCAFE_BABE_DEAD_BEEF);
    assert_eq!(view.a_i64(), -9876543210);
    assert_eq!(view.a_f32(), 1.5);
    assert_eq!(view.a_f64(), std::f64::consts::PI);
}

#[test]
fn wit_view_all_scalars() {
    let val = make_all_scalars();
    let buf = val.wit_pack();
    let view = AllScalarsWitView::from_buffer(&buf);
    assert_eq!(view.a_bool(), true);
    assert_eq!(view.a_u8(), 255);
    assert_eq!(view.a_i8(), -128);
    assert_eq!(view.a_u16(), 65535);
    assert_eq!(view.a_i16(), -32768);
    assert_eq!(view.a_u32(), 0xDEAD_BEEF);
    assert_eq!(view.a_i32(), -12345);
    assert_eq!(view.a_u64(), 0xCAFE_BABE_DEAD_BEEF);
    assert_eq!(view.a_i64(), -9876543210);
    assert_eq!(view.a_f32(), 1.5);
    assert_eq!(view.a_f64(), std::f64::consts::PI);
}

// ── Test struct: with string ──────────────────────────────────────────

#[derive(Debug, Clone, PartialEq, psio::CapnpPack, psio::CapnpView, psio::FbPack, psio::FbView, psio::WitPack, psio::WitView)]
#[fracpack(fracpack_mod = "psio")]
struct WithString {
    id: u64,
    name: String,
}

#[test]
fn capnp_view_string() {
    let val = WithString { id: 99, name: "hello capnp".into() };
    let msg = val.capnp_pack();
    let view = WithStringCapnpView::from_message(&msg).unwrap();
    assert_eq!(view.id(), 99);
    assert_eq!(view.name(), "hello capnp");
}

#[test]
fn fb_view_string() {
    let val = WithString { id: 99, name: "hello flatbuf".into() };
    let data = val.fb_pack();
    let view = WithStringFbView::from_buffer(&data).unwrap();
    assert_eq!(view.id(), 99);
    assert_eq!(view.name(), "hello flatbuf");
}

#[test]
fn wit_view_string() {
    let val = WithString { id: 99, name: "hello wit".into() };
    let buf = val.wit_pack();
    let view = WithStringWitView::from_buffer(&buf);
    assert_eq!(view.id(), 99);
    assert_eq!(view.name().unwrap(), "hello wit");
}

// ── Test: empty string ────────────────────────────────────────────────

#[test]
fn capnp_view_empty_string() {
    let val = WithString { id: 0, name: String::new() };
    let msg = val.capnp_pack();
    let view = WithStringCapnpView::from_message(&msg).unwrap();
    assert_eq!(view.id(), 0);
    assert_eq!(view.name(), "");
}

#[test]
fn fb_view_empty_string() {
    let val = WithString { id: 0, name: String::new() };
    let data = val.fb_pack();
    let view = WithStringFbView::from_buffer(&data).unwrap();
    assert_eq!(view.id(), 0);
    assert_eq!(view.name(), "");
}

#[test]
fn wit_view_empty_string() {
    let val = WithString { id: 0, name: String::new() };
    let buf = val.wit_pack();
    let view = WithStringWitView::from_buffer(&buf);
    assert_eq!(view.id(), 0);
    assert_eq!(view.name().unwrap(), "");
}

// ── Test: zero values ─────────────────────────────────────────────────

#[test]
fn capnp_view_zero_scalars() {
    let val = Point { x: 0, y: 0 };
    let msg = val.capnp_pack();
    let view = PointCapnpView::from_message(&msg).unwrap();
    assert_eq!(view.x(), 0);
    assert_eq!(view.y(), 0);
}

#[test]
fn fb_view_zero_scalars() {
    let val = Point { x: 0, y: 0 };
    let data = val.fb_pack();
    let view = PointFbView::from_buffer(&data).unwrap();
    assert_eq!(view.x(), 0);
    assert_eq!(view.y(), 0);
}

#[test]
fn wit_view_zero_scalars() {
    let val = Point { x: 0, y: 0 };
    let buf = val.wit_pack();
    let view = PointWitView::from_buffer(&buf);
    assert_eq!(view.x(), 0);
    assert_eq!(view.y(), 0);
}
