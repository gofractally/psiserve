use psio1::{FracMutView, FracMutViewType, FracView, FracViewType, Pack, Unpack};

#[derive(Pack, Unpack, FracView, FracMutView, PartialEq, Debug)]
#[fracpack(fracpack_mod = "psio")]
#[fracpack(definition_will_not_change)]
struct FixedOnly {
    a: u32,
    b: u64,
    c: i16,
}

#[derive(Pack, Unpack, FracView, FracMutView, PartialEq, Debug)]
#[fracpack(fracpack_mod = "psio")]
#[fracpack(definition_will_not_change)]
struct FixedWithString {
    x: u32,
    s: String,
    y: f64,
}

#[derive(Pack, Unpack, FracView, FracMutView, PartialEq, Debug)]
#[fracpack(fracpack_mod = "psio")]
struct Extensible {
    a: u32,
    b: String,
    opt_c: Option<u32>,
    opt_d: Option<String>,
}

#[derive(Pack, Unpack, FracView, FracMutView, PartialEq, Debug)]
#[fracpack(fracpack_mod = "psio")]
struct Nested {
    inner: Extensible,
    label: String,
}

#[derive(Pack, Unpack, FracView, FracMutView, PartialEq, Eq, Debug)]
#[fracpack(fracpack_mod = "psio")]
enum Color {
    Red(u32),
    Blue(String),
}

#[derive(Pack, Unpack, FracView, FracMutView, PartialEq, Debug)]
#[fracpack(fracpack_mod = "psio")]
struct WithEnum {
    id: u32,
    color: Color,
}

#[derive(Pack, Unpack, FracView, FracMutView, PartialEq, Debug)]
#[fracpack(fracpack_mod = "psio")]
struct WithVec {
    name: String,
    values: Vec<u32>,
}

// ── Canonical mode tests (splice-and-patch, immediately canonical) ──

#[test]
fn canonical_set_fixed_fields() {
    let orig = FixedOnly {
        a: 10,
        b: 20,
        c: -5,
    };
    let mut data = orig.packed();
    let m = FixedOnlyMutCanonical::new(&data).unwrap();

    m.set_a(&mut data, &42);
    m.set_b(&mut data, &999);
    m.set_c(&mut data, &-100);

    let result = FixedOnly::unpacked(&data).unwrap();
    assert_eq!(
        result,
        FixedOnly {
            a: 42,
            b: 999,
            c: -100
        }
    );
}

#[test]
fn canonical_set_string_grow() {
    let orig = FixedWithString {
        x: 1,
        s: "hi".to_string(),
        y: 2.718,
    };
    let mut data = orig.packed();
    let m = FixedWithStringMutCanonical::new(&data).unwrap();

    m.set_s(&mut data, &"hello world".to_string());

    // Immediately canonical — unpacked works directly
    let result = FixedWithString::unpacked(&data).unwrap();
    assert_eq!(result.x, 1);
    assert_eq!(result.s, "hello world");
    assert_eq!(result.y, 2.718);
}

#[test]
fn canonical_set_string_shrink() {
    let orig = FixedWithString {
        x: 1,
        s: "hello world".to_string(),
        y: 2.718,
    };
    let mut data = orig.packed();
    let m = FixedWithStringMutCanonical::new(&data).unwrap();

    m.set_s(&mut data, &"hi".to_string());

    let result = FixedWithString::unpacked(&data).unwrap();
    assert_eq!(result.x, 1);
    assert_eq!(result.s, "hi");
    assert_eq!(result.y, 2.718);
}

#[test]
fn canonical_extensible_set_string() {
    let orig = Extensible {
        a: 10,
        b: "short".to_string(),
        opt_c: Some(99),
        opt_d: Some("present".to_string()),
    };
    let mut data = orig.packed();
    let m = ExtensibleMutCanonical::new(&data).unwrap();

    m.set_b(&mut data, &"a much longer string value".to_string());

    let result = Extensible::unpacked(&data).unwrap();
    assert_eq!(result.a, 10);
    assert_eq!(result.b, "a much longer string value");
    assert_eq!(result.opt_c, Some(99));
    assert_eq!(result.opt_d, Some("present".to_string()));
}

