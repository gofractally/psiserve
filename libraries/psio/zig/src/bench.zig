const std = @import("std");
const fp = @import("fracpack.zig");

// ── Type definitions (matching benchmark_schemas.json) ──────────────────

const Point = struct {
    pub const fracpack_fixed = true;
    x: f64,
    y: f64,
};

const RGBA = struct {
    pub const fracpack_fixed = true;
    r: u8,
    g: u8,
    b: u8,
    a: u8,
};

const Token = struct {
    kind: u16,
    offset: u32,
    length: u32,
    text: []const u8,
};

const UserProfile = struct {
    id: u64,
    name: []const u8,
    email: []const u8,
    bio: ?[]const u8,
    age: u32,
    score: f64,
    tags: []const []const u8,
    verified: bool,
};

const LineItem = struct {
    product: []const u8,
    qty: u32,
    unit_price: f64,
};

const Order = struct {
    id: u64,
    customer: UserProfile,
    items: []const LineItem,
    total: f64,
    note: ?[]const u8,
};

const SensorReading = struct {
    timestamp: u64,
    device_id: []const u8,
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
    error_code: ?u32,
    firmware: []const u8,
};

// ── Test data constructors ──────────────────────────────────────────────

fn samplePoint() Point {
    return .{ .x = 3.14159265358979, .y = 2.71828182845905 };
}

fn sampleRGBA() RGBA {
    return .{ .r = 255, .g = 128, .b = 64, .a = 255 };
}

fn sampleToken() Token {
    return .{
        .kind = 42,
        .offset = 1024,
        .length = 15,
        .text = "identifier_name",
    };
}

fn sampleUserProfile() UserProfile {
    const tags: []const []const u8 = &.{ "admin", "premium", "verified" };
    return .{
        .id = 123456789,
        .name = "Alice Johnson",
        .email = "alice@example.com",
        .bio = "Software engineer with 10 years of experience in distributed systems.",
        .age = 32,
        .score = 98.6,
        .tags = tags,
        .verified = true,
    };
}

fn sampleLineItem() LineItem {
    return .{
        .product = "Widget Pro Max",
        .qty = 3,
        .unit_price = 29.99,
    };
}

fn sampleOrder() Order {
    const items: []const LineItem = &.{
        .{ .product = "Widget Pro Max", .qty = 3, .unit_price = 29.99 },
        .{ .product = "Gadget Mini", .qty = 1, .unit_price = 149.50 },
        .{ .product = "Thingamajig XL", .qty = 12, .unit_price = 5.25 },
    };
    return .{
        .id = 9876543210,
        .customer = sampleUserProfile(),
        .items = items,
        .total = 302.47,
        .note = "Please deliver before noon",
    };
}

fn sampleSensorReading() SensorReading {
    return .{
        .timestamp = 1700000000000,
        .device_id = "sensor-alpha-42",
        .temp = 22.5,
        .humidity = 65.3,
        .pressure = 1013.25,
        .accel_x = 0.01,
        .accel_y = -0.02,
        .accel_z = 9.81,
        .gyro_x = 0.001,
        .gyro_y = -0.003,
        .gyro_z = 0.002,
        .mag_x = 25.0,
        .mag_y = -12.5,
        .mag_z = 42.0,
        .battery = 87.5,
        .signal_dbm = -42,
        .error_code = null,
        .firmware = "v2.1.3-rc1",
    };
}

// ── Output helpers ──────────────────────────────────────────────────────

const print = std.debug.print;

// ── Benchmark harness ───────────────────────────────────────────────────

const BenchResult = struct {
    ops_per_sec: u64,
    median_ns: u64,
    bytes: usize,
};

