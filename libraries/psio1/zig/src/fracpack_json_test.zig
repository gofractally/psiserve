// Canonical JSON (de)serialization tests for fracpack.
//
// Tests round-trip fidelity, u64 string encoding, variant format,
// nested structs, optionals, bytes hex encoding, and compatibility
// with the reference JS/Rust implementations.

const std = @import("std");
const fp = @import("fracpack.zig");

const alloc = std.testing.allocator;

// ── Type definitions (mirroring golden test types) ─────────────────────

const FixedInts = struct {
    pub const fracpack_fixed = true;
    x: i32,
    y: i32,
};

const FixedMixed = struct {
    pub const fracpack_fixed = true;
    b: bool,
    u8_: u8,
    u16_: u16,
    u32_: u32,
    u64_: u64,
};

const AllPrimitives = struct {
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
};

const SingleBool = struct {
    pub const fracpack_fixed = true;
    value: bool,
};

const SingleU32 = struct {
    pub const fracpack_fixed = true;
    value: u32,
};

const SingleString = struct {
    value: []const u8,
};

const WithStrings = struct {
    empty_str: []const u8,
    hello: []const u8,
    unicode: []const u8,
};

const Inner = struct {
    value: u32,
    label: []const u8,
};

const Outer = struct {
    inner: Inner,
    name: []const u8,
};

const WithOptionals = struct {
    opt_int: ?u32,
    opt_str: ?[]const u8,
};

const DataVariant = union(enum) {
    uint32: u32,
    string: []const u8,
    Inner: Inner,
};

const WithVariant = struct {
    data: DataVariant,
};

const WithVectors = struct {
    ints: []const u32,
    strings: []const []const u8,
};

const FixedArray = struct {
    pub const fracpack_fixed = true;
    arr: [3]u32,
};

// ── Helper to check JSON output ────────────────────────────────────────

fn expectJson(comptime T: type, value: T, expected: []const u8) !void {
    const json = try fp.toJson(T, value, alloc);
    defer alloc.free(json);
    try std.testing.expectEqualStrings(expected, json);
}

fn expectRoundTrip(comptime T: type, value: T) !void {
    const json = try fp.toJson(T, value, alloc);
    defer alloc.free(json);
    const decoded = try fp.fromJson(T, json, alloc);
    defer fp.free(T, alloc, decoded);
    // Re-serialize and compare JSON strings (canonical form)
    const json2 = try fp.toJson(T, decoded, alloc);
    defer alloc.free(json2);
    try std.testing.expectEqualStrings(json, json2);
}

// ── 1. Basic scalar types ──────────────────────────────────────────────

test "toJson: bool true/false" {
    try expectJson(SingleBool, .{ .value = true }, "{\"value\":true}");
    try expectJson(SingleBool, .{ .value = false }, "{\"value\":false}");
}

test "toJson: u32 number" {
    try expectJson(SingleU32, .{ .value = 0 }, "{\"value\":0}");
    try expectJson(SingleU32, .{ .value = 42 }, "{\"value\":42}");
    try expectJson(SingleU32, .{ .value = 4294967295 }, "{\"value\":4294967295}");
}

test "toJson: i32 negative" {
    try expectJson(FixedInts, .{ .x = -1, .y = -2147483648 }, "{\"x\":-1,\"y\":-2147483648}");
}

// ── 2. u64/i64 as JSON string ──────────────────────────────────────────

test "toJson: u64 as string" {
    try expectJson(FixedMixed, .{
        .b = true,
        .u8_ = 255,
        .u16_ = 65535,
        .u32_ = 4294967295,
        .u64_ = 18446744073709551615,
    }, "{\"b\":true,\"u8_\":255,\"u16_\":65535,\"u32_\":4294967295,\"u64_\":\"18446744073709551615\"}");
}

test "toJson: u64 zero as string" {
    try expectJson(FixedMixed, .{
        .b = false,
        .u8_ = 0,
        .u16_ = 0,
        .u32_ = 0,
        .u64_ = 0,
    }, "{\"b\":false,\"u8_\":0,\"u16_\":0,\"u32_\":0,\"u64_\":\"0\"}");
}

