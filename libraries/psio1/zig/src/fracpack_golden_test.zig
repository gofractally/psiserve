// Golden binary comparison tests for fracpack.
// Tests all 71 cases from test_vectors/vectors.json using the comptime generic API.

const std = @import("std");
const fp = @import("fracpack.zig");

const alloc = std.testing.allocator;
const hexToBytes = fp.hexToBytes;

fn expectPack(comptime T: type, value: T, comptime expected_hex: []const u8) !void {
    const expected = hexToBytes(expected_hex);
    const data = try fp.marshal(T, alloc, value);
    defer alloc.free(data);
    try std.testing.expectEqualSlices(u8, &expected, data);
}

fn expectPackAndView(comptime T: type, value: T, comptime expected_hex: []const u8) !void {
    const expected = hexToBytes(expected_hex);
    const data = try fp.marshal(T, alloc, value);
    defer alloc.free(data);
    try std.testing.expectEqualSlices(u8, &expected, data);
    // Also verify view parses without error
    _ = try fp.view(T, data);
}

// ── Type definitions ────────────────────────────────────────────────────

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

const WithVectors = struct {
    ints: []const u32,
    strings: []const []const u8,
};

const WithOptionals = struct {
    opt_int: ?u32,
    opt_str: ?[]const u8,
};

const Inner = struct {
    value: u32,
    label: []const u8,
};

const Outer = struct {
    inner: Inner,
    name: []const u8,
};

const DataVariant = union(enum) {
    uint32: u32,
    string: []const u8,
    Inner: Inner,
};

const WithVariant = struct {
    data: DataVariant,
};

const VecOfStructs = struct {
    items: []const Inner,
};

const OptionalStruct = struct {
    item: ?Inner,
};

const VecOfOptionals = struct {
    items: []const ?u32,
};

const OptionalVec = struct {
    items: ?[]const u32,
};

const NestedVecs = struct {
    matrix: []const []const u32,
};

const FixedArray = struct {
    pub const fracpack_fixed = true;
    arr: [3]u32,
};

const Complex = struct {
    items: []const Inner,
    opt_vec: ?[]const u32,
    vec_opt: []const ?[]const u8,
    opt_struct: ?Inner,
};

const EmptyExtensible = struct {
    dummy: u32,
};

// ── FixedInts (4 cases) ────────────────────────────────────────────────

test "FixedInts/zeros" {
    try expectPack(FixedInts, .{ .x = 0, .y = 0 }, "0000000000000000");
}
test "FixedInts/positive" {
    try expectPack(FixedInts, .{ .x = 42, .y = 100 }, "2A00000064000000");
}
test "FixedInts/negative" {
    try expectPack(FixedInts, .{ .x = -1, .y = -2147483648 }, "FFFFFFFF00000080");
}
test "FixedInts/max" {
    try expectPack(FixedInts, .{ .x = 2147483647, .y = 2147483647 }, "FFFFFF7FFFFFFF7F");
}

// ── FixedMixed (3 cases) ───────────────────────────────────────────────

test "FixedMixed/zeros" {
    try expectPack(FixedMixed, .{ .b = false, .u8_ = 0, .u16_ = 0, .u32_ = 0, .u64_ = 0 }, "00000000000000000000000000000000");
}
test "FixedMixed/ones" {
    try expectPack(FixedMixed, .{ .b = true, .u8_ = 1, .u16_ = 1, .u32_ = 1, .u64_ = 1 }, "01010100010000000100000000000000");
}
test "FixedMixed/max" {
    try expectPack(FixedMixed, .{ .b = true, .u8_ = 255, .u16_ = 65535, .u32_ = 4294967295, .u64_ = 18446744073709551615 }, "01FFFFFFFFFFFFFFFFFFFFFFFFFFFFFF");
}

// ── AllPrimitives (5 cases) ─────────────────────────────────────────────

