// Validation tests for fracpack.validate().
//
// Tests zero-copy validation of packed data against type schemas:
// - valid data -> .valid
// - extended struct data -> .extended
// - various forms of corruption -> .invalid

const std = @import("std");
const fp = @import("fracpack.zig");

const alloc = std.testing.allocator;
const hexToBytes = fp.hexToBytes;
const validate = fp.validate;
const ValidationStatus = fp.ValidationStatus;

// ── Type definitions (reused from golden test) ────────────────────────

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

// ── Helper ──────────────────────────────────────────────────────────────

fn expectValid(comptime T: type, data: []const u8) !void {
    const result = validate(T, data);
    try std.testing.expectEqual(ValidationStatus.valid, result.status);
}

fn expectExtended(comptime T: type, data: []const u8) !void {
    const result = validate(T, data);
    try std.testing.expectEqual(ValidationStatus.extended, result.status);
}

fn expectInvalid(comptime T: type, data: []const u8) !void {
    const result = validate(T, data);
    try std.testing.expectEqual(ValidationStatus.invalid, result.status);
}

fn packAndValidate(comptime T: type, value: T) !void {
    const data = try fp.marshal(T, alloc, value);
    defer alloc.free(data);
    try expectValid(T, data);
}

// ── Golden vectors: all valid data should validate ─────────────────────

test "golden/FixedInts/zeros" {
    const d = hexToBytes("0000000000000000");
    try expectValid(FixedInts, &d);
}

test "golden/FixedInts/positive" {
    const d = hexToBytes("2A00000064000000");
    try expectValid(FixedInts, &d);
}

test "golden/FixedInts/negative" {
    const d = hexToBytes("FFFFFFFF00000080");
    try expectValid(FixedInts, &d);
}

test "golden/FixedInts/max" {
    const d = hexToBytes("FFFFFF7FFFFFFF7F");
    try expectValid(FixedInts, &d);
}

test "golden/FixedMixed/zeros" {
    const d = hexToBytes("00000000000000000000000000000000");
    try expectValid(FixedMixed, &d);
}

test "golden/FixedMixed/ones" {
    const d = hexToBytes("01010100010000000100000000000000");
    try expectValid(FixedMixed, &d);
}

test "golden/FixedMixed/max" {
    const d = hexToBytes("01FFFFFFFFFFFFFFFFFFFFFFFFFFFFFF");
    try expectValid(FixedMixed, &d);
}

test "golden/SingleBool/false" {
    const d = hexToBytes("00");
    try expectValid(SingleBool, &d);
}

test "golden/SingleBool/true" {
    const d = hexToBytes("01");
    try expectValid(SingleBool, &d);
}

test "golden/SingleU32/zero" {
    const d = hexToBytes("00000000");
    try expectValid(SingleU32, &d);
}

test "golden/SingleU32/max" {
    const d = hexToBytes("FFFFFFFF");
    try expectValid(SingleU32, &d);
}

test "golden/SingleString/empty" {
    const d = hexToBytes("040000000000");
    try expectValid(SingleString, &d);
}

test "golden/SingleString/hello" {
    const d = hexToBytes("0400040000000500000068656C6C6F");
    try expectValid(SingleString, &d);
}

test "golden/SingleString/unicode" {
    const d = hexToBytes("04000400000013000000636166C3A920E2989520E697A5E69CACE8AA9E");
    try expectValid(SingleString, &d);
}

test "golden/WithStrings/mixed" {
    const d = hexToBytes("0C0000000000080000000D0000000500000068656C6C6F11000000C3A96D6F6A69733A20F09F8E89F09F9A80");
    try expectValid(WithStrings, &d);
}

test "golden/WithVectors/both_empty" {
    const d = hexToBytes("08000000000000000000");
    try expectValid(WithVectors, &d);
}

test "golden/WithVectors/ints_only" {
    const d = hexToBytes("080008000000000000000C000000010000000200000003000000");
    try expectValid(WithVectors, &d);
}

test "golden/WithVectors/strings_only" {
    const d = hexToBytes("080000000000040000000C0000000C0000000D0000000F000000010000006102000000626203000000636363");
    try expectValid(WithVectors, &d);
}

