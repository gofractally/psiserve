use criterion::{
    black_box, criterion_group, criterion_main, BenchmarkId, Criterion, Throughput,
};
use psio1::{
    json, FracMutView, FracView, FracViewType, FromCanonicalJson, Pack, ToCanonicalJson, Unpack,
};
use serde::{Deserialize, Serialize};
use rkyv::rancor::Error as RkyvError;

// ============================================================
// Type definitions — matching benchmark-design.md tiers
// ============================================================

// Tier 1: Micro (fixed-size, no heap)
#[derive(Pack, Unpack, FracView, FracMutView, ToCanonicalJson, FromCanonicalJson, Serialize, Deserialize, borsh::BorshSerialize, borsh::BorshDeserialize, rkyv::Archive, rkyv::Serialize, rkyv::Deserialize, Clone, Debug, PartialEq)]
#[fracpack(fracpack_mod = "psio", definition_will_not_change)]
struct Point {
    x: f64,
    y: f64,
}

#[derive(Pack, Unpack, FracView, FracMutView, ToCanonicalJson, FromCanonicalJson, Serialize, Deserialize, borsh::BorshSerialize, borsh::BorshDeserialize, rkyv::Archive, rkyv::Serialize, rkyv::Deserialize, Clone, Debug, PartialEq)]
#[fracpack(fracpack_mod = "psio", definition_will_not_change)]
struct RGBA {
    r: u8,
    g: u8,
    b: u8,
    a: u8,
}

// Tier 2: Small (1-2 variable fields)
#[derive(Pack, Unpack, FracView, FracMutView, ToCanonicalJson, FromCanonicalJson, Serialize, Deserialize, borsh::BorshSerialize, borsh::BorshDeserialize, rkyv::Archive, rkyv::Serialize, rkyv::Deserialize, Clone, Debug, PartialEq)]
#[fracpack(fracpack_mod = "psio")]
struct Token {
    kind: u16,
    offset: u32,
    length: u32,
    text: String,
}

// Tier 3: Medium (many fields, mixed types)
#[derive(Pack, Unpack, FracView, FracMutView, ToCanonicalJson, FromCanonicalJson, Serialize, Deserialize, borsh::BorshSerialize, borsh::BorshDeserialize, rkyv::Archive, rkyv::Serialize, rkyv::Deserialize, Clone, Debug, PartialEq)]
#[fracpack(fracpack_mod = "psio")]
struct UserProfile {
    id: u64,
    name: String,
    email: String,
    bio: Option<String>,
    age: u32,
    score: f64,
    tags: Vec<String>,
    verified: bool,
}

// Tier 4: Nested
#[derive(Pack, Unpack, FracView, FracMutView, ToCanonicalJson, FromCanonicalJson, Serialize, Deserialize, borsh::BorshSerialize, borsh::BorshDeserialize, rkyv::Archive, rkyv::Serialize, rkyv::Deserialize, Clone, Debug, PartialEq)]
#[fracpack(fracpack_mod = "psio")]
struct LineItem {
    product: String,
    qty: u32,
    unit_price: f64,
}

#[derive(Pack, Unpack, FracView, FracMutView, ToCanonicalJson, FromCanonicalJson, Serialize, Deserialize, borsh::BorshSerialize, borsh::BorshDeserialize, rkyv::Archive, rkyv::Serialize, rkyv::Deserialize, Clone, Debug, PartialEq)]
#[fracpack(fracpack_mod = "psio")]
struct Order {
    id: u64,
    customer: UserProfile,
    items: Vec<LineItem>,
    total: f64,
    note: Option<String>,
}

// Tier 5: Wide (many fields)
#[derive(Pack, Unpack, FracView, FracMutView, ToCanonicalJson, FromCanonicalJson, Serialize, Deserialize, borsh::BorshSerialize, borsh::BorshDeserialize, rkyv::Archive, rkyv::Serialize, rkyv::Deserialize, Clone, Debug, PartialEq)]
#[fracpack(fracpack_mod = "psio")]
struct SensorReading {
    timestamp: u64,
    device_id: String,
    temp: f64,
    humidity: f64,
    pressure: f64,
    accel_x: f64,
    accel_y: f64,
    accel_z: f64,
    gyro_x: f64,
    gyro_y: f64,
    gyro_z: f64,
    mag_x: f64,
    mag_y: f64,
    mag_z: f64,
    battery: f32,
    signal_dbm: i16,
    error_code: Option<u32>,
    firmware: String,
}

// ============================================================
// Test data constructors
// ============================================================

fn make_point() -> Point {
    Point { x: 1.5, y: 2.5 }
}

fn make_rgba() -> RGBA {
    RGBA {
        r: 255,
        g: 128,
        b: 64,
        a: 255,
    }
}

fn make_token() -> Token {
    Token {
        kind: 42,
        offset: 1024,
        length: 15,
        text: "identifier_name".into(),
    }
}

fn make_user() -> UserProfile {
    UserProfile {
        id: 12345,
        name: "Alice Beta".into(),
        email: "alice@example.com".into(),
        bio: Some("Software engineer interested in distributed systems".into()),
        age: 30,
        score: 98.6,
        tags: vec!["rust".into(), "wasm".into(), "fracpack".into()],
        verified: true,
    }
}

fn make_line_item() -> LineItem {
    LineItem {
        product: "Widget Pro X".into(),
        qty: 3,
        unit_price: 49.99,
    }
}

fn make_order() -> Order {
    Order {
        id: 99001,
        customer: make_user(),
        items: vec![
            make_line_item(),
            LineItem {
                product: "Gadget Mini".into(),
                qty: 10,
                unit_price: 12.50,
            },
            LineItem {
                product: "Connector Cable 2m".into(),
                qty: 5,
                unit_price: 7.99,
            },
        ],
        total: 339.42,
        note: Some("Ship by Friday please".into()),
    }
}