test "AllPrimitives/zeros" {
    try expectPack(AllPrimitives, .{ .b = false, .u8v = 0, .i8v = 0, .u16v = 0, .i16v = 0, .u32v = 0, .i32v = 0, .u64v = 0, .i64v = 0, .f32v = 0, .f64v = 0 }, "2B0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000");
}
test "AllPrimitives/ones" {
    try expectPack(AllPrimitives, .{ .b = true, .u8v = 1, .i8v = 1, .u16v = 1, .i16v = 1, .u32v = 1, .i32v = 1, .u64v = 1, .i64v = 1, .f32v = 1.0, .f64v = 1.0 }, "2B00010101010001000100000001000000010000000000000001000000000000000000803F000000000000F03F");
}
test "AllPrimitives/max_unsigned" {
    try expectPack(AllPrimitives, .{
        .b = true,
        .u8v = 255,
        .i8v = 127,
        .u16v = 65535,
        .i16v = 32767,
        .u32v = 4294967295,
        .i32v = 2147483647,
        .u64v = 18446744073709551615,
        .i64v = 9223372036854775807,
        .f32v = @as(f32, @bitCast(@as(u32, 0x40490FD0))),
        .f64v = @as(f64, @bitCast(@as(u64, 0x4005BF0A8B145769))),
    }, "2B0001FF7FFFFFFF7FFFFFFFFFFFFFFF7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF7FD00F49406957148B0ABF0540");
}
test "AllPrimitives/min_signed" {
    try expectPack(AllPrimitives, .{ .b = false, .u8v = 0, .i8v = -128, .u16v = 0, .i16v = -32768, .u32v = 0, .i32v = -2147483648, .u64v = 0, .i64v = -9223372036854775808, .f32v = -1.0, .f64v = -1.0 }, "2B0000008000000080000000000000008000000000000000000000000000000080000080BF000000000000F0BF");
}
test "AllPrimitives/fractional_floats" {
    try expectPack(AllPrimitives, .{
        .b = false,
        .u8v = 0,
        .i8v = 0,
        .u16v = 0,
        .i16v = 0,
        .u32v = 0,
        .i32v = 0,
        .u64v = 0,
        .i64v = 0,
        .f32v = @as(f32, @bitCast(@as(u32, 0x3DCCCCCD))),
        .f64v = @as(f64, @bitCast(@as(u64, 0x3FB999999999999A))),
    }, "2B0000000000000000000000000000000000000000000000000000000000000000CDCCCC3D9A9999999999B93F");
}

// ── SingleBool (2 cases) ───────────────────────────────────────────────

test "SingleBool/false" {
    try expectPack(SingleBool, .{ .value = false }, "00");
}
test "SingleBool/true" {
    try expectPack(SingleBool, .{ .value = true }, "01");
}

// ── SingleU32 (4 cases) ────────────────────────────────────────────────

test "SingleU32/zero" {
    try expectPack(SingleU32, .{ .value = 0 }, "00000000");
}
test "SingleU32/one" {
    try expectPack(SingleU32, .{ .value = 1 }, "01000000");
}
test "SingleU32/max" {
    try expectPack(SingleU32, .{ .value = 4294967295 }, "FFFFFFFF");
}
test "SingleU32/hex_pattern" {
    try expectPack(SingleU32, .{ .value = 3735928559 }, "EFBEADDE");
}

// ── SingleString (6 cases) ─────────────────────────────────────────────

test "SingleString/empty" {
    try expectPackAndView(SingleString, .{ .value = "" }, "040000000000");
}
test "SingleString/hello" {
    try expectPackAndView(SingleString, .{ .value = "hello" }, "0400040000000500000068656C6C6F");
}
test "SingleString/with_spaces" {
    try expectPackAndView(SingleString, .{ .value = "hello world" }, "0400040000000B00000068656C6C6F20776F726C64");
}
test "SingleString/special_chars" {
    try expectPackAndView(SingleString, .{ .value = "tab\there\nnewline" }, "0400040000001000000074616209686572650A6E65776C696E65");
}
test "SingleString/unicode" {
    try expectPackAndView(SingleString, .{ .value = "caf\xc3\xa9 \xe2\x98\x95 \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e" }, "04000400000013000000636166C3A920E2989520E697A5E69CACE8AA9E");
}
test "SingleString/escapes" {
    try expectPackAndView(SingleString, .{ .value = "quote\"backslash\\" }, "0400040000001000000071756F7465226261636B736C6173685C");
}

// ── WithStrings (2 cases) ──────────────────────────────────────────────

test "WithStrings/all_empty" {
    try expectPackAndView(WithStrings, .{ .empty_str = "", .hello = "", .unicode = "" }, "0C00000000000000000000000000");
}
test "WithStrings/mixed" {
    try expectPackAndView(WithStrings, .{ .empty_str = "", .hello = "hello", .unicode = "\xc3\xa9mojis: \xf0\x9f\x8e\x89\xf0\x9f\x9a\x80" }, "0C0000000000080000000D0000000500000068656C6C6F11000000C3A96D6F6A69733A20F09F8E89F09F9A80");
}

// ── WithVectors (5 cases) ──────────────────────────────────────────────