test "golden/WithOptionals/both_null" {
    const d = hexToBytes("0000");
    try expectValid(WithOptionals, &d);
}

test "golden/WithOptionals/int_only" {
    const d = hexToBytes("0400040000002A000000");
    try expectValid(WithOptionals, &d);
}

test "golden/WithOptionals/str_only" {
    const d = hexToBytes("080001000000040000000500000068656C6C6F");
    try expectValid(WithOptionals, &d);
}

test "golden/WithOptionals/both_present" {
    const d = hexToBytes("080008000000080000006300000005000000776F726C64");
    try expectValid(WithOptionals, &d);
}

test "golden/Inner/simple" {
    const d = hexToBytes("08002A000000040000000500000068656C6C6F");
    try expectValid(Inner, &d);
}

test "golden/Inner/empty_label" {
    const d = hexToBytes("08000000000000000000");
    try expectValid(Inner, &d);
}

test "golden/Outer/simple" {
    const d = hexToBytes("080008000000170000000800010000000400000005000000696E6E6572050000006F75746572");
    try expectValid(Outer, &d);
}

test "golden/WithVariant/uint32_alt" {
    const d = hexToBytes("04000400000000040000002A000000");
    try expectValid(WithVariant, &d);
}

test "golden/WithVariant/string_alt" {
    const d = hexToBytes("04000400000001090000000500000068656C6C6F");
    try expectValid(WithVariant, &d);
}

test "golden/WithVariant/struct_alt" {
    const d = hexToBytes("040004000000021B000000080007000000040000000D00000076617269616E745F696E6E6572");
    try expectValid(WithVariant, &d);
}

test "golden/VecOfStructs/empty" {
    const d = hexToBytes("040000000000");
    try expectValid(VecOfStructs, &d);
}

test "golden/VecOfStructs/single" {
    const d = hexToBytes("040004000000040000000400000008000100000004000000030000006F6E65");
    try expectValid(VecOfStructs, &d);
}

test "golden/VecOfStructs/multiple" {
    const d = hexToBytes("0400040000000C0000000C000000190000002600000008000100000004000000030000006F6E65080002000000040000000300000074776F08000300000004000000050000007468726565");
    try expectValid(VecOfStructs, &d);
}

test "golden/OptionalStruct/null" {
    const d = hexToBytes("0000");
    try expectValid(OptionalStruct, &d);
}

test "golden/OptionalStruct/present" {
    const d = hexToBytes("04000400000008002A0000000400000006000000657869737473");
    try expectValid(OptionalStruct, &d);
}

test "golden/VecOfOptionals/empty" {
    const d = hexToBytes("040000000000");
    try expectValid(VecOfOptionals, &d);
}

test "golden/VecOfOptionals/all_null" {
    const d = hexToBytes("0400040000000C000000010000000100000001000000");
    try expectValid(VecOfOptionals, &d);
}

test "golden/VecOfOptionals/all_present" {
    const d = hexToBytes("0400040000000C0000000C0000000C0000000C000000010000000200000003000000");
    try expectValid(VecOfOptionals, &d);
}

test "golden/VecOfOptionals/mixed" {
    const d = hexToBytes("0400040000001000000010000000010000000C000000010000000100000003000000");
    try expectValid(VecOfOptionals, &d);
}

test "golden/OptionalVec/null" {
    const d = hexToBytes("0000");
    try expectValid(OptionalVec, &d);
}

test "golden/OptionalVec/empty_vec" {
    const d = hexToBytes("040000000000");
    try expectValid(OptionalVec, &d);
}

test "golden/OptionalVec/with_values" {
    const d = hexToBytes("0400040000000C0000000A000000140000001E000000");
    try expectValid(OptionalVec, &d);
}

test "golden/NestedVecs/identity_2x2" {
    const d = hexToBytes("040004000000080000000800000010000000080000000100000000000000080000000000000001000000");
    try expectValid(NestedVecs, &d);
}

