use psio1::{Pack, Unpack};

// Fixed struct (definition_will_not_change) — no heap header
#[derive(Pack, Unpack, PartialEq, Debug)]
#[fracpack(fracpack_mod = "psio")]
#[fracpack(definition_will_not_change)]
struct Point {
    x: u32,
    y: u32,
}

// Extensible struct — has a heap header
#[derive(Pack, Unpack, PartialEq, Debug)]
#[fracpack(fracpack_mod = "psio")]
struct Simple {
    x: u32,
}

// Extensible struct with mixed fixed/variable fields
#[derive(Pack, Unpack, PartialEq, Debug)]
#[fracpack(fracpack_mod = "psio")]
struct Person {
    name: String,
    age: u32,
    active: bool,
}

// Extensible struct with trailing optional
#[derive(Pack, Unpack, PartialEq, Debug)]
#[fracpack(fracpack_mod = "psio")]
struct PersonV2 {
    name: String,
    age: u32,
    email: Option<String>,
}

fn to_hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{:02x}", b)).collect()
}

#[test]
fn cross_lang_u32() {
    let val: u32 = 0x01020304;
    let packed = val.packed();
    println!("u32: {}", to_hex(&packed));
}

#[test]
fn cross_lang_string() {
    let val = String::from("hi");
    let packed = val.packed();
    println!("string: {}", to_hex(&packed));
}

#[test]
fn cross_lang_vec_u32() {
    let val: Vec<u32> = vec![1, 2];
    let packed = val.packed();
    println!("vec_u32: {}", to_hex(&packed));
}

#[test]
fn cross_lang_option_none() {
    let val: Option<u32> = None;
    let packed = val.packed();
    println!("option_none: {}", to_hex(&packed));
}

#[test]
fn cross_lang_option_some() {
    let val: Option<u32> = Some(42);
    let packed = val.packed();
    println!("option_some: {}", to_hex(&packed));
}

#[test]
fn cross_lang_fixed_struct() {
    let val = Point { x: 1, y: 2 };
    let packed = val.packed();
    println!("fixed_struct: {}", to_hex(&packed));
}

#[test]
fn cross_lang_extensible_struct() {
    let val = Simple { x: 42 };
    let packed = val.packed();
    println!("extensible_struct: {}", to_hex(&packed));
}

#[test]
fn cross_lang_person() {
    let val = Person {
        name: "Alice".into(),
        age: 30,
        active: true,
    };
    let packed = val.packed();
    println!("person: {}", to_hex(&packed));
}

#[test]
fn cross_lang_tuple() {
    let val: (u32, String) = (42, "hello".into());
    let packed = val.packed();
    println!("tuple: {}", to_hex(&packed));
}

#[test]
fn cross_lang_person_v2_email_none() {
    let val = PersonV2 {
        name: "Alice".into(),
        age: 30,
        email: None,
    };
    let packed = val.packed();
    println!("person_v2_email_none: {}", to_hex(&packed));
}