test "WithVectors/both_empty" {
    try expectPackAndView(WithVectors, .{ .ints = &.{}, .strings = &.{} }, "08000000000000000000");
}
test "WithVectors/ints_only" {
    try expectPackAndView(WithVectors, .{ .ints = &.{ 1, 2, 3 }, .strings = &.{} }, "080008000000000000000C000000010000000200000003000000");
}
test "WithVectors/strings_only" {
    const strs: []const []const u8 = &.{ "a", "bb", "ccc" };
    try expectPackAndView(WithVectors, .{ .ints = &.{}, .strings = strs }, "080000000000040000000C0000000C0000000D0000000F000000010000006102000000626203000000636363");
}
test "WithVectors/both_filled" {
    const strs: []const []const u8 = &.{ "hello", "world" };
    try expectPackAndView(WithVectors, .{ .ints = &.{ 10, 20 }, .strings = strs }, "08000800000010000000080000000A0000001400000008000000080000000D0000000500000068656C6C6F05000000776F726C64");
}
test "WithVectors/single_elements" {
    const strs: []const []const u8 = &.{"only"};
    try expectPackAndView(WithVectors, .{ .ints = &.{42}, .strings = strs }, "0800080000000C000000040000002A0000000400000004000000040000006F6E6C79");
}

// ── WithOptionals (5 cases) ────────────────────────────────────────────

test "WithOptionals/both_null" {
    try expectPackAndView(WithOptionals, .{ .opt_int = null, .opt_str = null }, "0000");
}
test "WithOptionals/int_only" {
    try expectPack(WithOptionals, .{ .opt_int = 42, .opt_str = null }, "0400040000002A000000");
}
test "WithOptionals/str_only" {
    try expectPack(WithOptionals, .{ .opt_int = null, .opt_str = "hello" }, "080001000000040000000500000068656C6C6F");
}
test "WithOptionals/both_present" {
    try expectPack(WithOptionals, .{ .opt_int = 99, .opt_str = "world" }, "080008000000080000006300000005000000776F726C64");
}
test "WithOptionals/zero_int" {
    try expectPack(WithOptionals, .{ .opt_int = 0, .opt_str = null }, "04000400000000000000");
}

// ── Inner (3 cases) ────────────────────────────────────────────────────

test "Inner/simple" {
    try expectPackAndView(Inner, .{ .value = 42, .label = "hello" }, "08002A000000040000000500000068656C6C6F");
}
test "Inner/empty_label" {
    try expectPackAndView(Inner, .{ .value = 0, .label = "" }, "08000000000000000000");
}
test "Inner/max_value" {
    try expectPackAndView(Inner, .{ .value = 4294967295, .label = "max" }, "0800FFFFFFFF04000000030000006D6178");
}

// ── Outer (3 cases) ────────────────────────────────────────────────────

test "Outer/simple" {
    try expectPackAndView(Outer, .{ .inner = .{ .value = 1, .label = "inner" }, .name = "outer" }, "080008000000170000000800010000000400000005000000696E6E6572050000006F75746572");
}
test "Outer/empty_strings" {
    try expectPackAndView(Outer, .{ .inner = .{ .value = 0, .label = "" }, .name = "" }, "0800080000000000000008000000000000000000");
}
test "Outer/nested_unicode" {
    try expectPackAndView(Outer, .{ .inner = .{ .value = 42, .label = "caf\xc3\xa9" }, .name = "na\xc3\xafve" }, "0800080000001700000008002A0000000400000005000000636166C3A9060000006E61C3AF7665");
}

// ── WithVariant (5 cases) ──────────────────────────────────────────────

test "WithVariant/uint32_alt" {
    try expectPack(WithVariant, .{ .data = .{ .uint32 = 42 } }, "04000400000000040000002A000000");
}
test "WithVariant/string_alt" {
    try expectPack(WithVariant, .{ .data = .{ .string = "hello" } }, "04000400000001090000000500000068656C6C6F");
}
test "WithVariant/struct_alt" {
    try expectPack(WithVariant, .{ .data = .{ .Inner = .{ .value = 7, .label = "variant_inner" } } }, "040004000000021B000000080007000000040000000D00000076617269616E745F696E6E6572");
}
test "WithVariant/uint32_zero" {
    try expectPack(WithVariant, .{ .data = .{ .uint32 = 0 } }, "040004000000000400000000000000");
}
test "WithVariant/string_empty" {
    try expectPack(WithVariant, .{ .data = .{ .string = "" } }, "040004000000010400000000000000");
}

