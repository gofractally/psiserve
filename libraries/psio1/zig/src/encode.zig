// Encode WIT (WebAssembly Interface Types) to Component Model binary format.
//
// Takes a WitWorld and serializes it to the standard component-type custom
// section binary encoding. This is the same format that wasm-tools and
// wit-component produce.

const std = @import("std");
const types = @import("types.zig");

pub const WitPrim = types.WitPrim;
pub const WitTypeKind = types.WitTypeKind;
pub const WitNamedType = types.WitNamedType;
pub const WitTypeDef = types.WitTypeDef;
pub const WitFunc = types.WitFunc;
pub const WitInterface = types.WitInterface;
pub const WitWorld = types.WitWorld;
pub const primIdx = types.primIdx;
pub const idxToPrim = types.idxToPrim;
pub const isPrimIdx = types.isPrimIdx;

// Component Model binary opcodes
pub const cm = struct {
    // Section IDs
    pub const section_custom: u8 = 0x00;
    pub const section_type: u8 = 0x07;
    pub const section_export: u8 = 0x0b;

    // Container type constructors
    pub const type_component: u8 = 0x41;
    pub const type_instance: u8 = 0x42;
    pub const type_func: u8 = 0x40;

    // Defined value type constructors (negative SLEB128)
    pub const def_record: u8 = 0x72;
    pub const def_variant: u8 = 0x71;
    pub const def_list: u8 = 0x70;
    pub const def_tuple: u8 = 0x6f;
    pub const def_flags: u8 = 0x6e;
    pub const def_enum_: u8 = 0x6d;
    pub const def_option: u8 = 0x6b;
    pub const def_result: u8 = 0x6a;

    // Primitive value types (negative SLEB128)
    pub const prim_bool: u8 = 0x7f;
    pub const prim_s8: u8 = 0x7e;
    pub const prim_u8: u8 = 0x7d;
    pub const prim_s16: u8 = 0x7c;
    pub const prim_u16: u8 = 0x7b;
    pub const prim_s32: u8 = 0x7a;
    pub const prim_u32: u8 = 0x79;
    pub const prim_s64: u8 = 0x78;
    pub const prim_u64: u8 = 0x77;
    pub const prim_f32: u8 = 0x76;
    pub const prim_f64: u8 = 0x75;
    pub const prim_char: u8 = 0x74;
    pub const prim_string: u8 = 0x73;

    // Instance/component item tags
    pub const item_type_def: u8 = 0x01;
    pub const item_export: u8 = 0x04;

    // Export sort
    pub const sort_func: u8 = 0x01;
    pub const sort_type: u8 = 0x03;
    pub const sort_component: u8 = 0x04;
    pub const sort_instance: u8 = 0x05;

    // Extern name discriminant
    pub const name_kebab: u8 = 0x00;

    // Type bound
    pub const bound_eq: u8 = 0x00;

    // Function result
    pub const result_single: u8 = 0x00;
    pub const result_named: u8 = 0x01;
};

/// Map from WitPrim to Component Model binary opcode.
fn primToCmByte(p: WitPrim) u8 {
    return switch (p) {
        .bool_ => cm.prim_bool,
        .u8_ => cm.prim_u8,
        .s8 => cm.prim_s8,
        .u16_ => cm.prim_u16,
        .s16 => cm.prim_s16,
        .u32_ => cm.prim_u32,
        .s32 => cm.prim_s32,
        .u64_ => cm.prim_u64,
        .s64 => cm.prim_s64,
        .f32_ => cm.prim_f32,
        .f64_ => cm.prim_f64,
        .char_ => cm.prim_char,
        .string_ => cm.prim_string,
    };
}

/// Binary writer backed by an ArrayList(u8).
const Writer = struct {
    buf: std.ArrayList(u8),
    alloc: std.mem.Allocator,

    fn init(allocator: std.mem.Allocator) Writer {
        return .{ .buf = .empty, .alloc = allocator };
    }

    fn deinit(self: *Writer) void {
        self.buf.deinit(self.alloc);
    }

    fn emitByte(self: *Writer, b: u8) !void {
        try self.buf.append(self.alloc, b);
    }

    fn emitUleb128(self: *Writer, val: u32) !void {
        var v = val;
        while (true) {
            var b: u8 = @intCast(v & 0x7f);
            v >>= 7;
            if (v != 0) b |= 0x80;
            try self.buf.append(self.alloc, b);
            if (v == 0) break;
        }
    }

    fn emitString(self: *Writer, s: []const u8) !void {
        try self.emitUleb128(@intCast(s.len));
        try self.buf.appendSlice(self.alloc, s);
    }

    fn emitBytes(self: *Writer, data: []const u8) !void {
        try self.buf.appendSlice(self.alloc, data);
    }

    fn size(self: *const Writer) usize {
        return self.buf.items.len;
    }

    /// Transfer ownership of the buffer to the caller.
    fn toOwnedSlice(self: *Writer) ![]u8 {
        return try self.buf.toOwnedSlice(self.alloc);
    }

    /// Get a read-only view of the current buffer contents.
    fn items(self: *const Writer) []const u8 {
        return self.buf.items;
    }
};