fn make_sensor() -> SensorReading {
    SensorReading {
        timestamp: 1700000000000,
        device_id: "sensor-42-alpha".into(),
        temp: 23.5,
        humidity: 45.2,
        pressure: 1013.25,
        accel_x: 0.01,
        accel_y: -0.02,
        accel_z: 9.81,
        gyro_x: 0.001,
        gyro_y: -0.003,
        gyro_z: 0.002,
        mag_x: 25.0,
        mag_y: -12.5,
        mag_z: 42.0,
        battery: 3.7,
        signal_dbm: -65,
        error_code: None,
        firmware: "v2.4.1-rc3".into(),
    }
}

// ============================================================
// Pack benchmarks
// ============================================================

fn bench_pack(c: &mut Criterion) {
    let mut group = c.benchmark_group("pack");

    let point = make_point();
    let point_bytes = point.packed();
    group.throughput(Throughput::Bytes(point_bytes.len() as u64));
    group.bench_function("Point", |b| b.iter(|| black_box(&point).packed()));

    let rgba = make_rgba();
    let rgba_bytes = rgba.packed();
    group.throughput(Throughput::Bytes(rgba_bytes.len() as u64));
    group.bench_function("RGBA", |b| b.iter(|| black_box(&rgba).packed()));

    let token = make_token();
    let token_bytes = token.packed();
    group.throughput(Throughput::Bytes(token_bytes.len() as u64));
    group.bench_function("Token", |b| b.iter(|| black_box(&token).packed()));

    let user = make_user();
    let user_bytes = user.packed();
    group.throughput(Throughput::Bytes(user_bytes.len() as u64));
    group.bench_function("UserProfile", |b| b.iter(|| black_box(&user).packed()));

    let order = make_order();
    let order_bytes = order.packed();
    group.throughput(Throughput::Bytes(order_bytes.len() as u64));
    group.bench_function("Order", |b| b.iter(|| black_box(&order).packed()));

    let sensor = make_sensor();
    let sensor_bytes = sensor.packed();
    group.throughput(Throughput::Bytes(sensor_bytes.len() as u64));
    group.bench_function("SensorReading", |b| {
        b.iter(|| black_box(&sensor).packed())
    });

    group.finish();
}

// ============================================================
// Unpack benchmarks
// ============================================================

fn bench_unpack(c: &mut Criterion) {
    let mut group = c.benchmark_group("unpack");

    let point_data = make_point().packed();
    group.throughput(Throughput::Bytes(point_data.len() as u64));
    group.bench_function("Point", |b| {
        b.iter(|| Point::unpacked(black_box(&point_data)).unwrap())
    });

    let rgba_data = make_rgba().packed();
    group.throughput(Throughput::Bytes(rgba_data.len() as u64));
    group.bench_function("RGBA", |b| {
        b.iter(|| RGBA::unpacked(black_box(&rgba_data)).unwrap())
    });

    let token_data = make_token().packed();
    group.throughput(Throughput::Bytes(token_data.len() as u64));
    group.bench_function("Token", |b| {
        b.iter(|| Token::unpacked(black_box(&token_data)).unwrap())
    });

    let user_data = make_user().packed();
    group.throughput(Throughput::Bytes(user_data.len() as u64));
    group.bench_function("UserProfile", |b| {
        b.iter(|| UserProfile::unpacked(black_box(&user_data)).unwrap())
    });

    let order_data = make_order().packed();
    group.throughput(Throughput::Bytes(order_data.len() as u64));
    group.bench_function("Order", |b| {
        b.iter(|| Order::unpacked(black_box(&order_data)).unwrap())
    });

    let sensor_data = make_sensor().packed();
    group.throughput(Throughput::Bytes(sensor_data.len() as u64));
    group.bench_function("SensorReading", |b| {
        b.iter(|| SensorReading::unpacked(black_box(&sensor_data)).unwrap())
    });

    group.finish();
}

// ============================================================
// Validate benchmarks (verify_no_extra without deserialization)
// ============================================================

fn bench_validate(c: &mut Criterion) {
    let mut group = c.benchmark_group("validate");

    let point_data = make_point().packed();
    group.bench_function("Point", |b| {
        b.iter(|| Point::verify_no_extra(black_box(&point_data)).unwrap())
    });

    let user_data = make_user().packed();
    group.bench_function("UserProfile", |b| {
        b.iter(|| UserProfile::verify_no_extra(black_box(&user_data)).unwrap())
    });

    let order_data = make_order().packed();
    group.bench_function("Order", |b| {
        b.iter(|| Order::verify_no_extra(black_box(&order_data)).unwrap())
    });

    let sensor_data = make_sensor().packed();
    group.bench_function("SensorReading", |b| {
        b.iter(|| SensorReading::verify_no_extra(black_box(&sensor_data)).unwrap())
    });

    group.finish();
}

// ============================================================
// View benchmarks — zero-copy field access
// ============================================================