test "fromJson: u64 from string" {
    const json = "{\"b\":true,\"u8_\":1,\"u16_\":1,\"u32_\":1,\"u64_\":\"18446744073709551615\"}";
    const val = try fp.fromJson(FixedMixed, json, alloc);
    try std.testing.expectEqual(true, val.b);
    try std.testing.expectEqual(@as(u8, 1), val.u8_);
    try std.testing.expectEqual(@as(u64, 18446744073709551615), val.u64_);
}

test "fromJson: i64 from string" {
    const T = struct { val: i64 };
    const json = "{\"val\":\"-9223372036854775808\"}";
    const val = try fp.fromJson(T, json, alloc);
    try std.testing.expectEqual(@as(i64, -9223372036854775808), val.val);
}

// ── 3. Strings ─────────────────────────────────────────────────────────

test "toJson: string escaping" {
    try expectJson(SingleString, .{ .value = "hello" }, "{\"value\":\"hello\"}");
    try expectJson(SingleString, .{ .value = "quote\"backslash\\" }, "{\"value\":\"quote\\\"backslash\\\\\"}");
    try expectJson(SingleString, .{ .value = "tab\there\nnewline" }, "{\"value\":\"tab\\there\\nnewline\"}");
}

test "toJson: empty string" {
    try expectJson(SingleString, .{ .value = "" }, "{\"value\":\"\"}");
}

test "fromJson: string round trip" {
    const json = "{\"value\":\"hello world\"}";
    const val = try fp.fromJson(SingleString, json, alloc);
    defer fp.free(SingleString, alloc, val);
    try std.testing.expectEqualStrings("hello world", val.value);
}

// ── 4. Floats ──────────────────────────────────────────────────────────

test "toJson: float values" {
    const T = struct {
        pub const fracpack_fixed = true;
        f: f64,
    };
    try expectJson(T, .{ .f = 1.0 }, "{\"f\":1}");
    try expectJson(T, .{ .f = -1.0 }, "{\"f\":-1}");
    try expectJson(T, .{ .f = 0.0 }, "{\"f\":0}");
}

test "toJson: float NaN/Inf as null" {
    const T = struct {
        pub const fracpack_fixed = true;
        f: f64,
    };
    try expectJson(T, .{ .f = std.math.nan(f64) }, "{\"f\":null}");
    try expectJson(T, .{ .f = std.math.inf(f64) }, "{\"f\":null}");
}

test "fromJson: float from null (NaN)" {
    const T = struct {
        pub const fracpack_fixed = true;
        f: f64,
    };
    const val = try fp.fromJson(T, "{\"f\":null}", alloc);
    try std.testing.expect(std.math.isNan(val.f));
}

// ── 5. Optionals ───────────────────────────────────────────────────────

test "toJson: optional none/some" {
    try expectJson(WithOptionals, .{ .opt_int = null, .opt_str = null }, "{\"opt_int\":null,\"opt_str\":null}");
    try expectJson(WithOptionals, .{ .opt_int = 42, .opt_str = null }, "{\"opt_int\":42,\"opt_str\":null}");
    try expectJson(WithOptionals, .{ .opt_int = null, .opt_str = "hello" }, "{\"opt_int\":null,\"opt_str\":\"hello\"}");
}

test "fromJson: optional none" {
    const val = try fp.fromJson(WithOptionals, "{\"opt_int\":null,\"opt_str\":null}", alloc);
    defer fp.free(WithOptionals, alloc, val);
    try std.testing.expectEqual(@as(?u32, null), val.opt_int);
    try std.testing.expectEqual(@as(?[]const u8, null), val.opt_str);
}

test "fromJson: optional some" {
    const val = try fp.fromJson(WithOptionals, "{\"opt_int\":99,\"opt_str\":\"world\"}", alloc);
    defer fp.free(WithOptionals, alloc, val);
    try std.testing.expectEqual(@as(?u32, 99), val.opt_int);
    try std.testing.expectEqualStrings("world", val.opt_str.?);
}

// ── 6. Variant encoding ───────────────────────────────────────────────

test "toJson: variant uint32" {
    try expectJson(WithVariant, .{ .data = .{ .uint32 = 42 } }, "{\"data\":{\"uint32\":42}}");
}

test "toJson: variant string" {
    try expectJson(WithVariant, .{ .data = .{ .string = "hello" } }, "{\"data\":{\"string\":\"hello\"}}");
}