#[test]
fn canonical_option_some_to_some() {
    let orig = Extensible {
        a: 1,
        b: "x".to_string(),
        opt_c: Some(10),
        opt_d: Some("old".to_string()),
    };
    let mut data = orig.packed();
    let m = ExtensibleMutCanonical::new(&data).unwrap();

    m.set_opt_c(&mut data, &Some(42));
    m.set_opt_d(&mut data, &Some("new value".to_string()));

    let result = Extensible::unpacked(&data).unwrap();
    assert_eq!(result.opt_c, Some(42));
    assert_eq!(result.opt_d, Some("new value".to_string()));
}

// ── Fast mode tests (overwrite/append, non-canonical) ──

#[test]
fn fast_set_fixed_fields() {
    let orig = FixedOnly {
        a: 10,
        b: 20,
        c: -5,
    };
    let mut data = orig.packed();
    let m = FixedOnlyMut::new(&data).unwrap();

    m.set_a(&mut data, &42);
    m.set_b(&mut data, &999);
    m.set_c(&mut data, &-100);

    // Fixed-size mutations are always canonical
    let result = FixedOnly::unpacked(&data).unwrap();
    assert_eq!(
        result,
        FixedOnly {
            a: 42,
            b: 999,
            c: -100
        }
    );
}

#[test]
fn fast_set_string_same_size() {
    let orig = FixedWithString {
        x: 1,
        s: "hello".to_string(),
        y: 3.14,
    };
    let mut data = orig.packed();
    let m = FixedWithStringMut::new(&data).unwrap();

    m.set_s(&mut data, &"world".to_string());

    // Same size: overwritten in place, still canonical
    let result = FixedWithString::unpacked(&data).unwrap();
    assert_eq!(result.x, 1);
    assert_eq!(result.s, "world");
    assert_eq!(result.y, 3.14);
}

#[test]
fn fast_set_string_shrink() {
    let orig = FixedWithString {
        x: 1,
        s: "hello world".to_string(),
        y: 2.718,
    };
    let mut data = orig.packed();
    let original_len = data.len();
    let m = FixedWithStringMut::new(&data).unwrap();

    m.set_s(&mut data, &"hi".to_string());

    // Buffer size unchanged (dead bytes left in place)
    assert_eq!(data.len(), original_len);

    // Getters read correctly from non-canonical data
    assert_eq!(m.x(&data), 1);
    assert_eq!(m.s(&data), "hi");
    assert_eq!(m.y(&data), 2.718);

    // compact restores canonical form
    FixedWithStringMut::compact(&mut data);
    let result = FixedWithString::unpacked(&data).unwrap();
    assert_eq!(result.s, "hi");
    assert_eq!(result.y, 2.718);
}

#[test]
fn fast_set_string_grow() {
    let orig = FixedWithString {
        x: 1,
        s: "hi".to_string(),
        y: 2.718,
    };
    let mut data = orig.packed();
    let original_len = data.len();
    let m = FixedWithStringMut::new(&data).unwrap();

    m.set_s(&mut data, &"hello world".to_string());

    // Buffer grew (new data appended to end)
    assert!(data.len() > original_len);

    // Getters read correctly from non-canonical data
    assert_eq!(m.x(&data), 1);
    assert_eq!(m.s(&data), "hello world");
    assert_eq!(m.y(&data), 2.718);

    // compact restores canonical form
    FixedWithStringMut::compact(&mut data);
    let result = FixedWithString::unpacked(&data).unwrap();
    assert_eq!(result.s, "hello world");
    assert_eq!(result.y, 2.718);
}