fn bench(comptime func: anytype, args: anytype) BenchResult {
    // Warmup
    for (0..100) |_| {
        const r = @call(.auto, func, args);
        std.mem.doNotOptimizeAway(&r);
    }

    // Calibrate: run for ~50ms to estimate iteration count
    var timer = std.time.Timer.start() catch unreachable;
    var cal_iters: usize = 0;
    while (timer.read() < 50_000_000) : (cal_iters += 1) {
        const r = @call(.auto, func, args);
        std.mem.doNotOptimizeAway(&r);
    }

    // Measure: run for at least 200ms or 1000 iterations
    const target = @max(1000, cal_iters * 4);
    timer.reset();
    for (0..target) |_| {
        const r = @call(.auto, func, args);
        std.mem.doNotOptimizeAway(&r);
    }
    const elapsed_ns = timer.read();

    const ops_per_sec = if (elapsed_ns > 0)
        @as(u64, target) * 1_000_000_000 / elapsed_ns
    else
        0;
    const median_ns = if (target > 0) elapsed_ns / @as(u64, target) else 0;

    return .{
        .ops_per_sec = ops_per_sec,
        .median_ns = median_ns,
        .bytes = 0,
    };
}

fn printResult(name: []const u8, result: BenchResult) void {
    if (result.bytes > 0) {
        print("{s:<40} {d:>12} ops/s  {d:>8} ns  {d:>6} B\n", .{
            name,
            result.ops_per_sec,
            result.median_ns,
            result.bytes,
        });
    } else {
        print("{s:<40} {d:>12} ops/s  {d:>8} ns\n", .{
            name,
            result.ops_per_sec,
            result.median_ns,
        });
    }
}

// ── Pack benchmarks ─────────────────────────────────────────────────────

// c_allocator: libc malloc/free — same allocator C++ (std::allocator) and Rust (system) use
const alloc = std.heap.c_allocator;

fn packPoint() usize {
    const data = fp.marshal(Point, alloc, samplePoint()) catch unreachable;
    defer alloc.free(data);
    return data.len;
}

fn packRGBA() usize {
    const data = fp.marshal(RGBA, alloc, sampleRGBA()) catch unreachable;
    defer alloc.free(data);
    return data.len;
}

fn packToken() usize {
    const data = fp.marshal(Token, alloc, sampleToken()) catch unreachable;
    defer alloc.free(data);
    return data.len;
}

fn packUserProfile() usize {
    const data = fp.marshal(UserProfile, alloc, sampleUserProfile()) catch unreachable;
    defer alloc.free(data);
    return data.len;
}

fn packOrder() usize {
    const data = fp.marshal(Order, alloc, sampleOrder()) catch unreachable;
    defer alloc.free(data);
    return data.len;
}

fn packSensorReading() usize {
    const data = fp.marshal(SensorReading, alloc, sampleSensorReading()) catch unreachable;
    defer alloc.free(data);
    return data.len;
}

// ── Unpack benchmarks ───────────────────────────────────────────────────

fn unpackPoint(data: []const u8) usize {
    const val = fp.unmarshal(Point, alloc, data) catch unreachable;
    _ = val;
    return data.len;
}

fn unpackToken(data: []const u8) usize {
    const val = fp.unmarshal(Token, alloc, data) catch unreachable;
    defer fp.free(Token, alloc, val);
    return data.len;
}

fn unpackUserProfile(data: []const u8) usize {
    const val = fp.unmarshal(UserProfile, alloc, data) catch unreachable;
    defer fp.free(UserProfile, alloc, val);
    return data.len;
}

fn unpackOrder(data: []const u8) usize {
    const val = fp.unmarshal(Order, alloc, data) catch unreachable;
    defer fp.free(Order, alloc, val);
    return data.len;
}

fn unpackSensorReading(data: []const u8) usize {
    const val = fp.unmarshal(SensorReading, alloc, data) catch unreachable;
    defer fp.free(SensorReading, alloc, val);
    return data.len;
}

// ── View benchmarks ─────────────────────────────────────────────────────

fn viewOneUserProfileTags(data: []const u8) usize {
    // Access the last variable-length field: tags (worst-case offset chasing)
    const v = fp.view(UserProfile, data) catch unreachable;
    const tags = v.tags;
    const n = tags.len();
    std.mem.doNotOptimizeAway(&n);
    return data.len;
}