fn bench_view(c: &mut Criterion) {
    let mut group = c.benchmark_group("view");

    // Point: view all fields (fixed-only struct)
    let point_data = make_point().packed();
    group.bench_function("all/Point", |b| {
        b.iter(|| {
            let v = Point::view(black_box(&point_data));
            let _ = black_box(v.x());
            let _ = black_box(v.y());
        })
    });

    // UserProfile: view one field (last string = worst case offset chasing)
    let user_data = make_user().packed();
    group.bench_function("one/UserProfile.verified", |b| {
        b.iter(|| {
            let v = UserProfile::view(black_box(&user_data));
            black_box(v.verified())
        })
    });

    // UserProfile: view name (string field)
    group.bench_function("one/UserProfile.name", |b| {
        b.iter(|| {
            let v = UserProfile::view(black_box(&user_data));
            black_box(v.name())
        })
    });

    // UserProfile: view all fields
    group.bench_function("all/UserProfile", |b| {
        b.iter(|| {
            let v = UserProfile::view(black_box(&user_data));
            let _ = black_box(v.id());
            let _ = black_box(v.name());
            let _ = black_box(v.email());
            let _ = black_box(v.bio());
            let _ = black_box(v.age());
            let _ = black_box(v.score());
            // tags returns FracVecView — iterate all
            for tag in v.tags() {
                black_box(tag);
            }
            let _ = black_box(v.verified());
        })
    });

    // Order: view nested customer name (deep offset chasing)
    let order_data = make_order().packed();
    group.bench_function("one/Order.customer.name", |b| {
        b.iter(|| {
            let v = Order::view(black_box(&order_data));
            let cust = v.customer();
            black_box(cust.name())
        })
    });

    // SensorReading: view firmware (last string, maximum offset chasing)
    let sensor_data = make_sensor().packed();
    group.bench_function("one/SensorReading.firmware", |b| {
        b.iter(|| {
            let v = SensorReading::view(black_box(&sensor_data));
            black_box(v.firmware())
        })
    });

    // SensorReading: view all fields
    group.bench_function("all/SensorReading", |b| {
        b.iter(|| {
            let v = SensorReading::view(black_box(&sensor_data));
            let _ = black_box(v.timestamp());
            let _ = black_box(v.device_id());
            let _ = black_box(v.temp());
            let _ = black_box(v.humidity());
            let _ = black_box(v.pressure());
            let _ = black_box(v.accel_x());
            let _ = black_box(v.accel_y());
            let _ = black_box(v.accel_z());
            let _ = black_box(v.gyro_x());
            let _ = black_box(v.gyro_y());
            let _ = black_box(v.gyro_z());
            let _ = black_box(v.mag_x());
            let _ = black_box(v.mag_y());
            let _ = black_box(v.mag_z());
            let _ = black_box(v.battery());
            let _ = black_box(v.signal_dbm());
            let _ = black_box(v.error_code());
            let _ = black_box(v.firmware());
        })
    });

    group.finish();
}

// ============================================================
// View-only benchmarks (skip validation, pre-validated data)
// ============================================================

fn bench_view_prevalidated(c: &mut Criterion) {
    let mut group = c.benchmark_group("view_prevalidated");

    // Measure pure view cost without validation overhead
    let user_data = make_user().packed();
    UserProfile::verify_no_extra(&user_data).unwrap();

    group.bench_function("one/UserProfile.name", |b| {
        b.iter(|| {
            let v = UserProfile::view_at(black_box(&user_data), 0);
            black_box(v.name())
        })
    });

    group.bench_function("all/UserProfile", |b| {
        b.iter(|| {
            let v = UserProfile::view_at(black_box(&user_data), 0);
            let _ = black_box(v.id());
            let _ = black_box(v.name());
            let _ = black_box(v.email());
            let _ = black_box(v.bio());
            let _ = black_box(v.age());
            let _ = black_box(v.score());
            for tag in v.tags() {
                black_box(tag);
            }
            let _ = black_box(v.verified());
        })
    });

    group.bench_function("one/UserProfile.verified", |b| {
        b.iter(|| {
            let v = UserProfile::view_at(black_box(&user_data), 0);
            black_box(v.verified())
        })
    });

    let sensor_data = make_sensor().packed();
    SensorReading::verify_no_extra(&sensor_data).unwrap();

    group.bench_function("one/SensorReading.firmware", |b| {
        b.iter(|| {
            let v = SensorReading::view_at(black_box(&sensor_data), 0);
            black_box(v.firmware())
        })
    });

    let point_data = make_point().packed();
    Point::verify_no_extra(&point_data).unwrap();

    group.bench_function("all/Point", |b| {
        b.iter(|| {
            let v = Point::view_at(black_box(&point_data), 0);
            let _ = black_box(v.x());
            let _ = black_box(v.y());
        })
    });

    group.bench_function("all/SensorReading", |b| {
        b.iter(|| {
            let v = SensorReading::view_at(black_box(&sensor_data), 0);
            let _ = black_box(v.timestamp());
            let _ = black_box(v.device_id());
            let _ = black_box(v.temp());
            let _ = black_box(v.humidity());
            let _ = black_box(v.pressure());
            let _ = black_box(v.accel_x());
            let _ = black_box(v.accel_y());
            let _ = black_box(v.accel_z());
            let _ = black_box(v.gyro_x());
            let _ = black_box(v.gyro_y());
            let _ = black_box(v.gyro_z());
            let _ = black_box(v.mag_x());
            let _ = black_box(v.mag_y());
            let _ = black_box(v.mag_z());
            let _ = black_box(v.battery());
            let _ = black_box(v.signal_dbm());
            let _ = black_box(v.error_code());
            let _ = black_box(v.firmware());
        })
    });

    let order_data = make_order().packed();
    Order::verify_no_extra(&order_data).unwrap();

    group.bench_function("one/Order.customer.name", |b| {
        b.iter(|| {
            let v = Order::view_at(black_box(&order_data), 0);
            let cust = v.customer();
            black_box(cust.name())
        })
    });

    group.finish();
}

// ============================================================
// Canonical JSON benchmarks
// ============================================================