// ── VecOfStructs (3 cases) ─────────────────────────────────────────────

test "VecOfStructs/empty" {
    try expectPack(VecOfStructs, .{ .items = &.{} }, "040000000000");
}
test "VecOfStructs/single" {
    const items: []const Inner = &.{.{ .value = 1, .label = "one" }};
    try expectPack(VecOfStructs, .{ .items = items }, "040004000000040000000400000008000100000004000000030000006F6E65");
}
test "VecOfStructs/multiple" {
    const items: []const Inner = &.{
        .{ .value = 1, .label = "one" },
        .{ .value = 2, .label = "two" },
        .{ .value = 3, .label = "three" },
    };
    try expectPack(VecOfStructs, .{ .items = items }, "0400040000000C0000000C000000190000002600000008000100000004000000030000006F6E65080002000000040000000300000074776F08000300000004000000050000007468726565");
}

// ── OptionalStruct (2 cases) ───────────────────────────────────────────

test "OptionalStruct/null" {
    try expectPack(OptionalStruct, .{ .item = null }, "0000");
}
test "OptionalStruct/present" {
    try expectPack(OptionalStruct, .{ .item = .{ .value = 42, .label = "exists" } }, "04000400000008002A0000000400000006000000657869737473");
}

// ── VecOfOptionals (4 cases) ───────────────────────────────────────────

test "VecOfOptionals/empty" {
    try expectPack(VecOfOptionals, .{ .items = &.{} }, "040000000000");
}
test "VecOfOptionals/all_null" {
    const items: []const ?u32 = &.{ null, null, null };
    try expectPack(VecOfOptionals, .{ .items = items }, "0400040000000C000000010000000100000001000000");
}
test "VecOfOptionals/all_present" {
    const items: []const ?u32 = &.{ 1, 2, 3 };
    try expectPack(VecOfOptionals, .{ .items = items }, "0400040000000C0000000C0000000C0000000C000000010000000200000003000000");
}
test "VecOfOptionals/mixed" {
    const items: []const ?u32 = &.{ 1, null, 3, null };
    try expectPack(VecOfOptionals, .{ .items = items }, "0400040000001000000010000000010000000C000000010000000100000003000000");
}

// ── OptionalVec (3 cases) ──────────────────────────────────────────────

test "OptionalVec/null" {
    try expectPack(OptionalVec, .{ .items = null }, "0000");
}
test "OptionalVec/empty_vec" {
    const empty: []const u32 = &.{};
    try expectPack(OptionalVec, .{ .items = empty }, "040000000000");
}
test "OptionalVec/with_values" {
    const vals: []const u32 = &.{ 10, 20, 30 };
    try expectPack(OptionalVec, .{ .items = vals }, "0400040000000C0000000A000000140000001E000000");
}

// ── NestedVecs (4 cases) ───────────────────────────────────────────────

test "NestedVecs/empty" {
    try expectPack(NestedVecs, .{ .matrix = &.{} }, "040000000000");
}
test "NestedVecs/empty_rows" {
    const rows: []const []const u32 = &.{ &.{}, &.{}, &.{} };
    try expectPack(NestedVecs, .{ .matrix = rows }, "0400040000000C000000000000000000000000000000");
}
test "NestedVecs/identity_2x2" {
    const rows: []const []const u32 = &.{ &.{ 1, 0 }, &.{ 0, 1 } };
    try expectPack(NestedVecs, .{ .matrix = rows }, "040004000000080000000800000010000000080000000100000000000000080000000000000001000000");
}
test "NestedVecs/ragged" {
    const rows: []const []const u32 = &.{ &.{1}, &.{ 2, 3 }, &.{ 4, 5, 6 } };
    try expectPack(NestedVecs, .{ .matrix = rows }, "0400040000000C0000000C000000100000001800000004000000010000000800000002000000030000000C000000040000000500000006000000");
}

// ── FixedArray (3 cases) ───────────────────────────────────────────────

test "FixedArray/zeros" {
    try expectPack(FixedArray, .{ .arr = .{ 0, 0, 0 } }, "000000000000000000000000");
}
test "FixedArray/sequence" {
    try expectPack(FixedArray, .{ .arr = .{ 1, 2, 3 } }, "010000000200000003000000");
}
test "FixedArray/max" {
    try expectPack(FixedArray, .{ .arr = .{ 4294967295, 4294967295, 4294967295 } }, "FFFFFFFFFFFFFFFFFFFFFFFF");
}

// ── Complex (3 cases) ──────────────────────────────────────────────────