fn viewAllUserProfile(data: []const u8) usize {
    const v = fp.view(UserProfile, data) catch unreachable;
    std.mem.doNotOptimizeAway(&v.id);
    std.mem.doNotOptimizeAway(&v.name);
    std.mem.doNotOptimizeAway(&v.email);
    std.mem.doNotOptimizeAway(&v.bio);
    std.mem.doNotOptimizeAway(&v.age);
    std.mem.doNotOptimizeAway(&v.score);
    const tags = v.tags;
    var tag_iter = tags.iter();
    while (tag_iter.next()) |t| {
        std.mem.doNotOptimizeAway(&t);
    }
    std.mem.doNotOptimizeAway(&v.verified);
    return data.len;
}

fn viewOneOrderCustomerName(data: []const u8) usize {
    // Nested access: order.customer.name
    const v = fp.view(Order, data) catch unreachable;
    const name = v.customer.name;
    std.mem.doNotOptimizeAway(&name);
    return data.len;
}

fn viewAllSensorReading(data: []const u8) usize {
    const v = fp.view(SensorReading, data) catch unreachable;
    std.mem.doNotOptimizeAway(&v.timestamp);
    std.mem.doNotOptimizeAway(&v.device_id);
    std.mem.doNotOptimizeAway(&v.temp);
    std.mem.doNotOptimizeAway(&v.humidity);
    std.mem.doNotOptimizeAway(&v.pressure);
    std.mem.doNotOptimizeAway(&v.accel_x);
    std.mem.doNotOptimizeAway(&v.accel_y);
    std.mem.doNotOptimizeAway(&v.accel_z);
    std.mem.doNotOptimizeAway(&v.gyro_x);
    std.mem.doNotOptimizeAway(&v.gyro_y);
    std.mem.doNotOptimizeAway(&v.gyro_z);
    std.mem.doNotOptimizeAway(&v.mag_x);
    std.mem.doNotOptimizeAway(&v.mag_y);
    std.mem.doNotOptimizeAway(&v.mag_z);
    std.mem.doNotOptimizeAway(&v.battery);
    std.mem.doNotOptimizeAway(&v.signal_dbm);
    std.mem.doNotOptimizeAway(&v.error_code);
    std.mem.doNotOptimizeAway(&v.firmware);
    return data.len;
}

fn viewAllOrder(data: []const u8) usize {
    const v = fp.view(Order, data) catch unreachable;
    std.mem.doNotOptimizeAway(&v.id);
    std.mem.doNotOptimizeAway(&v.customer.id);
    std.mem.doNotOptimizeAway(&v.customer.name);
    std.mem.doNotOptimizeAway(&v.customer.email);
    std.mem.doNotOptimizeAway(&v.customer.bio);
    std.mem.doNotOptimizeAway(&v.customer.age);
    std.mem.doNotOptimizeAway(&v.customer.score);
    std.mem.doNotOptimizeAway(&v.customer.verified);
    const items = v.items;
    var iter = items.iter();
    while (iter.next()) |item| {
        std.mem.doNotOptimizeAway(&item.product);
        std.mem.doNotOptimizeAway(&item.qty);
        std.mem.doNotOptimizeAway(&item.unit_price);
    }
    std.mem.doNotOptimizeAway(&v.total);
    std.mem.doNotOptimizeAway(&v.note);
    return data.len;
}

// ── Validate benchmarks ─────────────────────────────────────────────────

fn validatePoint(data: []const u8) usize {
    const result = fp.validate(Point, data);
    std.mem.doNotOptimizeAway(&result);
    return data.len;
}

fn validateUserProfile(data: []const u8) usize {
    const result = fp.validate(UserProfile, data);
    std.mem.doNotOptimizeAway(&result);
    return data.len;
}

fn validateOrder(data: []const u8) usize {
    const result = fp.validate(Order, data);
    std.mem.doNotOptimizeAway(&result);
    return data.len;
}

fn validateSensorReading(data: []const u8) usize {
    const result = fp.validate(SensorReading, data);
    std.mem.doNotOptimizeAway(&result);
    return data.len;
}

// ── JSON benchmarks ─────────────────────────────────────────────────────

fn jsonWriteUserProfile() usize {
    const json = fp.toJson(UserProfile, sampleUserProfile(), alloc) catch unreachable;
    defer alloc.free(json);
    return json.len;
}

fn jsonWriteOrder() usize {
    const json = fp.toJson(Order, sampleOrder(), alloc) catch unreachable;
    defer alloc.free(json);
    return json.len;
}