fn bench_json(c: &mut Criterion) {
    let mut group = c.benchmark_group("json");

    let user = make_user();
    group.bench_function("write/UserProfile", |b| {
        b.iter(|| json::to_json(black_box(&user)))
    });

    let user_json = json::to_json(&user);
    group.bench_function("read/UserProfile", |b| {
        b.iter(|| json::from_json::<UserProfile>(black_box(&user_json)).unwrap())
    });

    let order = make_order();
    group.bench_function("write/Order", |b| {
        b.iter(|| json::to_json(black_box(&order)))
    });

    let order_json = json::to_json(&order);
    group.bench_function("read/Order", |b| {
        b.iter(|| json::from_json::<Order>(black_box(&order_json)).unwrap())
    });

    let sensor = make_sensor();
    group.bench_function("write/SensorReading", |b| {
        b.iter(|| json::to_json(black_box(&sensor)))
    });

    let sensor_json = json::to_json(&sensor);
    group.bench_function("read/SensorReading", |b| {
        b.iter(|| json::from_json::<SensorReading>(black_box(&sensor_json)).unwrap())
    });

    group.finish();
}

// ============================================================
// Mutation benchmarks — MutView vs unpack-modify-repack
// ============================================================

fn bench_mutate(c: &mut Criterion) {
    let mut group = c.benchmark_group("mutate");

    // --- UserProfile.id (first fixed field, scalar overwrite) ---
    let user_data = make_user().packed();

    group.bench_function("mutview_canonical/UserProfile.id", |b| {
        b.iter(|| {
            let mut data = black_box(user_data.clone());
            let m = UserProfileMutCanonical::new(&data).unwrap();
            m.set_id(&mut data, &99999u64);
            black_box(&data);
        })
    });

    group.bench_function("repack/UserProfile.id", |b| {
        let data = &user_data;
        b.iter(|| {
            let mut u = UserProfile::unpacked(black_box(data)).unwrap();
            u.id = 99999;
            black_box(u.packed())
        })
    });

    // --- UserProfile.name (root string, same size) ---
    group.bench_function("mutview_canonical/UserProfile.name", |b| {
        b.iter(|| {
            let mut data = black_box(user_data.clone());
            let m = UserProfileMutCanonical::new(&data).unwrap();
            m.set_name(&mut data, &"Bobby Gamma".into());
            black_box(&data);
        })
    });

    group.bench_function("repack/UserProfile.name", |b| {
        let data = &user_data;
        b.iter(|| {
            let mut u = UserProfile::unpacked(black_box(data)).unwrap();
            u.name = "Bobby Gamma".into();
            black_box(u.packed())
        })
    });

    // --- UserProfile.name grow (short -> long) ---
    let short_user = UserProfile {
        name: "Al".into(),
        ..make_user()
    };
    let short_user_data = short_user.packed();

    group.bench_function("mutview_canonical/UserProfile.name_grow", |b| {
        b.iter(|| {
            let mut data = black_box(short_user_data.clone());
            let m = UserProfileMutCanonical::new(&data).unwrap();
            m.set_name(
                &mut data,
                &"Alexander Von Longnamington III Esq.".into(),
            );
            black_box(&data);
        })
    });

    group.bench_function("repack/UserProfile.name_grow", |b| {
        let data = &short_user_data;
        b.iter(|| {
            let mut u = UserProfile::unpacked(black_box(data)).unwrap();
            u.name = "Alexander Von Longnamington III Esq.".into();
            black_box(u.packed())
        })
    });

    // --- SensorReading.signal_dbm (last fixed field in wide struct) ---
    let sensor_data = make_sensor().packed();

    group.bench_function("mutview_canonical/SensorReading.signal_dbm", |b| {
        b.iter(|| {
            let mut data = black_box(sensor_data.clone());
            let m = SensorReadingMutCanonical::new(&data).unwrap();
            m.set_signal_dbm(&mut data, &(-80i16));
            black_box(&data);
        })
    });

    group.bench_function("repack/SensorReading.signal_dbm", |b| {
        let data = &sensor_data;
        b.iter(|| {
            let mut s = SensorReading::unpacked(black_box(data)).unwrap();
            s.signal_dbm = -80;
            black_box(s.packed())
        })
    });

    // --- Order.customer.age (nested fixed field, requires replacing whole customer) ---
    let order_data = make_order().packed();

    group.bench_function("mutview_canonical/Order.customer.age", |b| {
        let mut customer = make_user();
        customer.age = 55;
        b.iter(|| {
            let mut data = black_box(order_data.clone());
            let m = OrderMutCanonical::new(&data).unwrap();
            m.set_customer(&mut data, &customer);
            black_box(&data);
        })
    });

    group.bench_function("repack/Order.customer.age", |b| {
        let data = &order_data;
        b.iter(|| {
            let mut o = Order::unpacked(black_box(data)).unwrap();
            o.customer.age = 55;
            black_box(o.packed())
        })
    });

    group.finish();
}

// ============================================================
// Roundtrip benchmarks (pack -> unpack -> pack, byte-exact)
// ============================================================

fn bench_roundtrip(c: &mut Criterion) {
    let mut group = c.benchmark_group("roundtrip");

    let user = make_user();
    let user_data = user.packed();
    group.bench_function("UserProfile", |b| {
        b.iter(|| {
            let unpacked = UserProfile::unpacked(black_box(&user_data)).unwrap();
            black_box(unpacked.packed())
        })
    });

    let order = make_order();
    let order_data = order.packed();
    group.bench_function("Order", |b| {
        b.iter(|| {
            let unpacked = Order::unpacked(black_box(&order_data)).unwrap();
            black_box(unpacked.packed())
        })
    });

    group.finish();
}

// ============================================================
// Competitor: serde_json
// ============================================================

