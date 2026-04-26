use psio1::{FracView, FracViewType, Pack, Unpack};

#[derive(Pack, Unpack, FracView, PartialEq, Eq, Debug)]
#[fracpack(fracpack_mod = "psio")]
enum Color {
    Red(u32),
    Blue(String),
}

#[derive(Pack, Unpack, FracView, PartialEq, Debug)]
#[fracpack(fracpack_mod = "psio")]
#[fracpack(definition_will_not_change)]
struct FixedOnly {
    a: u32,
    b: u64,
    c: i16,
    f: f32,
}

#[derive(Pack, Unpack, FracView, PartialEq, Debug)]
#[fracpack(fracpack_mod = "psio")]
#[fracpack(definition_will_not_change)]
struct FixedWithString {
    x: u32,
    s: String,
    y: f64,
}

#[derive(Pack, Unpack, FracView, PartialEq, Debug)]
#[fracpack(fracpack_mod = "psio")]
struct Extensible {
    a: u32,
    b: String,
    opt_c: Option<u32>,
    opt_d: Option<String>,
}

#[derive(Pack, Unpack, FracView, PartialEq, Debug)]
#[fracpack(fracpack_mod = "psio")]
struct WithVec {
    name: String,
    values: Vec<u32>,
    tags: Vec<String>,
}

#[derive(Pack, Unpack, FracView, PartialEq, Debug)]
#[fracpack(fracpack_mod = "psio")]
struct WithArray {
    fixed_arr: [i16; 3],
    var_arr: [String; 2],
}

#[derive(Pack, Unpack, FracView, PartialEq, Debug)]
#[fracpack(fracpack_mod = "psio")]
struct Nested {
    inner: Extensible,
    label: String,
}

#[derive(Pack, Unpack, FracView, PartialEq, Debug)]
#[fracpack(fracpack_mod = "psio")]
struct WithEnum {
    id: u32,
    color: Color,
    opt_color: Option<Color>,
}

#[derive(Pack, Unpack, FracView, PartialEq, Debug)]
#[fracpack(fracpack_mod = "psio")]
struct TrailingOptionals {
    required: u32,
    opt_a: Option<u32>,
    opt_b: Option<String>,
    opt_c: Option<u64>,
}

#[derive(Pack, Unpack, FracView, PartialEq, Eq, Debug)]
#[fracpack(fracpack_mod = "psio")]
struct WithSkip {
    a: u32,
    #[fracpack(skip)]
    skipped: u32,
    b: u64,
}

impl Default for WithSkip {
    fn default() -> Self {
        WithSkip {
            a: 0,
            skipped: 0,
            b: 0,
        }
    }
}

// ── Tests ──

#[test]
fn fixed_only_struct() {
    let orig = FixedOnly {
        a: 100,
        b: 200,
        c: -42,
        f: 1.5,
    };
    let data = orig.packed();
    let view = FixedOnly::view(&data);
    assert_eq!(view.a(), 100);
    assert_eq!(view.b(), 200);
    assert_eq!(view.c(), -42);
    assert_eq!(view.f(), 1.5);
}

#[test]
fn fixed_with_string_struct() {
    let orig = FixedWithString {
        x: 42,
        s: "hello world".to_string(),
        y: 2.718,
    };
    let data = orig.packed();
    let view = FixedWithString::view(&data);
    assert_eq!(view.x(), 42);
    assert_eq!(view.s(), "hello world");
    assert_eq!(view.y(), 2.718);
}

#[test]
fn extensible_struct_all_present() {
    let orig = Extensible {
        a: 10,
        b: "ext".to_string(),
        opt_c: Some(99),
        opt_d: Some("present".to_string()),
    };
    let data = orig.packed();
    let view = Extensible::view(&data);
    assert_eq!(view.a(), 10);
    assert_eq!(view.b(), "ext");
    assert_eq!(view.opt_c(), Some(99));
    assert_eq!(view.opt_d(), Some("present"));
}

#[test]
fn extensible_struct_trailing_none() {
    let orig = Extensible {
        a: 10,
        b: "ext".to_string(),
        opt_c: None,
        opt_d: None,
    };
    let data = orig.packed();
    let view = Extensible::view(&data);
    assert_eq!(view.a(), 10);
    assert_eq!(view.b(), "ext");
    assert_eq!(view.opt_c(), None);
    assert_eq!(view.opt_d(), None);
}

#[test]
fn extensible_struct_partial_trailing() {
    let orig = Extensible {
        a: 5,
        b: "x".to_string(),
        opt_c: Some(42),
        opt_d: None,
    };
    let data = orig.packed();
    let view = Extensible::view(&data);
    assert_eq!(view.a(), 5);
    assert_eq!(view.b(), "x");
    assert_eq!(view.opt_c(), Some(42));
    assert_eq!(view.opt_d(), None);
}

#[test]
fn with_vec_struct() {
    let orig = WithVec {
        name: "test".to_string(),
        values: vec![1, 2, 3, 4, 5],
        tags: vec!["a".to_string(), "b".to_string()],
    };
    let data = orig.packed();
    let view = WithVec::view(&data);
    assert_eq!(view.name(), "test");

    let vals = view.values();
    assert_eq!(vals.len(), 5);
    let collected: Vec<u32> = vals.iter().collect();
    assert_eq!(collected, vec![1, 2, 3, 4, 5]);

    let tags = view.tags();
    assert_eq!(tags.len(), 2);
    assert_eq!(tags.get(0), "a");
    assert_eq!(tags.get(1), "b");
}