fn jsonWriteSensorReading() usize {
    const json = fp.toJson(SensorReading, sampleSensorReading(), alloc) catch unreachable;
    defer alloc.free(json);
    return json.len;
}

fn jsonReadUserProfile(json: []const u8) usize {
    const val = fp.fromJson(UserProfile, json, alloc) catch unreachable;
    defer fp.free(UserProfile, alloc, val);
    return json.len;
}

fn jsonReadOrder(json: []const u8) usize {
    const val = fp.fromJson(Order, json, alloc) catch unreachable;
    defer fp.free(Order, alloc, val);
    return json.len;
}

fn jsonReadSensorReading(json: []const u8) usize {
    const val = fp.fromJson(SensorReading, json, alloc) catch unreachable;
    defer fp.free(SensorReading, alloc, val);
    return json.len;
}

// ── Mutate-repack benchmarks (unpack-modify-repack) ─────────────────────

fn mutateRepackUserProfileId(data: []const u8) usize {
    var val = fp.unmarshal(UserProfile, alloc, data) catch unreachable;
    defer fp.free(UserProfile, alloc, val);
    val.id = 999999;
    const out = fp.marshal(UserProfile, alloc, val) catch unreachable;
    defer alloc.free(out);
    return out.len;
}

fn mutateRepackUserProfileName(data: []const u8) usize {
    var val = fp.unmarshal(UserProfile, alloc, data) catch unreachable;
    // Free old allocated name, then replace with new allocated copy
    alloc.free(val.name);
    val.name = alloc.dupe(u8, "Bob Smith-Richardson") catch unreachable;
    const out = fp.marshal(UserProfile, alloc, val) catch unreachable;
    defer alloc.free(out);
    defer fp.free(UserProfile, alloc, val);
    return out.len;
}

// ── Competitor: std.json ────────────────────────────────────────────────

fn stdJsonParse(json: []const u8) usize {
    const parsed_val = std.json.parseFromSlice(std.json.Value, alloc, json, .{}) catch unreachable;
    defer parsed_val.deinit();
    const root = parsed_val.value;
    std.mem.doNotOptimizeAway(&root);
    return json.len;
}

// ── Array scaling benchmarks ────────────────────────────────────────────

const PointArray = struct {
    points: []const Point,
};

fn generatePoints(n: usize) []const Point {
    const points = alloc.alloc(Point, n) catch unreachable;
    for (points, 0..) |*p, i| {
        const fi: f64 = @floatFromInt(i);
        p.* = .{ .x = fi * 1.1, .y = fi * 2.2 };
    }
    return points;
}

fn packPointsN(points: []const Point) usize {
    const val = PointArray{ .points = points };
    const data = fp.marshal(PointArray, alloc, val) catch unreachable;
    defer alloc.free(data);
    return data.len;
}

fn unpackPointsN(data: []const u8) usize {
    const val = fp.unmarshal(PointArray, alloc, data) catch unreachable;
    defer fp.free(PointArray, alloc, val);
    return data.len;
}

fn viewPointsN(data: []const u8) usize {
    const v = fp.view(PointArray, data) catch unreachable;
    const pts = v.points;
    var i: u32 = 0;
    const n = pts.len();
    while (i < n) : (i += 1) {
        const p = pts.get(i) catch unreachable;
        std.mem.doNotOptimizeAway(&p);
    }
    return data.len;
}

// ── View vs Native benchmarks ──────────────────────────────────────────

fn nativePointAccess() f64 {
    const p = samplePoint();
    const result = p.x + p.y;
    std.mem.doNotOptimizeAway(&result);
    return result;
}

fn viewPointAccess(data: []const u8) f64 {
    const v = fp.view(Point, data) catch unreachable;
    const result = v.x + v.y;
    std.mem.doNotOptimizeAway(&result);
    return result;
}

fn unpackPointAccess(data: []const u8) f64 {
    const p = fp.unmarshal(Point, alloc, data) catch unreachable;
    const result = p.x + p.y;
    std.mem.doNotOptimizeAway(&result);
    return result;
}