test "golden/NestedVecs/ragged" {
    const d = hexToBytes("0400040000000C0000000C000000100000001800000004000000010000000800000002000000030000000C000000040000000500000006000000");
    try expectValid(NestedVecs, &d);
}

test "golden/FixedArray/sequence" {
    const d = hexToBytes("010000000200000003000000");
    try expectValid(FixedArray, &d);
}

test "golden/Complex/all_empty" {
    const d = hexToBytes("0C00000000000100000000000000");
    try expectValid(Complex, &d);
}

test "golden/Complex/all_populated" {
    const d = hexToBytes("100010000000360000003E00000054000000080000000800000013000000080001000000040000000100000061080002000000040000000100000062080000000A000000140000000C0000000C00000001000000090000000100000078010000007A080063000000040000000700000070726573656E74");
    try expectValid(Complex, &d);
}

test "golden/AllPrimitives/zeros" {
    const d = hexToBytes("2B0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000");
    try expectValid(AllPrimitives, &d);
}

test "golden/AllPrimitives/ones" {
    const d = hexToBytes("2B00010101010001000100000001000000010000000000000001000000000000000000803F000000000000F03F");
    try expectValid(AllPrimitives, &d);
}

// ── Tuple golden vectors ────────────────────────────────────────────────

test "golden/Tuple/scalar_pair" {
    const T = struct { u32, u32 };
    const d = hexToBytes("08002A00000064000000");
    try expectValid(T, &d);
}

test "golden/Tuple/single_u32" {
    const T = struct { u32 };
    const d = hexToBytes("04002A000000");
    try expectValid(T, &d);
}

test "golden/Tuple/trailing_optional_elided" {
    const T = struct { u32, ?u32 };
    const d = hexToBytes("04000A000000");
    try expectValid(T, &d);
}

test "golden/Tuple/all_optionals_elided" {
    const T = struct { ?u32, ?u32 };
    const d = hexToBytes("0000");
    try expectValid(T, &d);
}

// ── Truncated data -> invalid ──────────────────────────────────────────

test "invalid/truncated/FixedInts" {
    // FixedInts needs 8 bytes, give it 4
    const d = hexToBytes("2A000000");
    try expectInvalid(FixedInts, &d);
}

test "invalid/truncated/SingleU32" {
    // Needs 4 bytes, give 2
    const d = hexToBytes("2A00");
    try expectInvalid(SingleU32, &d);
}

test "invalid/truncated/extensible_header" {
    // Extensible struct: u16 header says 8 bytes of fixed, but only 4 bytes present
    const d = hexToBytes("08002A000000");
    try expectInvalid(Inner, &d);
}

test "invalid/truncated/string_length" {
    // SingleString: u16 fixed=4, offset=4 -> target at byte 6, but string length truncated
    const d = hexToBytes("040004000000050000");
    try expectInvalid(SingleString, &d);
}

test "invalid/truncated/string_data" {
    // String says 5 bytes but only 3 present
    const d = hexToBytes("0400040000000500000068656C");
    try expectInvalid(SingleString, &d);
}

test "invalid/truncated/empty_buffer" {
    try expectInvalid(SingleU32, &.{});
}

test "invalid/truncated/extensible_empty" {
    try expectInvalid(Inner, &.{});
}

// ── Extra trailing bytes -> invalid ────────────────────────────────────

test "invalid/extra_bytes/FixedInts" {
    const d = hexToBytes("000000000000000000"); // 9 bytes, should be 8
    try expectInvalid(FixedInts, &d);
}

test "invalid/extra_bytes/SingleU32" {
    const d = hexToBytes("2A00000000"); // 5 bytes, should be 4
    try expectInvalid(SingleU32, &d);
}

test "invalid/extra_bytes/extensible" {
    // Inner{value=0, label=""}: correct is "08000000000000000000", add a trailing byte
    const d = hexToBytes("0800000000000000000000");
    try expectInvalid(Inner, &d);
}

// ── Bad offset (past buffer) -> invalid ────────────────────────────────

test "invalid/bad_offset/string_past_end" {
    // SingleString with offset pointing way past buffer end
    // u16 fixed=4, offset=0xFF000000 (huge)
    const d = hexToBytes("0400000000FF");
    try expectInvalid(SingleString, &d);
}