test "Complex/all_empty" {
    const empty_items: []const Inner = &.{};
    const empty_opts: []const ?[]const u8 = &.{};
    try expectPack(Complex, .{ .items = empty_items, .opt_vec = null, .vec_opt = empty_opts, .opt_struct = null }, "0C00000000000100000000000000");
}
test "Complex/all_populated" {
    const items: []const Inner = &.{
        .{ .value = 1, .label = "a" },
        .{ .value = 2, .label = "b" },
    };
    const opt_vec: []const u32 = &.{ 10, 20 };
    const vec_opt: []const ?[]const u8 = &.{ "x", null, "z" };
    try expectPack(Complex, .{
        .items = items,
        .opt_vec = opt_vec,
        .vec_opt = vec_opt,
        .opt_struct = .{ .value = 99, .label = "present" },
    }, "100010000000360000003E00000054000000080000000800000013000000080001000000040000000100000061080002000000040000000100000062080000000A000000140000000C0000000C00000001000000090000000100000078010000007A080063000000040000000700000070726573656E74");
}
test "Complex/sparse" {
    const items: []const Inner = &.{.{ .value = 42, .label = "only" }};
    const vec_opt: []const ?[]const u8 = &.{ null, null };
    try expectPack(Complex, .{
        .items = items,
        .opt_vec = null,
        .vec_opt = vec_opt,
        .opt_struct = null,
    }, "0C000C000000010000001E000000040000000400000008002A00000004000000040000006F6E6C79080000000100000001000000");
}

// ── EmptyExtensible (2 cases) ──────────────────────────────────────────

test "EmptyExtensible/zero" {
    try expectPack(EmptyExtensible, .{ .dummy = 0 }, "040000000000");
}
test "EmptyExtensible/max" {
    try expectPack(EmptyExtensible, .{ .dummy = 4294967295 }, "0400FFFFFFFF");
}

// ── Tuple tests ────────────────────────────────────────────────────────

// Tuple (u32, u32): same wire format as extensible struct with two u32 fields
// u16 fixed_size=8, u32 elem0, u32 elem1
test "Tuple/scalar_pair" {
    const T = struct { u32, u32 };
    try expectPackAndView(T, .{ 42, 100 }, "08002A00000064000000");
}

test "Tuple/scalar_pair_zeros" {
    const T = struct { u32, u32 };
    try expectPackAndView(T, .{ 0, 0 }, "08000000000000000000");
}

// Single element tuple
test "Tuple/single_u32" {
    const T = struct { u32 };
    try expectPackAndView(T, .{42}, "04002A000000");
}

// Tuple with trailing optional elision: (u32, ?u32) with null elided
test "Tuple/trailing_optional_elided" {
    const T = struct { u32, ?u32 };
    try expectPack(T, .{ 10, @as(?u32, null) }, "04000A000000");
}

// Single bool tuple
test "Tuple/single_bool" {
    const T = struct { bool };
    try expectPackAndView(T, .{true}, "010001");
}

// Tuple with string: (u32, []const u8) — same wire format as Inner struct
test "Tuple/u32_and_string" {
    const T = struct { u32, []const u8 };
    // Same encoding as Inner{value=42, label="hello"}:
    // u16 fixed_size=8, u32 42, u32 offset=4, u32 len=5, "hello"
    try expectPackAndView(T, .{ 42, "hello" }, "08002A000000040000000500000068656C6C6F");
}

// Tuple with all trailing optionals elided
test "Tuple/all_optionals_elided" {
    const T = struct { ?u32, ?u32 };
    try expectPack(T, .{ @as(?u32, null), @as(?u32, null) }, "0000");
}

// Tuple unmarshal round-trip
test "Tuple/unmarshal_round_trip" {
    const T = struct { u32, []const u8 };
    const original: T = .{ 42, "hello" };
    const data = try fp.marshal(T, alloc, original);
    defer alloc.free(data);

    const decoded = try fp.unmarshal(T, alloc, data);
    defer fp.free(T, alloc, decoded);

    try std.testing.expectEqual(@as(u32, 42), decoded[0]);
    try std.testing.expectEqualSlices(u8, "hello", decoded[1]);
}

// Tuple view access
test "Tuple/view_access" {
    const T = struct { u32, []const u8 };
    const data = try fp.marshal(T, alloc, .{ 42, "hello" });
    defer alloc.free(data);

    const v = try fp.view(T, data);
    try std.testing.expectEqual(@as(u32, 42), v[0]);
    try std.testing.expectEqualSlices(u8, "hello", v[1]);
}