#[test]
fn fast_extensible_set_fixed_field() {
    let orig = Extensible {
        a: 10,
        b: "test".to_string(),
        opt_c: Some(99),
        opt_d: Some("present".to_string()),
    };
    let mut data = orig.packed();
    let m = ExtensibleMut::new(&data).unwrap();

    m.set_a(&mut data, &42);

    assert_eq!(m.a(&data), 42);
    assert_eq!(m.b(&data), "test");
    assert_eq!(m.opt_c(&data), Some(99));
    assert_eq!(m.opt_d(&data), Some("present"));
}

#[test]
fn fast_extensible_set_string_field() {
    let orig = Extensible {
        a: 10,
        b: "short".to_string(),
        opt_c: Some(99),
        opt_d: Some("present".to_string()),
    };
    let mut data = orig.packed();
    let m = ExtensibleMut::new(&data).unwrap();

    m.set_b(&mut data, &"a much longer string value".to_string());

    assert_eq!(m.a(&data), 10);
    assert_eq!(m.b(&data), "a much longer string value");
    assert_eq!(m.opt_c(&data), Some(99));
    assert_eq!(m.opt_d(&data), Some("present"));

    ExtensibleMut::compact(&mut data);
    let result = Extensible::unpacked(&data).unwrap();
    assert_eq!(result.b, "a much longer string value");
}

#[test]
fn fast_option_some_to_some() {
    let orig = Extensible {
        a: 1,
        b: "x".to_string(),
        opt_c: Some(10),
        opt_d: Some("old".to_string()),
    };
    let mut data = orig.packed();
    let m = ExtensibleMut::new(&data).unwrap();

    m.set_opt_c(&mut data, &Some(42));
    m.set_opt_d(&mut data, &Some("new value".to_string()));

    assert_eq!(m.opt_c(&data), Some(42));
    assert_eq!(m.opt_d(&data), Some("new value"));

    ExtensibleMut::compact(&mut data);
    let result = Extensible::unpacked(&data).unwrap();
    assert_eq!(result.opt_c, Some(42));
    assert_eq!(result.opt_d, Some("new value".to_string()));
}

#[test]
fn fast_option_some_to_none() {
    let orig = Extensible {
        a: 1,
        b: "x".to_string(),
        opt_c: Some(10),
        opt_d: Some("old".to_string()),
    };
    let mut data = orig.packed();
    let m = ExtensibleMut::new(&data).unwrap();

    m.set_opt_d(&mut data, &None);

    // Verify via getters (non-canonical data)
    assert_eq!(m.a(&data), 1);
    assert_eq!(m.b(&data), "x");
    assert_eq!(m.opt_c(&data), Some(10));
    assert_eq!(m.opt_d(&data), None);
}

#[test]
fn fast_option_none_to_some() {
    // Fast mode supports None → Some when the field is not elided
    // (a later field has data, so this None has an offset slot in the buffer)
    let orig = Extensible {
        a: 1,
        b: "x".to_string(),
        opt_c: None,
        opt_d: Some("present".to_string()),
    };
    let mut data = orig.packed();
    let m = ExtensibleMut::new(&data).unwrap();

    m.set_opt_c(&mut data, &Some(42));

    assert_eq!(m.opt_c(&data), Some(42));
    assert_eq!(m.opt_d(&data), Some("present"));

    ExtensibleMut::compact(&mut data);
    let result = Extensible::unpacked(&data).unwrap();
    assert_eq!(result.opt_c, Some(42));
    assert_eq!(result.opt_d, Some("present".to_string()));
}

#[test]
fn fast_option_set_middle_to_none() {
    let orig = Extensible {
        a: 1,
        b: "x".to_string(),
        opt_c: Some(10),
        opt_d: Some("present".to_string()),
    };
    let mut data = orig.packed();
    let m = ExtensibleMut::new(&data).unwrap();

    m.set_opt_c(&mut data, &None);

    assert_eq!(m.opt_c(&data), None);
    assert_eq!(m.opt_d(&data), Some("present"));

    ExtensibleMut::compact(&mut data);
    let result = Extensible::unpacked(&data).unwrap();
    assert_eq!(result.opt_c, None);
    assert_eq!(result.opt_d, Some("present".to_string()));
}