test "toJson: variant struct" {
    try expectJson(WithVariant, .{
        .data = .{ .Inner = .{ .value = 7, .label = "variant_inner" } },
    }, "{\"data\":{\"Inner\":{\"value\":7,\"label\":\"variant_inner\"}}}");
}

test "fromJson: variant uint32" {
    const val = try fp.fromJson(WithVariant, "{\"data\":{\"uint32\":42}}", alloc);
    defer fp.free(WithVariant, alloc, val);
    try std.testing.expectEqual(@as(u32, 42), val.data.uint32);
}

test "fromJson: variant string" {
    const val = try fp.fromJson(WithVariant, "{\"data\":{\"string\":\"hello\"}}", alloc);
    defer fp.free(WithVariant, alloc, val);
    try std.testing.expectEqualStrings("hello", val.data.string);
}

test "fromJson: variant struct" {
    const val = try fp.fromJson(WithVariant, "{\"data\":{\"Inner\":{\"value\":7,\"label\":\"test\"}}}", alloc);
    defer fp.free(WithVariant, alloc, val);
    try std.testing.expectEqual(@as(u32, 7), val.data.Inner.value);
    try std.testing.expectEqualStrings("test", val.data.Inner.label);
}

// ── 7. Nested structs ──────────────────────────────────────────────────

test "toJson: nested struct" {
    try expectJson(Outer, .{
        .inner = .{ .value = 1, .label = "inner" },
        .name = "outer",
    }, "{\"inner\":{\"value\":1,\"label\":\"inner\"},\"name\":\"outer\"}");
}

test "fromJson: nested struct round trip" {
    const json = "{\"inner\":{\"value\":42,\"label\":\"test\"},\"name\":\"hello\"}";
    const val = try fp.fromJson(Outer, json, alloc);
    defer fp.free(Outer, alloc, val);
    try std.testing.expectEqual(@as(u32, 42), val.inner.value);
    try std.testing.expectEqualStrings("test", val.inner.label);
    try std.testing.expectEqualStrings("hello", val.name);
}

// ── 8. Vectors/arrays ──────────────────────────────────────────────────

test "toJson: vector of u32" {
    const T = struct { items: []const u32 };
    try expectJson(T, .{ .items = &.{ 1, 2, 3 } }, "{\"items\":[1,2,3]}");
}

test "toJson: vector of strings" {
    const T = struct { items: []const []const u8 };
    const strs: []const []const u8 = &.{ "a", "bb", "ccc" };
    try expectJson(T, .{ .items = strs }, "{\"items\":[\"a\",\"bb\",\"ccc\"]}");
}

test "toJson: empty vector" {
    const T = struct { items: []const u32 };
    try expectJson(T, .{ .items = &.{} }, "{\"items\":[]}");
}

test "toJson: fixed array" {
    try expectJson(FixedArray, .{ .arr = .{ 1, 2, 3 } }, "{\"arr\":[1,2,3]}");
}

test "fromJson: vector of u32" {
    const T = struct { items: []const u32 };
    const val = try fp.fromJson(T, "{\"items\":[10,20,30]}", alloc);
    defer fp.free(T, alloc, val);
    try std.testing.expectEqual(@as(usize, 3), val.items.len);
    try std.testing.expectEqual(@as(u32, 10), val.items[0]);
    try std.testing.expectEqual(@as(u32, 20), val.items[1]);
    try std.testing.expectEqual(@as(u32, 30), val.items[2]);
}

// ── 9. Bytes as hex ────────────────────────────────────────────────────

test "toJson: Bytes as hex string" {
    const T = struct { data: fp.Bytes };
    try expectJson(T, .{ .data = fp.Bytes.init(&.{ 0xDE, 0xAD, 0xBE, 0xEF }) }, "{\"data\":\"deadbeef\"}");
}

test "toJson: empty Bytes" {
    const T = struct { data: fp.Bytes };
    try expectJson(T, .{ .data = fp.Bytes.init(&.{}) }, "{\"data\":\"\"}");
}

test "fromJson: hex string to Bytes" {
    const T = struct { data: fp.Bytes };
    const val = try fp.fromJson(T, "{\"data\":\"cafebabe\"}", alloc);
    defer alloc.free(val.data.data);
    try std.testing.expectEqualSlices(u8, &.{ 0xCA, 0xFE, 0xBA, 0xBE }, val.data.data);
}