/// Encode a WitWorld into Component Model binary format.
/// Returns owned slice; caller must free with the same allocator.
pub fn encode(world: *const WitWorld, allocator: std.mem.Allocator) ![]u8 {
    var out = Writer.init(allocator);
    defer out.deinit();

    // Component header: \x00asm\x0d\x00\x01\x00
    try out.emitByte(0x00);
    try out.emitByte(0x61);
    try out.emitByte(0x73);
    try out.emitByte(0x6d);
    try out.emitByte(0x0d);
    try out.emitByte(0x00);
    try out.emitByte(0x01);
    try out.emitByte(0x00);

    // Custom section: wit-component-encoding (version 4, UTF-8)
    {
        var cs = Writer.init(allocator);
        defer cs.deinit();
        try cs.emitString("wit-component-encoding");
        try cs.emitByte(0x04); // version
        try cs.emitByte(0x00); // encoding: UTF-8
        try out.emitByte(cm.section_custom);
        try out.emitUleb128(@intCast(cs.size()));
        try out.emitBytes(cs.items());
    }

    // Build the instance for the first export interface
    if (world.exports.len == 0) return try out.toOwnedSlice();

    const exp_iface = &world.exports[0];
    const inst = try encodeInterface(world, exp_iface, allocator);
    defer allocator.free(inst.bytes);

    // Type section: 1 type = outer COMPONENT
    {
        var type_sec = Writer.init(allocator);
        defer type_sec.deinit();
        try type_sec.emitUleb128(1); // 1 type

        // Outer component (2 items: inner component + world export)
        try type_sec.emitByte(cm.type_component);
        try type_sec.emitUleb128(2);

        // Item 0: inner component (2 items: instance + interface export)
        try type_sec.emitByte(cm.item_type_def);
        try type_sec.emitByte(cm.type_component);
        try type_sec.emitUleb128(2);

        // Inner item 0: instance type
        try type_sec.emitByte(cm.item_type_def);
        try type_sec.emitByte(cm.type_instance);
        try type_sec.emitUleb128(inst.item_count);
        try type_sec.emitBytes(inst.bytes);

        // Inner item 1: export interface as instance(0)
        try type_sec.emitByte(cm.item_export);
        const iface_name = try qualifiedInterfaceName(world, exp_iface, allocator);
        defer allocator.free(iface_name);
        try emitExternName(&type_sec, iface_name);
        try type_sec.emitByte(cm.sort_instance);
        try type_sec.emitUleb128(0);

        // Outer item 1: export world as component(0)
        try type_sec.emitByte(cm.item_export);
        const w_name = try qualifiedWorldName(world, allocator);
        defer allocator.free(w_name);
        try emitExternName(&type_sec, w_name);
        try type_sec.emitByte(cm.sort_component);
        try type_sec.emitUleb128(0);

        // Write the type section
        try out.emitByte(cm.section_type);
        try out.emitUleb128(@intCast(type_sec.size()));
        try out.emitBytes(type_sec.items());
    }

    // Export section: export world name as type(0)
    {
        var exp_sec = Writer.init(allocator);
        defer exp_sec.deinit();
        try exp_sec.emitUleb128(1); // 1 export
        try emitExternName(&exp_sec, world.name);
        try exp_sec.emitByte(cm.sort_type);
        try exp_sec.emitUleb128(0); // type index 0
        try exp_sec.emitByte(0x00); // no explicit type annotation

        try out.emitByte(cm.section_export);
        try out.emitUleb128(@intCast(exp_sec.size()));
        try out.emitBytes(exp_sec.items());
    }

    // Custom section: producers
    {
        var cs = Writer.init(allocator);
        defer cs.deinit();
        try cs.emitString("producers");
        try cs.emitUleb128(1); // 1 field
        try cs.emitString("processed-by");
        try cs.emitUleb128(1); // 1 entry
        try cs.emitString("psio-wit-gen");
        try cs.emitString("1.0.0");

        try out.emitByte(cm.section_custom);
        try out.emitUleb128(@intCast(cs.size()));
        try out.emitBytes(cs.items());
    }

    return try out.toOwnedSlice();
}