fn nativeUserProfileAccess() u64 {
    const u = sampleUserProfile();
    std.mem.doNotOptimizeAway(&u.id);
    std.mem.doNotOptimizeAway(u.name.ptr);
    std.mem.doNotOptimizeAway(u.email.ptr);
    std.mem.doNotOptimizeAway(&u.bio);
    std.mem.doNotOptimizeAway(&u.age);
    std.mem.doNotOptimizeAway(&u.score);
    for (u.tags) |t| {
        std.mem.doNotOptimizeAway(t.ptr);
    }
    std.mem.doNotOptimizeAway(&u.verified);
    return u.id;
}

fn viewUserProfileAccess(data: []const u8) u64 {
    const v = fp.view(UserProfile, data) catch unreachable;
    std.mem.doNotOptimizeAway(&v.id);
    std.mem.doNotOptimizeAway(v.name.ptr);
    std.mem.doNotOptimizeAway(v.email.ptr);
    std.mem.doNotOptimizeAway(&v.bio);
    std.mem.doNotOptimizeAway(&v.age);
    std.mem.doNotOptimizeAway(&v.score);
    const tags = v.tags;
    var tag_iter = tags.iter();
    while (tag_iter.next()) |t| {
        std.mem.doNotOptimizeAway(t.ptr);
    }
    std.mem.doNotOptimizeAway(&v.verified);
    return v.id;
}

fn unpackUserProfileAccess(data: []const u8) u64 {
    const u = fp.unmarshal(UserProfile, alloc, data) catch unreachable;
    defer fp.free(UserProfile, alloc, u);
    std.mem.doNotOptimizeAway(&u.id);
    std.mem.doNotOptimizeAway(u.name.ptr);
    std.mem.doNotOptimizeAway(u.email.ptr);
    std.mem.doNotOptimizeAway(&u.bio);
    std.mem.doNotOptimizeAway(&u.age);
    std.mem.doNotOptimizeAway(&u.score);
    for (u.tags) |t| {
        std.mem.doNotOptimizeAway(t.ptr);
    }
    std.mem.doNotOptimizeAway(&u.verified);
    return u.id;
}

fn nativeSensorReadingAccess() f64 {
    const s = sampleSensorReading();
    std.mem.doNotOptimizeAway(&s.timestamp);
    std.mem.doNotOptimizeAway(s.device_id.ptr);
    std.mem.doNotOptimizeAway(&s.temp);
    std.mem.doNotOptimizeAway(&s.humidity);
    std.mem.doNotOptimizeAway(&s.pressure);
    std.mem.doNotOptimizeAway(&s.accel_x);
    std.mem.doNotOptimizeAway(&s.accel_y);
    std.mem.doNotOptimizeAway(&s.accel_z);
    std.mem.doNotOptimizeAway(&s.gyro_x);
    std.mem.doNotOptimizeAway(&s.gyro_y);
    std.mem.doNotOptimizeAway(&s.gyro_z);
    std.mem.doNotOptimizeAway(&s.mag_x);
    std.mem.doNotOptimizeAway(&s.mag_y);
    std.mem.doNotOptimizeAway(&s.mag_z);
    std.mem.doNotOptimizeAway(&s.battery);
    std.mem.doNotOptimizeAway(&s.signal_dbm);
    std.mem.doNotOptimizeAway(&s.error_code);
    std.mem.doNotOptimizeAway(s.firmware.ptr);
    return s.temp;
}

fn viewSensorReadingAccess(data: []const u8) f64 {
    const v = fp.view(SensorReading, data) catch unreachable;
    std.mem.doNotOptimizeAway(&v.timestamp);
    std.mem.doNotOptimizeAway(v.device_id.ptr);
    std.mem.doNotOptimizeAway(&v.temp);
    std.mem.doNotOptimizeAway(&v.humidity);
    std.mem.doNotOptimizeAway(&v.pressure);
    std.mem.doNotOptimizeAway(&v.accel_x);
    std.mem.doNotOptimizeAway(&v.accel_y);
    std.mem.doNotOptimizeAway(&v.accel_z);
    std.mem.doNotOptimizeAway(&v.gyro_x);
    std.mem.doNotOptimizeAway(&v.gyro_y);
    std.mem.doNotOptimizeAway(&v.gyro_z);
    std.mem.doNotOptimizeAway(&v.mag_x);
    std.mem.doNotOptimizeAway(&v.mag_y);
    std.mem.doNotOptimizeAway(&v.mag_z);
    std.mem.doNotOptimizeAway(&v.battery);
    std.mem.doNotOptimizeAway(&v.signal_dbm);
    std.mem.doNotOptimizeAway(&v.error_code);
    std.mem.doNotOptimizeAway(v.firmware.ptr);
    return v.temp;
}