test "invalid/bad_offset/struct_past_end" {
    // Outer: inner offset points past buffer
    const d = hexToBytes("0800FF00000004000000");
    try expectInvalid(Outer, &d);
}

// ── Bad UTF-8 -> invalid ───────────────────────────────────────────────

test "invalid/bad_utf8/continuation_only" {
    // SingleString with bytes that aren't valid UTF-8 (bare continuation byte 0x80)
    // u16 fixed_size=4, offset=4, then string: len=1, byte=0x80
    const d = hexToBytes("04000400000001000000" ++ "80");
    try expectInvalid(SingleString, &d);
}

test "invalid/bad_utf8/truncated_multibyte" {
    // 2-byte lead (0xC3) but no continuation — string len=1
    const d = hexToBytes("04000400000001000000" ++ "C3");
    try expectInvalid(SingleString, &d);
}

test "invalid/bad_utf8/overlong" {
    // Overlong encoding of '/' (0x2F): 0xC0 0xAF
    const d = hexToBytes("04000400000002000000" ++ "C0AF");
    try expectInvalid(SingleString, &d);
}

// ── Bad bool (value 2) -> invalid ──────────────────────────────────────

test "invalid/bad_bool/value_2" {
    const d = [_]u8{2};
    try expectInvalid(SingleBool, &d);
}

test "invalid/bad_bool/value_255" {
    const d = [_]u8{0xFF};
    try expectInvalid(SingleBool, &d);
}

test "invalid/bad_bool/in_extensible_struct" {
    // AllPrimitives first field is bool. Set it to 2.
    // u16 fixed_size=0x2B, then bool=2, rest zeros
    var d = hexToBytes("2B0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000");
    d[2] = 2; // corrupt the bool field
    try expectInvalid(AllPrimitives, &d);
}

// ── Bad variant tag -> invalid ─────────────────────────────────────────

test "invalid/bad_variant_tag/out_of_range" {
    // WithVariant: offset=4 -> variant at byte 6
    // Variant: tag=3 (DataVariant has 3 cases: 0,1,2), size=4, data=00000000
    const d = hexToBytes("040004000000" ++ "03" ++ "04000000" ++ "00000000");
    try expectInvalid(WithVariant, &d);
}

test "invalid/bad_variant_tag/tag_127" {
    // tag=127 is out of range for DataVariant (3 cases)
    const d = hexToBytes("040004000000" ++ "7F" ++ "04000000" ++ "00000000");
    try expectInvalid(WithVariant, &d);
}

// ── Extended struct detection ──────────────────────────────────────────

test "extended/struct_with_extra_fixed_bytes" {
    // Inner has fixed_size=8 (u32 + offset). If we claim fixed_size=12 with 4 extra
    // bytes, it should be 'extended'.
    // u16 fixed=12, u32 value=42, u32 label_offset, u32 unknown=0, then string on heap
    //
    // fixed_start = 2, label at offset 2+4=6, label_offset = 8 (points to byte 14)
    // heap starts at 2+12=14
    // label data: len=5, "hello"
    const d = hexToBytes("0C002A0000000800000000000000" ++ "0500000068656C6C6F");
    try expectExtended(Inner, &d);
}

test "extended/nested_extended" {
    // Pack a normal Outer, but with the inner struct having extra fixed bytes
    // Build manually:
    // Outer: u16 fixed=8, inner_offset(4), name_offset(4)
    // inner_offset points to inner struct
    // inner struct: u16 fixed=12 (extended from 8), u32 value, u32 label_offset, u32 unknown
    // then label string, then name string
    //
    // Let's build this carefully:
    // Outer header: 08 00 (fixed=8)
    // inner_offset at pos 2: offset to inner (will be at heap, which is at pos 10)
    //   inner_offset = 10 - 2 = 8
    // name_offset at pos 6: offset to name string (after inner data)
    //   inner is: 0C 00, 2A000000, 08000000, 00000000, 0500000068656C6C6F = 2+12+9 = 23 bytes
    //   name at pos 10+23 = 33, name_offset = 33 - 6 = 27
    // name string: 05000000776F726C64
    const d = hexToBytes(
        "0800" ++ // Outer: fixed=8
        "08000000" ++ // inner offset = 8 (from pos 2 -> target pos 10)
        "1B000000" ++ // name offset = 27 (from pos 6 -> target pos 33)
        "0C00" ++ // Inner: fixed=12 (extended from 8)
        "2A000000" ++ // Inner.value = 42
        "08000000" ++ // Inner.label offset = 8 (from pos 16 -> target pos 24)
        "00000000" ++ // unknown extra field
        "0500000068656C6C6F" ++ // "hello"
        "05000000776F726C64", // "world"
    );
    try expectExtended(Outer, &d);
}