const InstanceEncoding = struct {
    bytes: []u8,
    item_count: u32,
};

/// Emit a valtype reference: primitive byte or type index.
fn emitValtype(w: *Writer, type_idx: i32, remap: *const std.AutoHashMap(i32, u32)) !void {
    if (isPrimIdx(type_idx)) {
        try w.emitByte(primToCmByte(idxToPrim(type_idx)));
    } else {
        const idx = remap.get(type_idx) orelse @as(u32, @intCast(type_idx));
        try w.emitUleb128(idx);
    }
}

/// Emit a defined type (record, list, option, etc.).
fn emitDefinedType(w: *Writer, td: *const WitTypeDef, remap: *const std.AutoHashMap(i32, u32)) !void {
    switch (td.kind) {
        .record => {
            try w.emitByte(cm.def_record);
            try w.emitUleb128(@intCast(td.fields.len));
            for (td.fields) |f| {
                try w.emitString(f.name);
                try emitValtype(w, f.type_idx, remap);
            }
        },
        .list => {
            try w.emitByte(cm.def_list);
            try emitValtype(w, td.element_type_idx, remap);
        },
        .option => {
            try w.emitByte(cm.def_option);
            try emitValtype(w, td.element_type_idx, remap);
        },
        .result => {
            try w.emitByte(cm.def_result);
            try emitValtype(w, td.element_type_idx, remap);
            try emitValtype(w, td.error_type_idx, remap);
        },
        .variant => {
            try w.emitByte(cm.def_variant);
            try w.emitUleb128(@intCast(td.fields.len));
            for (td.fields) |f| {
                try w.emitString(f.name);
                try emitValtype(w, f.type_idx, remap);
            }
        },
        .enum_ => {
            try w.emitByte(cm.def_enum_);
            try w.emitUleb128(@intCast(td.fields.len));
            for (td.fields) |f| {
                try w.emitString(f.name);
            }
        },
        .flags => {
            try w.emitByte(cm.def_flags);
            try w.emitUleb128(@intCast(td.fields.len));
            for (td.fields) |f| {
                try w.emitString(f.name);
            }
        },
        .tuple => {
            try w.emitByte(cm.def_tuple);
            try w.emitUleb128(@intCast(td.fields.len));
            for (td.fields) |f| {
                try emitValtype(w, f.type_idx, remap);
            }
        },
    }
}

/// Emit a func type.
fn emitFuncType(w: *Writer, func: *const WitFunc, remap: *const std.AutoHashMap(i32, u32)) !void {
    try w.emitByte(cm.type_func);
    // Parameters
    try w.emitUleb128(@intCast(func.params.len));
    for (func.params) |p| {
        try w.emitString(p.name);
        try emitValtype(w, p.type_idx, remap);
    }
    // Results
    if (func.results.len == 1 and func.results[0].name.len == 0) {
        // Single unnamed result
        try w.emitByte(cm.result_single);
        try emitValtype(w, func.results[0].type_idx, remap);
    } else {
        // Named results (or empty = void)
        try w.emitByte(cm.result_named);
        try w.emitUleb128(@intCast(func.results.len));
        for (func.results) |r| {
            try w.emitString(r.name);
            try emitValtype(w, r.type_idx, remap);
        }
    }
}

/// Emit an export name (discriminant + string).
fn emitExternName(w: *Writer, name: []const u8) !void {
    try w.emitByte(cm.name_kebab);
    try w.emitString(name);
}

/// Ensure a type is emitted, recursively emitting dependencies first.
fn ensureEmitted(
    world: *const WitWorld,
    type_idx: i32,
    w: *Writer,
    remap: *std.AutoHashMap(i32, u32),
    next_type_idx: *u32,
    item_count: *u32,
) !void {
    if (isPrimIdx(type_idx)) return;
    if (remap.get(type_idx) != null) return;

    const idx = @as(usize, @intCast(type_idx));
    if (idx >= world.types.len) return;

    const td = &world.types[idx];

    // Ensure dependencies are emitted first
    switch (td.kind) {
        .record => {
            for (td.fields) |f| {
                try ensureEmitted(world, f.type_idx, w, remap, next_type_idx, item_count);
            }
        },
        .list, .option => {
            try ensureEmitted(world, td.element_type_idx, w, remap, next_type_idx, item_count);
        },
        .result => {
            try ensureEmitted(world, td.element_type_idx, w, remap, next_type_idx, item_count);
            try ensureEmitted(world, td.error_type_idx, w, remap, next_type_idx, item_count);
        },
        .variant, .tuple => {
            for (td.fields) |f| {
                try ensureEmitted(world, f.type_idx, w, remap, next_type_idx, item_count);
            }
        },
        .enum_, .flags => {},
    }

    // Dependency resolution may have emitted this type
    if (remap.get(type_idx) != null) return;

    try w.emitByte(cm.item_type_def);
    try emitDefinedType(w, td, remap);
    const def_idx = next_type_idx.*;
    next_type_idx.* += 1;
    item_count.* += 1;

    if (td.name.len > 0) {
        try w.emitByte(cm.item_export);
        try emitExternName(w, td.name);
        try w.emitByte(cm.sort_type);
        try w.emitByte(cm.bound_eq);
        try w.emitUleb128(def_idx);
        try remap.put(type_idx, next_type_idx.*);
        next_type_idx.* += 1;
        item_count.* += 1;
    } else {
        try remap.put(type_idx, def_idx);
    }
}