fn unpackSensorReadingAccess(data: []const u8) f64 {
    const s = fp.unmarshal(SensorReading, alloc, data) catch unreachable;
    defer fp.free(SensorReading, alloc, s);
    std.mem.doNotOptimizeAway(&s.timestamp);
    std.mem.doNotOptimizeAway(s.device_id.ptr);
    std.mem.doNotOptimizeAway(&s.temp);
    std.mem.doNotOptimizeAway(&s.humidity);
    std.mem.doNotOptimizeAway(&s.pressure);
    std.mem.doNotOptimizeAway(&s.accel_x);
    std.mem.doNotOptimizeAway(&s.accel_y);
    std.mem.doNotOptimizeAway(&s.accel_z);
    std.mem.doNotOptimizeAway(&s.gyro_x);
    std.mem.doNotOptimizeAway(&s.gyro_y);
    std.mem.doNotOptimizeAway(&s.gyro_z);
    std.mem.doNotOptimizeAway(&s.mag_x);
    std.mem.doNotOptimizeAway(&s.mag_y);
    std.mem.doNotOptimizeAway(&s.mag_z);
    std.mem.doNotOptimizeAway(&s.battery);
    std.mem.doNotOptimizeAway(&s.signal_dbm);
    std.mem.doNotOptimizeAway(&s.error_code);
    std.mem.doNotOptimizeAway(s.firmware.ptr);
    return s.temp;
}

// ── Main ────────────────────────────────────────────────────────────────