#[test]
fn fast_mut_getters_work() {
    let orig = Extensible {
        a: 10,
        b: "test".to_string(),
        opt_c: Some(99),
        opt_d: Some("present".to_string()),
    };
    let data = orig.packed();
    let m = ExtensibleMut::new(&data).unwrap();

    assert_eq!(m.a(&data), 10);
    assert_eq!(m.b(&data), "test");
    assert_eq!(m.opt_c(&data), Some(99));
    assert_eq!(m.opt_d(&data), Some("present"));
}

#[test]
fn fast_mut_getters_after_mutation() {
    let orig = Extensible {
        a: 10,
        b: "test".to_string(),
        opt_c: Some(99),
        opt_d: Some("hi".to_string()),
    };
    let mut data = orig.packed();
    let m = ExtensibleMut::new(&data).unwrap();

    m.set_a(&mut data, &42);
    m.set_b(&mut data, &"modified string".to_string());

    assert_eq!(m.a(&data), 42);
    assert_eq!(m.b(&data), "modified string");
    assert_eq!(m.opt_c(&data), Some(99));
    assert_eq!(m.opt_d(&data), Some("hi"));
}

#[test]
fn fast_nested_struct_set_label() {
    let orig = Nested {
        inner: Extensible {
            a: 1,
            b: "inner".to_string(),
            opt_c: Some(88),
            opt_d: None,
        },
        label: "outer".to_string(),
    };
    let mut data = orig.packed();
    let m = NestedMut::new(&data).unwrap();

    m.set_label(&mut data, &"new outer label".to_string());

    assert_eq!(m.label(&data), "new outer label");

    NestedMut::compact(&mut data);
    let result = Nested::unpacked(&data).unwrap();
    assert_eq!(result.label, "new outer label");
    assert_eq!(result.inner.a, 1);
    assert_eq!(result.inner.b, "inner");
    assert_eq!(result.inner.opt_c, Some(88));
}

#[test]
fn fast_nested_struct_replace_inner() {
    let orig = Nested {
        inner: Extensible {
            a: 1,
            b: "old".to_string(),
            opt_c: Some(10),
            opt_d: None,
        },
        label: "outer".to_string(),
    };
    let mut data = orig.packed();
    let m = NestedMut::new(&data).unwrap();

    let new_inner = Extensible {
        a: 99,
        b: "a much longer replacement".to_string(),
        opt_c: Some(200),
        opt_d: Some("now present".to_string()),
    };
    m.set_inner(&mut data, &new_inner);

    assert_eq!(m.label(&data), "outer");

    NestedMut::compact(&mut data);
    let result = Nested::unpacked(&data).unwrap();
    assert_eq!(result.inner, new_inner);
    assert_eq!(result.label, "outer");
}

#[test]
fn fast_with_enum_set_whole_enum() {
    let orig = WithEnum {
        id: 1,
        color: Color::Red(255),
    };
    let mut data = orig.packed();
    let m = WithEnumMut::new(&data).unwrap();

    m.set_color(&mut data, &Color::Blue("sky".to_string()));

    WithEnumMut::compact(&mut data);
    let result = WithEnum::unpacked(&data).unwrap();
    assert_eq!(result.id, 1);
    assert_eq!(result.color, Color::Blue("sky".to_string()));
}