fn bench_competitor_serde(c: &mut Criterion) {
    let mut group = c.benchmark_group("competitor_serde_json");

    let user = make_user();
    group.bench_function("serialize/UserProfile", |b| {
        b.iter(|| serde_json::to_string(black_box(&user)).unwrap())
    });

    let user_json = serde_json::to_string(&user).unwrap();
    group.bench_function("deserialize/UserProfile", |b| {
        b.iter(|| serde_json::from_str::<UserProfile>(black_box(&user_json)).unwrap())
    });

    let order = make_order();
    group.bench_function("serialize/Order", |b| {
        b.iter(|| serde_json::to_string(black_box(&order)).unwrap())
    });

    let order_json = serde_json::to_string(&order).unwrap();
    group.bench_function("deserialize/Order", |b| {
        b.iter(|| serde_json::from_str::<Order>(black_box(&order_json)).unwrap())
    });

    let sensor = make_sensor();
    group.bench_function("serialize/SensorReading", |b| {
        b.iter(|| serde_json::to_string(black_box(&sensor)).unwrap())
    });

    let sensor_json = serde_json::to_string(&sensor).unwrap();
    group.bench_function("deserialize/SensorReading", |b| {
        b.iter(|| serde_json::from_str::<SensorReading>(black_box(&sensor_json)).unwrap())
    });

    group.finish();
}

// ============================================================
// Array scaling benchmarks
// ============================================================

fn bench_array_scale(c: &mut Criterion) {
    let mut group = c.benchmark_group("array_scale");
    group.sample_size(30); // reduce sample count for large arrays

    for n in [10u64, 100, 1000, 10000] {
        let points: Vec<Point> = (0..n)
            .map(|i| Point {
                x: i as f64,
                y: i as f64 * 2.0,
            })
            .collect();
        let packed = points.packed();

        group.throughput(Throughput::Elements(n));
        group.bench_with_input(BenchmarkId::new("pack_points", n), &points, |b, pts| {
            b.iter(|| black_box(pts).packed())
        });
        group.bench_with_input(
            BenchmarkId::new("unpack_points", n),
            &packed,
            |b, data| b.iter(|| <Vec<Point>>::unpacked(black_box(data)).unwrap()),
        );
        group.bench_with_input(
            BenchmarkId::new("view_points_iter", n),
            &packed,
            |b, data| {
                b.iter(|| {
                    let view = <Vec<Point>>::view(black_box(data));
                    let mut sum = 0.0f64;
                    for pt in view {
                        sum += pt.x() + pt.y();
                    }
                    black_box(sum)
                })
            },
        );
    }

    // String vectors (variable-size elements)
    for n in [10u64, 100, 1000] {
        let strings: Vec<String> = (0..n).map(|i| format!("item-{:05}", i)).collect();
        let packed = strings.packed();

        group.throughput(Throughput::Elements(n));
        group.bench_with_input(
            BenchmarkId::new("pack_strings", n),
            &strings,
            |b, strs| b.iter(|| black_box(strs).packed()),
        );
        group.bench_with_input(
            BenchmarkId::new("unpack_strings", n),
            &packed,
            |b, data| b.iter(|| <Vec<String>>::unpacked(black_box(data)).unwrap()),
        );
    }

    // UserProfile vectors (complex nested type)
    for n in [10u64, 100, 1000] {
        let users: Vec<UserProfile> = (0..n)
            .map(|i| UserProfile {
                id: i,
                name: format!("User {}", i),
                email: format!("user{}@example.com", i),
                bio: if i % 3 == 0 {
                    Some(format!("Bio for user {}", i))
                } else {
                    None
                },
                age: 20 + (i % 50) as u32,
                score: 50.0 + (i % 50) as f64,
                tags: vec![format!("tag{}", i % 5)],
                verified: i % 2 == 0,
            })
            .collect();
        let packed = users.packed();

        group.throughput(Throughput::Elements(n));
        group.bench_with_input(
            BenchmarkId::new("pack_users", n),
            &users,
            |b, us| b.iter(|| black_box(us).packed()),
        );
        group.bench_with_input(
            BenchmarkId::new("unpack_users", n),
            &packed,
            |b, data| b.iter(|| <Vec<UserProfile>>::unpacked(black_box(data)).unwrap()),
        );
    }

    group.finish();
}

// ============================================================
// Size comparison (not timed, just reports packed sizes)
// ============================================================

fn bench_packed_sizes(c: &mut Criterion) {
    let mut group = c.benchmark_group("packed_size");

    // Print sizes as a single "benchmark" so they appear in output
    let point_sz = make_point().packed().len();
    let rgba_sz = make_rgba().packed().len();
    let token_sz = make_token().packed().len();
    let user_sz = make_user().packed().len();
    let order_sz = make_order().packed().len();
    let sensor_sz = make_sensor().packed().len();

    // serde_json sizes for comparison
    let user_json_sz = serde_json::to_string(&make_user()).unwrap().len();
    let order_json_sz = serde_json::to_string(&make_order()).unwrap().len();

    // Use a trivial bench to print sizes (criterion doesn't have a "report" mode)
    group.bench_function("report", |b| {
        b.iter(|| {
            black_box((
                point_sz,
                rgba_sz,
                token_sz,
                user_sz,
                order_sz,
                sensor_sz,
                user_json_sz,
                order_json_sz,
            ))
        })
    });

    // Print sizes to stderr so they appear in cargo bench output
    eprintln!("\n--- Packed sizes (fracpack vs serde_json) ---");
    eprintln!("Point:         {:>5} bytes", point_sz);
    eprintln!("RGBA:          {:>5} bytes", rgba_sz);
    eprintln!("Token:         {:>5} bytes", token_sz);
    eprintln!("UserProfile:   {:>5} bytes (json: {} bytes)", user_sz, user_json_sz);
    eprintln!("Order:         {:>5} bytes (json: {} bytes)", order_sz, order_json_sz);
    eprintln!("SensorReading: {:>5} bytes", sensor_sz);
    eprintln!("-------------------------------------------\n");

    group.finish();
}