// ── 10. Tuples ─────────────────────────────────────────────────────────

test "toJson: tuple as array" {
    const T = struct { u32, []const u8 };
    try expectJson(T, .{ 42, "hello" }, "[42,\"hello\"]");
}

test "fromJson: tuple from array" {
    const T = struct { u32, []const u8 };
    const val = try fp.fromJson(T, "[42,\"hello\"]", alloc);
    defer fp.free(T, alloc, val);
    try std.testing.expectEqual(@as(u32, 42), val[0]);
    try std.testing.expectEqualStrings("hello", val[1]);
}

// ── 11. Round-trip tests ───────────────────────────────────────────────

test "round trip: FixedInts" {
    try expectRoundTrip(FixedInts, .{ .x = -42, .y = 2147483647 });
}

test "round trip: WithOptionals both present" {
    try expectRoundTrip(WithOptionals, .{ .opt_int = 99, .opt_str = "world" });
}

test "round trip: WithOptionals both null" {
    try expectRoundTrip(WithOptionals, .{ .opt_int = null, .opt_str = null });
}

test "round trip: nested struct" {
    try expectRoundTrip(Outer, .{
        .inner = .{ .value = 100, .label = "deep" },
        .name = "top",
    });
}

test "round trip: variant" {
    try expectRoundTrip(WithVariant, .{ .data = .{ .uint32 = 42 } });
    try expectRoundTrip(WithVariant, .{ .data = .{ .string = "hello" } });
    try expectRoundTrip(WithVariant, .{
        .data = .{ .Inner = .{ .value = 7, .label = "variant_inner" } },
    });
}

// ── 12. Golden vector JSON matching ────────────────────────────────────
// These test cases verify that toJson output matches the "json" field
// from test_vectors/vectors.json exactly.

test "golden: FixedInts/zeros" {
    try expectJson(FixedInts, .{ .x = 0, .y = 0 }, "{\"x\":0,\"y\":0}");
}

test "golden: FixedInts/positive" {
    try expectJson(FixedInts, .{ .x = 42, .y = 100 }, "{\"x\":42,\"y\":100}");
}

test "golden: FixedInts/negative" {
    try expectJson(FixedInts, .{ .x = -1, .y = -2147483648 }, "{\"x\":-1,\"y\":-2147483648}");
}

test "golden: SingleBool/false" {
    try expectJson(SingleBool, .{ .value = false }, "{\"value\":false}");
}

test "golden: SingleBool/true" {
    try expectJson(SingleBool, .{ .value = true }, "{\"value\":true}");
}

test "golden: SingleString/empty" {
    try expectJson(SingleString, .{ .value = "" }, "{\"value\":\"\"}");
}

test "golden: SingleString/hello" {
    try expectJson(SingleString, .{ .value = "hello" }, "{\"value\":\"hello\"}");
}

test "golden: SingleString/escapes" {
    try expectJson(SingleString, .{ .value = "quote\"backslash\\" }, "{\"value\":\"quote\\\"backslash\\\\\"}");
}

test "golden: WithVariant/uint32_alt" {
    try expectJson(WithVariant, .{ .data = .{ .uint32 = 42 } }, "{\"data\":{\"uint32\":42}}");
}

test "golden: WithVariant/string_alt" {
    try expectJson(WithVariant, .{ .data = .{ .string = "hello" } }, "{\"data\":{\"string\":\"hello\"}}");
}

test "golden: WithVariant/struct_alt" {
    try expectJson(WithVariant, .{
        .data = .{ .Inner = .{ .value = 7, .label = "variant_inner" } },
    }, "{\"data\":{\"Inner\":{\"value\":7,\"label\":\"variant_inner\"}}}");
}

test "golden: WithOptionals/both_null" {
    try expectJson(WithOptionals, .{ .opt_int = null, .opt_str = null }, "{\"opt_int\":null,\"opt_str\":null}");
}

test "golden: WithOptionals/int_only" {
    try expectJson(WithOptionals, .{ .opt_int = 42, .opt_str = null }, "{\"opt_int\":42,\"opt_str\":null}");
}

test "golden: WithOptionals/both_present" {
    try expectJson(WithOptionals, .{ .opt_int = 99, .opt_str = "world" }, "{\"opt_int\":99,\"opt_str\":\"world\"}");
}