/// Encode an interface as an instance type body.
fn encodeInterface(
    world: *const WitWorld,
    iface: *const WitInterface,
    allocator: std.mem.Allocator,
) !InstanceEncoding {
    var remap = std.AutoHashMap(i32, u32).init(allocator);
    defer remap.deinit();
    var next_type_idx: u32 = 0;
    var item_count: u32 = 0;
    var w = Writer.init(allocator);
    defer w.deinit();

    // Phase 1: Emit all named record types (and their dependencies)
    for (iface.type_idxs) |type_idx| {
        try ensureEmitted(world, @intCast(type_idx), &w, &remap, &next_type_idx, &item_count);
    }

    // Phase 2: Emit functions, lazily emitting remaining anonymous types
    for (iface.func_idxs) |func_idx| {
        if (func_idx >= world.funcs.len) continue;
        const func = &world.funcs[func_idx];

        for (func.params) |p| {
            try ensureEmitted(world, p.type_idx, &w, &remap, &next_type_idx, &item_count);
        }
        for (func.results) |r| {
            try ensureEmitted(world, r.type_idx, &w, &remap, &next_type_idx, &item_count);
        }

        try w.emitByte(cm.item_type_def);
        try emitFuncType(&w, func, &remap);
        const func_type_idx = next_type_idx;
        next_type_idx += 1;
        item_count += 1;

        try w.emitByte(cm.item_export);
        try emitExternName(&w, func.name);
        try w.emitByte(cm.sort_func);
        try w.emitUleb128(func_type_idx);
        item_count += 1;
    }

    const bytes = try w.toOwnedSlice();
    return .{
        .bytes = bytes,
        .item_count = item_count,
    };
}

/// Build the fully qualified interface name: "namespace:package/interface@version"
fn qualifiedInterfaceName(
    world: *const WitWorld,
    iface: *const WitInterface,
    allocator: std.mem.Allocator,
) ![]u8 {
    const pkg = world.package;
    if (std.mem.indexOfScalar(u8, pkg, '@')) |at_pos| {
        const before_at = pkg[0..at_pos];
        const after_at = pkg[at_pos..];
        const result = try std.fmt.allocPrint(allocator, "{s}/{s}{s}", .{ before_at, iface.name, after_at });
        return result;
    }
    const result = try std.fmt.allocPrint(allocator, "{s}/{s}", .{ pkg, iface.name });
    return result;
}

/// Build the fully qualified world name.
fn qualifiedWorldName(
    world: *const WitWorld,
    allocator: std.mem.Allocator,
) ![]u8 {
    const pkg = world.package;
    if (std.mem.indexOfScalar(u8, pkg, '@')) |at_pos| {
        const before_at = pkg[0..at_pos];
        const after_at = pkg[at_pos..];
        const result = try std.fmt.allocPrint(allocator, "{s}/{s}{s}", .{ before_at, world.name, after_at });
        return result;
    }
    const result = try std.fmt.allocPrint(allocator, "{s}/{s}", .{ pkg, world.name });
    return result;
}

test "uleb128 encoding" {
    const allocator = std.testing.allocator;
    var w = Writer.init(allocator);
    defer w.deinit();

    try w.emitUleb128(0);
    try std.testing.expectEqualSlices(u8, &.{0x00}, w.items());

    w.buf.clearRetainingCapacity();
    try w.emitUleb128(127);
    try std.testing.expectEqualSlices(u8, &.{0x7f}, w.items());

    w.buf.clearRetainingCapacity();
    try w.emitUleb128(128);
    try std.testing.expectEqualSlices(u8, &.{ 0x80, 0x01 }, w.items());

    w.buf.clearRetainingCapacity();
    try w.emitUleb128(624485);
    try std.testing.expectEqualSlices(u8, &.{ 0xe5, 0x8e, 0x26 }, w.items());
}