#[test]
fn fast_with_vec_set_whole_vec() {
    let orig = WithVec {
        name: "test".to_string(),
        values: vec![1, 2, 3],
    };
    let mut data = orig.packed();
    let m = WithVecMut::new(&data).unwrap();

    m.set_values(&mut data, &vec![10, 20, 30, 40, 50]);

    assert_eq!(m.name(&data), "test");

    WithVecMut::compact(&mut data);
    let result = WithVec::unpacked(&data).unwrap();
    assert_eq!(result.name, "test");
    assert_eq!(result.values, vec![10, 20, 30, 40, 50]);
}

#[test]
fn fast_multiple_mutations_then_compact() {
    let orig = Extensible {
        a: 1,
        b: "first".to_string(),
        opt_c: Some(10),
        opt_d: Some("second".to_string()),
    };
    let mut data = orig.packed();
    let m = ExtensibleMut::new(&data).unwrap();

    // Multiple fast mutations
    m.set_a(&mut data, &100);
    m.set_b(&mut data, &"a longer first string".to_string());
    m.set_opt_c(&mut data, &Some(999));
    m.set_opt_d(&mut data, &Some("a longer second string".to_string()));

    // Verify via getters before compact
    assert_eq!(m.a(&data), 100);
    assert_eq!(m.b(&data), "a longer first string");
    assert_eq!(m.opt_c(&data), Some(999));
    assert_eq!(m.opt_d(&data), Some("a longer second string"));

    // Single compact restores canonical form
    ExtensibleMut::compact(&mut data);
    let result = Extensible::unpacked(&data).unwrap();
    assert_eq!(result.a, 100);
    assert_eq!(result.b, "a longer first string");
    assert_eq!(result.opt_c, Some(999));
    assert_eq!(result.opt_d, Some("a longer second string".to_string()));
}

#[test]
fn fast_roundtrip_view_after_compact() {
    let orig = FixedWithString {
        x: 42,
        s: "original".to_string(),
        y: 1.0,
    };
    let mut data = orig.packed();
    let m = FixedWithStringMut::new(&data).unwrap();

    m.set_s(&mut data, &"modified".to_string());

    FixedWithStringMut::compact(&mut data);

    // Verify via FracView after compact
    let view = FixedWithString::view(&data);
    assert_eq!(view.x(), 42);
    assert_eq!(view.s(), "modified");
    assert_eq!(view.y(), 1.0);
}

#[test]
fn packed_size_at_fixed_struct() {
    let data = FixedOnly {
        a: 1,
        b: 2,
        c: 3,
    }
    .packed();
    assert_eq!(FixedOnly::packed_size_at(&data, 0), data.len() as u32);
}

#[test]
fn packed_size_at_extensible_struct() {
    let data = Extensible {
        a: 1,
        b: "hello".to_string(),
        opt_c: Some(42),
        opt_d: Some("world".to_string()),
    }
    .packed();
    assert_eq!(Extensible::packed_size_at(&data, 0), data.len() as u32);
}

#[test]
fn packed_size_at_enum() {
    let data = Color::Red(42).packed();
    assert_eq!(Color::packed_size_at(&data, 0), data.len() as u32);

    let data = Color::Blue("test".to_string()).packed();
    assert_eq!(Color::packed_size_at(&data, 0), data.len() as u32);
}

#[test]
fn compact_is_idempotent() {
    let orig = Extensible {
        a: 42,
        b: "hello".to_string(),
        opt_c: Some(10),
        opt_d: Some("world".to_string()),
    };
    let mut data = orig.packed();
    let canonical = data.clone();

    // compact on already-canonical data is a no-op
    ExtensibleMut::compact(&mut data);
    assert_eq!(data, canonical);
}

#[test]
fn compact_after_shrink_reduces_size() {
    let orig = FixedWithString {
        x: 1,
        s: "hello world this is a long string".to_string(),
        y: 1.0,
    };
    let mut data = orig.packed();
    let m = FixedWithStringMut::new(&data).unwrap();

    m.set_s(&mut data, &"hi".to_string());

    let non_canonical_len = data.len();
    FixedWithStringMut::compact(&mut data);
    assert!(data.len() < non_canonical_len);
}