// AllPrimitives golden tests — verifying float formatting and u64/i64 string encoding

test "golden: AllPrimitives/zeros" {
    try expectJson(AllPrimitives, .{
        .b = false, .u8v = 0, .i8v = 0, .u16v = 0, .i16v = 0,
        .u32v = 0, .i32v = 0, .u64v = 0, .i64v = 0, .f32v = 0, .f64v = 0,
    }, "{\"b\":false,\"u8v\":0,\"i8v\":0,\"u16v\":0,\"i16v\":0,\"u32v\":0,\"i32v\":0,\"u64v\":\"0\",\"i64v\":\"0\",\"f32v\":0,\"f64v\":0}");
}

test "golden: AllPrimitives/ones" {
    try expectJson(AllPrimitives, .{
        .b = true, .u8v = 1, .i8v = 1, .u16v = 1, .i16v = 1,
        .u32v = 1, .i32v = 1, .u64v = 1, .i64v = 1, .f32v = 1.0, .f64v = 1.0,
    }, "{\"b\":true,\"u8v\":1,\"i8v\":1,\"u16v\":1,\"i16v\":1,\"u32v\":1,\"i32v\":1,\"u64v\":\"1\",\"i64v\":\"1\",\"f32v\":1,\"f64v\":1}");
}

test "golden: AllPrimitives/max_unsigned" {
    try expectJson(AllPrimitives, .{
        .b = true, .u8v = 255, .i8v = 127, .u16v = 65535, .i16v = 32767,
        .u32v = 4294967295, .i32v = 2147483647,
        .u64v = 18446744073709551615, .i64v = 9223372036854775807,
        .f32v = @as(f32, @bitCast(@as(u32, 0x40490FD0))),
        .f64v = @as(f64, @bitCast(@as(u64, 0x4005BF0A8B145769))),
    }, "{\"b\":true,\"u8v\":255,\"i8v\":127,\"u16v\":65535,\"i16v\":32767,\"u32v\":4294967295,\"i32v\":2147483647,\"u64v\":\"18446744073709551615\",\"i64v\":\"9223372036854775807\",\"f32v\":3.141590118408203,\"f64v\":2.718281828459045}");
}

test "golden: AllPrimitives/min_signed" {
    try expectJson(AllPrimitives, .{
        .b = false, .u8v = 0, .i8v = -128, .u16v = 0, .i16v = -32768,
        .u32v = 0, .i32v = -2147483648, .u64v = 0, .i64v = -9223372036854775808,
        .f32v = -1.0, .f64v = -1.0,
    }, "{\"b\":false,\"u8v\":0,\"i8v\":-128,\"u16v\":0,\"i16v\":-32768,\"u32v\":0,\"i32v\":-2147483648,\"u64v\":\"0\",\"i64v\":\"-9223372036854775808\",\"f32v\":-1,\"f64v\":-1}");
}

test "golden: AllPrimitives/fractional_floats" {
    try expectJson(AllPrimitives, .{
        .b = false, .u8v = 0, .i8v = 0, .u16v = 0, .i16v = 0,
        .u32v = 0, .i32v = 0, .u64v = 0, .i64v = 0,
        .f32v = @as(f32, @bitCast(@as(u32, 0x3DCCCCCD))),
        .f64v = @as(f64, @bitCast(@as(u64, 0x3FB999999999999A))),
    }, "{\"b\":false,\"u8v\":0,\"i8v\":0,\"u16v\":0,\"i16v\":0,\"u32v\":0,\"i32v\":0,\"u64v\":\"0\",\"i64v\":\"0\",\"f32v\":0.10000000149011612,\"f64v\":0.1}");
}

// ── 13. Full round-trip: JSON → struct → binary → struct → JSON ────────

test "full round trip: JSON → pack → unpack → JSON" {
    const json = "{\"x\":42,\"y\":-100}";
    const val = try fp.fromJson(FixedInts, json, alloc);
    // Pack to binary
    const binary = try fp.marshal(FixedInts, alloc, val);
    defer alloc.free(binary);
    // Unpack from binary
    const unpacked = try fp.unmarshal(FixedInts, alloc, binary);
    // Back to JSON
    const json2 = try fp.toJson(FixedInts, unpacked, alloc);
    defer alloc.free(json2);
    try std.testing.expectEqualStrings(json, json2);
}