// ── Vec size alignment ─────────────────────────────────────────────────

test "invalid/vec_size_misaligned" {
    // WithVectors: ints vec with data_size=5 (not divisible by 4 for u32)
    // u16 fixed=8, ints_offset=8 (points to byte 10), strings_offset=0
    // vec at byte 10: data_size=5
    const d = hexToBytes("0800080000000000000005000000" ++ "0000000000");
    try expectInvalid(WithVectors, &d);
}

// ── Reserved offset values (2, 3) -> invalid ──────────────────────────

test "invalid/reserved_offset_2" {
    // WithOptionals: opt_int offset = 2 (reserved)
    // u16 fixed=4, offset=2
    const d = hexToBytes("040002000000");
    try expectInvalid(WithOptionals, &d);
}

test "invalid/reserved_offset_3" {
    // WithOptionals: opt_int offset = 3 (reserved)
    const d = hexToBytes("040003000000");
    try expectInvalid(WithOptionals, &d);
}

// ── Round-trip: marshal then validate ──────────────────────────────────

test "roundtrip/Inner" {
    try packAndValidate(Inner, .{ .value = 42, .label = "hello" });
}

test "roundtrip/Outer" {
    try packAndValidate(Outer, .{
        .inner = .{ .value = 1, .label = "inner" },
        .name = "outer",
    });
}

test "roundtrip/WithOptionals/both_null" {
    try packAndValidate(WithOptionals, .{ .opt_int = null, .opt_str = null });
}

test "roundtrip/WithOptionals/both_present" {
    try packAndValidate(WithOptionals, .{ .opt_int = 99, .opt_str = "world" });
}

test "roundtrip/WithVariant/string" {
    try packAndValidate(WithVariant, .{ .data = .{ .string = "test" } });
}

test "roundtrip/WithVariant/struct" {
    try packAndValidate(WithVariant, .{ .data = .{ .Inner = .{ .value = 7, .label = "variant" } } });
}

test "roundtrip/VecOfStructs" {
    const items: []const Inner = &.{
        .{ .value = 1, .label = "one" },
        .{ .value = 2, .label = "two" },
    };
    try packAndValidate(VecOfStructs, .{ .items = items });
}

test "roundtrip/Complex/all_populated" {
    const items: []const Inner = &.{
        .{ .value = 1, .label = "a" },
        .{ .value = 2, .label = "b" },
    };
    const opt_vec: []const u32 = &.{ 10, 20 };
    const vec_opt: []const ?[]const u8 = &.{ "x", null, "z" };
    try packAndValidate(Complex, .{
        .items = items,
        .opt_vec = opt_vec,
        .vec_opt = vec_opt,
        .opt_struct = .{ .value = 99, .label = "present" },
    });
}

test "roundtrip/NestedVecs" {
    const rows: []const []const u32 = &.{ &.{ 1, 0 }, &.{ 0, 1 } };
    try packAndValidate(NestedVecs, .{ .matrix = rows });
}

test "roundtrip/Tuple" {
    const T = struct { u32, []const u8 };
    const data = try fp.marshal(T, alloc, .{ 42, "hello" });
    defer alloc.free(data);
    try expectValid(T, data);
}

test "roundtrip/FixedArray" {
    try packAndValidate(FixedArray, .{ .arr = .{ 1, 2, 3 } });
}