// ============================================================
// Competitor: bincode (serde-based, most common Rust binary format)
// ============================================================

fn bench_competitor_bincode(c: &mut Criterion) {
    let mut group = c.benchmark_group("competitor_bincode");

    let point = make_point();
    let token = make_token();
    let user = make_user();
    let order = make_order();
    let sensor = make_sensor();

    let point_bytes = bincode::serialize(&point).unwrap();
    let token_bytes = bincode::serialize(&token).unwrap();
    let user_bytes = bincode::serialize(&user).unwrap();
    let order_bytes = bincode::serialize(&order).unwrap();
    let sensor_bytes = bincode::serialize(&sensor).unwrap();

    eprintln!("\n--- bincode wire sizes ---");
    eprintln!("Point: {} B, Token: {} B, UserProfile: {} B, Order: {} B, SensorReading: {} B",
        point_bytes.len(), token_bytes.len(), user_bytes.len(), order_bytes.len(), sensor_bytes.len());

    // Pack
    group.bench_function("serialize/Point", |b| {
        b.iter(|| bincode::serialize(black_box(&point)).unwrap())
    });
    group.bench_function("serialize/Token", |b| {
        b.iter(|| bincode::serialize(black_box(&token)).unwrap())
    });
    group.bench_function("serialize/UserProfile", |b| {
        b.iter(|| bincode::serialize(black_box(&user)).unwrap())
    });
    group.bench_function("serialize/Order", |b| {
        b.iter(|| bincode::serialize(black_box(&order)).unwrap())
    });
    group.bench_function("serialize/SensorReading", |b| {
        b.iter(|| bincode::serialize(black_box(&sensor)).unwrap())
    });

    // Unpack
    group.bench_function("deserialize/Point", |b| {
        b.iter(|| bincode::deserialize::<Point>(black_box(&point_bytes)).unwrap())
    });
    group.bench_function("deserialize/Token", |b| {
        b.iter(|| bincode::deserialize::<Token>(black_box(&token_bytes)).unwrap())
    });
    group.bench_function("deserialize/UserProfile", |b| {
        b.iter(|| bincode::deserialize::<UserProfile>(black_box(&user_bytes)).unwrap())
    });
    group.bench_function("deserialize/Order", |b| {
        b.iter(|| bincode::deserialize::<Order>(black_box(&order_bytes)).unwrap())
    });
    group.bench_function("deserialize/SensorReading", |b| {
        b.iter(|| bincode::deserialize::<SensorReading>(black_box(&sensor_bytes)).unwrap())
    });

    group.finish();
}

// ============================================================
// Competitor: borsh (NEAR / Solana canonical binary format)
// ============================================================

fn bench_competitor_borsh(c: &mut Criterion) {
    use borsh::{BorshDeserialize, BorshSerialize};

    let mut group = c.benchmark_group("competitor_borsh");

    let point = make_point();
    let token = make_token();
    let user = make_user();
    let order = make_order();
    let sensor = make_sensor();

    let point_bytes = borsh::to_vec(&point).unwrap();
    let token_bytes = borsh::to_vec(&token).unwrap();
    let user_bytes = borsh::to_vec(&user).unwrap();
    let order_bytes = borsh::to_vec(&order).unwrap();
    let sensor_bytes = borsh::to_vec(&sensor).unwrap();

    eprintln!("\n--- borsh wire sizes ---");
    eprintln!("Point: {} B, Token: {} B, UserProfile: {} B, Order: {} B, SensorReading: {} B",
        point_bytes.len(), token_bytes.len(), user_bytes.len(), order_bytes.len(), sensor_bytes.len());

    // Pack
    group.bench_function("serialize/Point", |b| {
        b.iter(|| borsh::to_vec(black_box(&point)).unwrap())
    });
    group.bench_function("serialize/Token", |b| {
        b.iter(|| borsh::to_vec(black_box(&token)).unwrap())
    });
    group.bench_function("serialize/UserProfile", |b| {
        b.iter(|| borsh::to_vec(black_box(&user)).unwrap())
    });
    group.bench_function("serialize/Order", |b| {
        b.iter(|| borsh::to_vec(black_box(&order)).unwrap())
    });
    group.bench_function("serialize/SensorReading", |b| {
        b.iter(|| borsh::to_vec(black_box(&sensor)).unwrap())
    });

    // Unpack
    group.bench_function("deserialize/Point", |b| {
        b.iter(|| Point::try_from_slice(black_box(&point_bytes)).unwrap())
    });
    group.bench_function("deserialize/Token", |b| {
        b.iter(|| Token::try_from_slice(black_box(&token_bytes)).unwrap())
    });
    group.bench_function("deserialize/UserProfile", |b| {
        b.iter(|| UserProfile::try_from_slice(black_box(&user_bytes)).unwrap())
    });
    group.bench_function("deserialize/Order", |b| {
        b.iter(|| Order::try_from_slice(black_box(&order_bytes)).unwrap())
    });
    group.bench_function("deserialize/SensorReading", |b| {
        b.iter(|| SensorReading::try_from_slice(black_box(&sensor_bytes)).unwrap())
    });

    group.finish();
}

// ============================================================
// Competitor: postcard (compact no_std serde binary format)
// ============================================================