#[test]
fn with_array_struct() {
    let orig = WithArray {
        fixed_arr: [10, 20, 30],
        var_arr: ["first".to_string(), "second".to_string()],
    };
    let data = orig.packed();
    let view = WithArray::view(&data);
    assert_eq!(view.fixed_arr(), [10, 20, 30]);
    assert_eq!(view.var_arr(), ["first", "second"]);
}

#[test]
fn nested_struct() {
    let orig = Nested {
        inner: Extensible {
            a: 77,
            b: "inner".to_string(),
            opt_c: Some(88),
            opt_d: None,
        },
        label: "outer".to_string(),
    };
    let data = orig.packed();
    let view = Nested::view(&data);
    assert_eq!(view.label(), "outer");

    let inner = view.inner();
    assert_eq!(inner.a(), 77);
    assert_eq!(inner.b(), "inner");
    assert_eq!(inner.opt_c(), Some(88));
    assert_eq!(inner.opt_d(), None);
}

#[test]
fn enum_view() {
    let data = Color::Red(42).packed();
    let view = Color::view(&data);
    match view {
        ColorView::Red(v) => assert_eq!(v, 42),
        _ => panic!("expected Red"),
    }

    let data = Color::Blue("sky".to_string()).packed();
    let view = Color::view(&data);
    match view {
        ColorView::Blue(s) => assert_eq!(s, "sky"),
        _ => panic!("expected Blue"),
    }
}

#[test]
fn with_enum_struct() {
    let orig = WithEnum {
        id: 1,
        color: Color::Red(255),
        opt_color: Some(Color::Blue("ocean".to_string())),
    };
    let data = orig.packed();
    let view = WithEnum::view(&data);
    assert_eq!(view.id(), 1);

    match view.color() {
        ColorView::Red(v) => assert_eq!(v, 255),
        _ => panic!("expected Red"),
    }

    match view.opt_color().unwrap() {
        ColorView::Blue(s) => assert_eq!(s, "ocean"),
        _ => panic!("expected Blue"),
    }
}

#[test]
fn trailing_optionals() {
    // All present
    let orig = TrailingOptionals {
        required: 1,
        opt_a: Some(2),
        opt_b: Some("three".to_string()),
        opt_c: Some(4),
    };
    let data = orig.packed();
    let view = TrailingOptionals::view(&data);
    assert_eq!(view.required(), 1);
    assert_eq!(view.opt_a(), Some(2));
    assert_eq!(view.opt_b(), Some("three"));
    assert_eq!(view.opt_c(), Some(4));

    // All trailing None
    let orig = TrailingOptionals {
        required: 10,
        opt_a: None,
        opt_b: None,
        opt_c: None,
    };
    let data = orig.packed();
    let view = TrailingOptionals::view(&data);
    assert_eq!(view.required(), 10);
    assert_eq!(view.opt_a(), None);
    assert_eq!(view.opt_b(), None);
    assert_eq!(view.opt_c(), None);

    // Some trailing present
    let orig = TrailingOptionals {
        required: 20,
        opt_a: Some(21),
        opt_b: None,
        opt_c: None,
    };
    let data = orig.packed();
    let view = TrailingOptionals::view(&data);
    assert_eq!(view.required(), 20);
    assert_eq!(view.opt_a(), Some(21));
    assert_eq!(view.opt_b(), None);
    assert_eq!(view.opt_c(), None);
}

#[test]
fn skip_field() {
    let orig = WithSkip {
        a: 111,
        skipped: 222,
        b: 333,
    };
    let data = orig.packed();
    let view = WithSkip::view(&data);
    assert_eq!(view.a(), 111);
    assert_eq!(view.b(), 333);
    // skipped field has no accessor
}

#[test]
fn view_debug_format() {
    let orig = FixedOnly {
        a: 1,
        b: 2,
        c: 3,
        f: 4.0,
    };
    let data = orig.packed();
    let view = FixedOnly::view(&data);
    let debug = format!("{:?}", view);
    assert!(debug.contains("FixedOnly"));
    assert!(debug.contains("a: 1"));
    assert!(debug.contains("b: 2"));
}

#[test]
fn roundtrip_consistency() {
    let orig = Extensible {
        a: 0xDEAD,
        b: "roundtrip".to_string(),
        opt_c: Some(0xBEEF),
        opt_d: Some("consistency".to_string()),
    };
    let data = orig.packed();

    let unpacked = Extensible::unpacked(&data).unwrap();
    let view = Extensible::view(&data);

    assert_eq!(view.a(), unpacked.a);
    assert_eq!(view.b(), unpacked.b);
    assert_eq!(view.opt_c(), unpacked.opt_c);
    assert_eq!(view.opt_d(), unpacked.opt_d.as_deref());
}

#[test]
fn vec_of_structs_view() {
    let data = vec![
        Extensible {
            a: 1,
            b: "first".to_string(),
            opt_c: Some(10),
            opt_d: None,
        },
        Extensible {
            a: 2,
            b: "second".to_string(),
            opt_c: None,
            opt_d: Some("here".to_string()),
        },
    ]
    .packed();

    let view = <Vec<Extensible>>::view(&data);
    assert_eq!(view.len(), 2);

    let e0 = view.get(0);
    assert_eq!(e0.a(), 1);
    assert_eq!(e0.b(), "first");
    assert_eq!(e0.opt_c(), Some(10));
    assert_eq!(e0.opt_d(), None);

    let e1 = view.get(1);
    assert_eq!(e1.a(), 2);
    assert_eq!(e1.b(), "second");
    assert_eq!(e1.opt_c(), None);
    assert_eq!(e1.opt_d(), Some("here"));
}