pub fn main() !void {
    print("\n", .{});
    print("Fracpack Zig Benchmarks\n", .{});
    print("=======================\n\n", .{});
    print("{s:<40} {s:>15}  {s:>8}  {s:>6}\n", .{ "Operation", "ops/sec", "ns/op", "bytes" });
    print("{s:-<76}\n", .{""});

    // ── Pack benchmarks ─────────────────────────────────────────────

    print("\n--- Pack (value -> bytes) ---\n", .{});
    {
        var r = bench(packPoint, .{});
        r.bytes = packPoint();
        printResult("pack/Point", r);
    }
    {
        var r = bench(packRGBA, .{});
        r.bytes = packRGBA();
        printResult("pack/RGBA", r);
    }
    {
        var r = bench(packToken, .{});
        r.bytes = packToken();
        printResult("pack/Token", r);
    }
    {
        var r = bench(packUserProfile, .{});
        r.bytes = packUserProfile();
        printResult("pack/UserProfile", r);
    }
    {
        var r = bench(packOrder, .{});
        r.bytes = packOrder();
        printResult("pack/Order", r);
    }
    {
        var r = bench(packSensorReading, .{});
        r.bytes = packSensorReading();
        printResult("pack/SensorReading", r);
    }

    // ── Unpack benchmarks ───────────────────────────────────────────

    print("\n--- Unpack (bytes -> value) ---\n", .{});

    const point_data = try fp.marshal(Point, alloc, samplePoint());
    defer alloc.free(point_data);
    {
        var r = bench(unpackPoint, .{point_data});
        r.bytes = point_data.len;
        printResult("unpack/Point", r);
    }

    const token_data = try fp.marshal(Token, alloc, sampleToken());
    defer alloc.free(token_data);
    {
        var r = bench(unpackToken, .{token_data});
        r.bytes = token_data.len;
        printResult("unpack/Token", r);
    }

    const user_data = try fp.marshal(UserProfile, alloc, sampleUserProfile());
    defer alloc.free(user_data);
    {
        var r = bench(unpackUserProfile, .{user_data});
        r.bytes = user_data.len;
        printResult("unpack/UserProfile", r);
    }

    const order_data = try fp.marshal(Order, alloc, sampleOrder());
    defer alloc.free(order_data);
    {
        var r = bench(unpackOrder, .{order_data});
        r.bytes = order_data.len;
        printResult("unpack/Order", r);
    }

    const sensor_data = try fp.marshal(SensorReading, alloc, sampleSensorReading());
    defer alloc.free(sensor_data);
    {
        var r = bench(unpackSensorReading, .{sensor_data});
        r.bytes = sensor_data.len;
        printResult("unpack/SensorReading", r);
    }

    // ── View benchmarks (THE fracpack advantage) ────────────────────

    print("\n--- View (zero-copy field access) ---\n", .{});
    {
        var r = bench(viewOneUserProfileTags, .{user_data});
        r.bytes = user_data.len;
        printResult("view-one/UserProfile.tags", r);
    }
    {
        var r = bench(viewAllUserProfile, .{user_data});
        r.bytes = user_data.len;
        printResult("view-all/UserProfile", r);
    }
    {
        var r = bench(viewOneOrderCustomerName, .{order_data});
        r.bytes = order_data.len;
        printResult("view-one/Order.customer.name", r);
    }
    {
        var r = bench(viewAllOrder, .{order_data});
        r.bytes = order_data.len;
        printResult("view-all/Order", r);
    }
    {
        var r = bench(viewAllSensorReading, .{sensor_data});
        r.bytes = sensor_data.len;
        printResult("view-all/SensorReading", r);
    }

    // ── Validate benchmarks ─────────────────────────────────────────

    print("\n--- Validate (zero-copy integrity check) ---\n", .{});
    {
        var r = bench(validatePoint, .{point_data});
        r.bytes = point_data.len;
        printResult("validate/Point", r);
    }
    {
        var r = bench(validateUserProfile, .{user_data});
        r.bytes = user_data.len;
        printResult("validate/UserProfile", r);
    }
    {
        var r = bench(validateOrder, .{order_data});
        r.bytes = order_data.len;
        printResult("validate/Order", r);
    }
    {
        var r = bench(validateSensorReading, .{sensor_data});
        r.bytes = sensor_data.len;
        printResult("validate/SensorReading", r);
    }

    // ── JSON benchmarks ─────────────────────────────────────────────

    print("\n--- JSON (fracpack canonical JSON) ---\n", .{});
    {
        var r = bench(jsonWriteUserProfile, .{});
        r.bytes = jsonWriteUserProfile();
        printResult("json-write/UserProfile", r);
    }
    {
        var r = bench(jsonWriteOrder, .{});
        r.bytes = jsonWriteOrder();
        printResult("json-write/Order", r);
    }
    {
        var r = bench(jsonWriteSensorReading, .{});
        r.bytes = jsonWriteSensorReading();
        printResult("json-write/SensorReading", r);
    }

    const user_json = try fp.toJson(UserProfile, sampleUserProfile(), alloc);
    defer alloc.free(user_json);
    {
        var r = bench(jsonReadUserProfile, .{user_json});
        r.bytes = user_json.len;
        printResult("json-read/UserProfile", r);
    }

    const order_json = try fp.toJson(Order, sampleOrder(), alloc);
    defer alloc.free(order_json);
    {
        var r = bench(jsonReadOrder, .{order_json});
        r.bytes = order_json.len;
        printResult("json-read/Order", r);
    }

    const sensor_json = try fp.toJson(SensorReading, sampleSensorReading(), alloc);
    defer alloc.free(sensor_json);
    {
        var r = bench(jsonReadSensorReading, .{sensor_json});
        r.bytes = sensor_json.len;
        printResult("json-read/SensorReading", r);
    }

    // ── Mutate-repack benchmarks ────────────────────────────────────

    print("\n--- Mutate-Repack (unpack -> modify -> repack) ---\n", .{});
    print("  NOTE: Zig has MutView but these benchmark the naive\n", .{});
    print("  unpack-modify-repack path for comparison.\n", .{});
    {
        var r = bench(mutateRepackUserProfileId, .{user_data});
        r.bytes = user_data.len;
        printResult("mutate-repack/UserProfile.id", r);
    }
    {
        var r = bench(mutateRepackUserProfileName, .{user_data});
        r.bytes = user_data.len;
        printResult("mutate-repack/UserProfile.name", r);
    }

    // ── Competitor: std.json ────────────────────────────────────────

    print("\n--- Competitor: std.json (baseline) ---\n", .{});
    {
        var r = bench(stdJsonParse, .{user_json});
        r.bytes = user_json.len;
        printResult("competitor/std.json.parse", r);
    }

    // ── Array scaling ───────────────────────────────────────────────

    print("\n--- Array Scaling (points) ---\n", .{});

    inline for (.{ 10, 100, 1000, 10000 }) |comptime_n| {
        const n: usize = comptime_n;
        const points = generatePoints(n);
        defer alloc.free(points);
        const pa = PointArray{ .points = points };

        const packed_pts = fp.marshal(PointArray, alloc, pa) catch unreachable;
        defer alloc.free(packed_pts);

        {
            var r = bench(packPointsN, .{points});
            r.bytes = packed_pts.len;
            printResult("array/pack-points[" ++ std.fmt.comptimePrint("{d}", .{comptime_n}) ++ "]", r);
        }
        {
            var r = bench(unpackPointsN, .{packed_pts});
            r.bytes = packed_pts.len;
            printResult("array/unpack-points[" ++ std.fmt.comptimePrint("{d}", .{comptime_n}) ++ "]", r);
        }
        {
            var r = bench(viewPointsN, .{packed_pts});
            r.bytes = packed_pts.len;
            printResult("array/view-points[" ++ std.fmt.comptimePrint("{d}", .{comptime_n}) ++ "]", r);
        }
    }

    // ── View vs Native Struct Access ─────────────────────────────────

    print("\n--- View vs Native Struct Access ---\n", .{});

    // Point (fixed-size — view should equal native)
    {
        const r = bench(nativePointAccess, .{});
        printResult("native/Point", r);
    }
    {
        const r = bench(viewPointAccess, .{point_data});
        printResult("view/Point", r);
    }
    {
        const r = bench(unpackPointAccess, .{point_data});
        printResult("unpack+access/Point", r);
    }

    // UserProfile (complex — strings, vec, optional)
    {
        const r = bench(nativeUserProfileAccess, .{});
        printResult("native/UserProfile", r);
    }
    {
        const r = bench(viewUserProfileAccess, .{user_data});
        printResult("view/UserProfile", r);
    }
    {
        const r = bench(unpackUserProfileAccess, .{user_data});
        printResult("unpack+access/UserProfile", r);
    }

    // SensorReading (wide — 18 fields)
    {
        const r = bench(nativeSensorReadingAccess, .{});
        printResult("native/SensorReading", r);
    }
    {
        const r = bench(viewSensorReadingAccess, .{sensor_data});
        printResult("view/SensorReading", r);
    }
    {
        const r = bench(unpackSensorReadingAccess, .{sensor_data});
        printResult("unpack+access/SensorReading", r);
    }

    // ── Size Comparison ─────────────────────────────────────────────

    print("\n--- Size Comparison ---\n", .{});
    print("{s:<30} {s:>10} {s:>10} {s:>8}\n", .{ "Schema", "fracpack", "JSON", "ratio" });
    print("{s:-<62}\n", .{""});

    printSizeComparison("Point", Point, samplePoint());
    printSizeComparison("RGBA", RGBA, sampleRGBA());
    printSizeComparison("Token", Token, sampleToken());
    printSizeComparison("UserProfile", UserProfile, sampleUserProfile());
    printSizeComparison("Order", Order, sampleOrder());
    printSizeComparison("SensorReading", SensorReading, sampleSensorReading());

    print("\n", .{});
}

fn printSizeComparison(comptime name: []const u8, comptime T: type, value: T) void {
    const fp_data = fp.marshal(T, alloc, value) catch unreachable;
    defer alloc.free(fp_data);
    const json_data = fp.toJson(T, value, alloc) catch unreachable;
    defer alloc.free(json_data);

    const ratio: f64 = @as(f64, @floatFromInt(fp_data.len)) / @as(f64, @floatFromInt(json_data.len));
    print("{s:<30} {d:>10} {d:>10} {d:>7.2}x\n", .{
        name,
        fp_data.len,
        json_data.len,
        ratio,
    });
}