fn bench_competitor_postcard(c: &mut Criterion) {
    let mut group = c.benchmark_group("competitor_postcard");

    let point = make_point();
    let token = make_token();
    let user = make_user();
    let order = make_order();
    let sensor = make_sensor();

    let point_bytes = postcard::to_allocvec(&point).unwrap();
    let token_bytes = postcard::to_allocvec(&token).unwrap();
    let user_bytes = postcard::to_allocvec(&user).unwrap();
    let order_bytes = postcard::to_allocvec(&order).unwrap();
    let sensor_bytes = postcard::to_allocvec(&sensor).unwrap();

    eprintln!("\n--- postcard wire sizes ---");
    eprintln!("Point: {} B, Token: {} B, UserProfile: {} B, Order: {} B, SensorReading: {} B",
        point_bytes.len(), token_bytes.len(), user_bytes.len(), order_bytes.len(), sensor_bytes.len());

    // Pack
    group.bench_function("serialize/Point", |b| {
        b.iter(|| postcard::to_allocvec(black_box(&point)).unwrap())
    });
    group.bench_function("serialize/Token", |b| {
        b.iter(|| postcard::to_allocvec(black_box(&token)).unwrap())
    });
    group.bench_function("serialize/UserProfile", |b| {
        b.iter(|| postcard::to_allocvec(black_box(&user)).unwrap())
    });
    group.bench_function("serialize/Order", |b| {
        b.iter(|| postcard::to_allocvec(black_box(&order)).unwrap())
    });
    group.bench_function("serialize/SensorReading", |b| {
        b.iter(|| postcard::to_allocvec(black_box(&sensor)).unwrap())
    });

    // Unpack
    group.bench_function("deserialize/Point", |b| {
        b.iter(|| postcard::from_bytes::<Point>(black_box(&point_bytes)).unwrap())
    });
    group.bench_function("deserialize/Token", |b| {
        b.iter(|| postcard::from_bytes::<Token>(black_box(&token_bytes)).unwrap())
    });
    group.bench_function("deserialize/UserProfile", |b| {
        b.iter(|| postcard::from_bytes::<UserProfile>(black_box(&user_bytes)).unwrap())
    });
    group.bench_function("deserialize/Order", |b| {
        b.iter(|| postcard::from_bytes::<Order>(black_box(&order_bytes)).unwrap())
    });
    group.bench_function("deserialize/SensorReading", |b| {
        b.iter(|| postcard::from_bytes::<SensorReading>(black_box(&sensor_bytes)).unwrap())
    });

    group.finish();
}

// ============================================================
// Competitor: rmp-serde (MessagePack via serde)
// ============================================================

fn bench_competitor_rmp(c: &mut Criterion) {
    let mut group = c.benchmark_group("competitor_rmp");

    let point = make_point();
    let token = make_token();
    let user = make_user();
    let order = make_order();
    let sensor = make_sensor();

    let point_bytes = rmp_serde::to_vec(&point).unwrap();
    let token_bytes = rmp_serde::to_vec(&token).unwrap();
    let user_bytes = rmp_serde::to_vec(&user).unwrap();
    let order_bytes = rmp_serde::to_vec(&order).unwrap();
    let sensor_bytes = rmp_serde::to_vec(&sensor).unwrap();

    eprintln!("\n--- rmp (msgpack) wire sizes ---");
    eprintln!("Point: {} B, Token: {} B, UserProfile: {} B, Order: {} B, SensorReading: {} B",
        point_bytes.len(), token_bytes.len(), user_bytes.len(), order_bytes.len(), sensor_bytes.len());

    // Pack
    group.bench_function("serialize/Point", |b| {
        b.iter(|| rmp_serde::to_vec(black_box(&point)).unwrap())
    });
    group.bench_function("serialize/Token", |b| {
        b.iter(|| rmp_serde::to_vec(black_box(&token)).unwrap())
    });
    group.bench_function("serialize/UserProfile", |b| {
        b.iter(|| rmp_serde::to_vec(black_box(&user)).unwrap())
    });
    group.bench_function("serialize/Order", |b| {
        b.iter(|| rmp_serde::to_vec(black_box(&order)).unwrap())
    });
    group.bench_function("serialize/SensorReading", |b| {
        b.iter(|| rmp_serde::to_vec(black_box(&sensor)).unwrap())
    });

    // Unpack
    group.bench_function("deserialize/Point", |b| {
        b.iter(|| rmp_serde::from_slice::<Point>(black_box(&point_bytes)).unwrap())
    });
    group.bench_function("deserialize/Token", |b| {
        b.iter(|| rmp_serde::from_slice::<Token>(black_box(&token_bytes)).unwrap())
    });
    group.bench_function("deserialize/UserProfile", |b| {
        b.iter(|| rmp_serde::from_slice::<UserProfile>(black_box(&user_bytes)).unwrap())
    });
    group.bench_function("deserialize/Order", |b| {
        b.iter(|| rmp_serde::from_slice::<Order>(black_box(&order_bytes)).unwrap())
    });
    group.bench_function("deserialize/SensorReading", |b| {
        b.iter(|| rmp_serde::from_slice::<SensorReading>(black_box(&sensor_bytes)).unwrap())
    });

    group.finish();
}

// ============================================================
// Competitor: rkyv (zero-copy deserialization — the real competitor)
// ============================================================

fn bench_competitor_rkyv(c: &mut Criterion) {
    let mut group = c.benchmark_group("competitor_rkyv");

    let point = make_point();
    let token = make_token();
    let user = make_user();
    let order = make_order();
    let sensor = make_sensor();

    let point_bytes = rkyv::to_bytes::<RkyvError>(&point).unwrap();
    let token_bytes = rkyv::to_bytes::<RkyvError>(&token).unwrap();
    let user_bytes = rkyv::to_bytes::<RkyvError>(&user).unwrap();
    let order_bytes = rkyv::to_bytes::<RkyvError>(&order).unwrap();
    let sensor_bytes = rkyv::to_bytes::<RkyvError>(&sensor).unwrap();

    eprintln!("\n--- rkyv wire sizes ---");
    eprintln!("Point: {} B, Token: {} B, UserProfile: {} B, Order: {} B, SensorReading: {} B",
        point_bytes.len(), token_bytes.len(), user_bytes.len(), order_bytes.len(), sensor_bytes.len());

    // Pack (serialize)
    group.bench_function("serialize/Point", |b| {
        b.iter(|| rkyv::to_bytes::<RkyvError>(black_box(&point)).unwrap())
    });
    group.bench_function("serialize/Token", |b| {
        b.iter(|| rkyv::to_bytes::<RkyvError>(black_box(&token)).unwrap())
    });
    group.bench_function("serialize/UserProfile", |b| {
        b.iter(|| rkyv::to_bytes::<RkyvError>(black_box(&user)).unwrap())
    });
    group.bench_function("serialize/Order", |b| {
        b.iter(|| rkyv::to_bytes::<RkyvError>(black_box(&order)).unwrap())
    });
    group.bench_function("serialize/SensorReading", |b| {
        b.iter(|| rkyv::to_bytes::<RkyvError>(black_box(&sensor)).unwrap())
    });

    // Unpack (full deserialize to native type)
    group.bench_function("deserialize/Point", |b| {
        b.iter(|| rkyv::from_bytes::<Point, RkyvError>(black_box(&point_bytes)).unwrap())
    });
    group.bench_function("deserialize/Token", |b| {
        b.iter(|| rkyv::from_bytes::<Token, RkyvError>(black_box(&token_bytes)).unwrap())
    });
    group.bench_function("deserialize/UserProfile", |b| {
        b.iter(|| rkyv::from_bytes::<UserProfile, RkyvError>(black_box(&user_bytes)).unwrap())
    });
    group.bench_function("deserialize/Order", |b| {
        b.iter(|| rkyv::from_bytes::<Order, RkyvError>(black_box(&order_bytes)).unwrap())
    });
    group.bench_function("deserialize/SensorReading", |b| {
        b.iter(|| rkyv::from_bytes::<SensorReading, RkyvError>(black_box(&sensor_bytes)).unwrap())
    });

    // View (zero-copy access — comparable to fracpack view)
    group.bench_function("view-all/Point", |b| {
        b.iter(|| {
            let a = unsafe { rkyv::access_unchecked::<ArchivedPoint>(black_box(&point_bytes)) };
            let _ = black_box(a.x);
            let _ = black_box(a.y);
        })
    });
    group.bench_function("view-all/Token", |b| {
        b.iter(|| {
            let a = unsafe { rkyv::access_unchecked::<ArchivedToken>(black_box(&token_bytes)) };
            let _ = black_box(a.kind);
            let _ = black_box(a.offset);
            let _ = black_box(a.length);
            let _ = black_box(a.text.as_str());
        })
    });
    group.bench_function("view-all/UserProfile", |b| {
        b.iter(|| {
            let a = unsafe { rkyv::access_unchecked::<ArchivedUserProfile>(black_box(&user_bytes)) };
            let _ = black_box(a.id);
            let _ = black_box(a.name.as_str());
            let _ = black_box(a.email.as_str());
            let _ = black_box(a.bio.as_ref().map(|s| s.as_str()));
            let _ = black_box(a.age);
            let _ = black_box(a.score);
            let _ = black_box(a.tags.len());
            let _ = black_box(a.verified);
        })
    });
    group.bench_function("view-all/SensorReading", |b| {
        b.iter(|| {
            let a = unsafe { rkyv::access_unchecked::<ArchivedSensorReading>(black_box(&sensor_bytes)) };
            let _ = black_box(a.timestamp);
            let _ = black_box(a.device_id.as_str());
            let _ = black_box(a.temp);
            let _ = black_box(a.humidity);
            let _ = black_box(a.pressure);
            let _ = black_box(a.accel_x);
            let _ = black_box(a.accel_y);
            let _ = black_box(a.accel_z);
            let _ = black_box(a.gyro_x);
            let _ = black_box(a.gyro_y);
            let _ = black_box(a.gyro_z);
            let _ = black_box(a.mag_x);
            let _ = black_box(a.mag_y);
            let _ = black_box(a.mag_z);
            let _ = black_box(a.battery);
            let _ = black_box(a.signal_dbm);
            let _ = black_box(a.error_code);
            let _ = black_box(a.firmware.as_str());
        })
    });

    // View-one (single field, zero-copy)
    group.bench_function("view-one/UserProfile.name", |b| {
        b.iter(|| {
            let a = unsafe { rkyv::access_unchecked::<ArchivedUserProfile>(black_box(&user_bytes)) };
            black_box(a.name.as_str())
        })
    });
    group.bench_function("view-one/UserProfile.id", |b| {
        b.iter(|| {
            let a = unsafe { rkyv::access_unchecked::<ArchivedUserProfile>(black_box(&user_bytes)) };
            black_box(a.id)
        })
    });

    group.finish();
}

criterion_group! {
    name = benches;
    config = Criterion::default()
        .sample_size(50)
        .measurement_time(std::time::Duration::from_millis(500))
        .warm_up_time(std::time::Duration::from_millis(200));
    targets =
        bench_pack,
        bench_unpack,
        bench_validate,
        bench_view,
        bench_view_prevalidated,
        bench_json,
        bench_mutate,
        bench_roundtrip,
        bench_competitor_serde,
        bench_competitor_bincode,
        bench_competitor_borsh,
        bench_competitor_postcard,
        bench_competitor_rmp,
        bench_competitor_rkyv,
        bench_array_scale,
        bench_packed_sizes,
}
criterion_main!(benches);
