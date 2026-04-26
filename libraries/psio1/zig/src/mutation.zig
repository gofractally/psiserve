//! In-place mutation for fracpack packed data.
//!
//! Provides `MutView` types that allow modifying individual fields of packed
//! fracpack data without full deserialization/reserialization.
//!
//! Two modes:
//! - **Canonical** (default): splice-and-patch produces valid canonical output
//!   after every mutation. O(buffer_tail) memmove + O(num_variable_fields) patches.
//! - **Fast**: overwrite-or-append produces non-canonical but readable data.
//!   O(new_value_size) per mutation. Call `compact()` to restore canonical form.
//!
//! Usage:
//!   var buf: []u8 = @constCast(packed_data);
//!   var mv = MutView(MyStruct).init(&buf, .canonical);
//!   mv.set("name", "Bob");
//!   // buf now contains the mutated data

const std = @import("std");
const mem = std.mem;
const Allocator = std.mem.Allocator;
const fp = @import("fracpack.zig");

// ── Buffer operations ──────────────────────────────────────────────────

/// Replace `data[pos..pos+old_len]` with `new_bytes`, shifting the tail.
/// Returns the byte delta (positive = grew, negative = shrank).
pub fn spliceBuffer(data: *std.ArrayList(u8), alloc: Allocator, pos: usize, old_len: usize, new_bytes: []const u8) !i32 {
    const new_len = new_bytes.len;
    const delta: i32 = @as(i32, @intCast(new_len)) - @as(i32, @intCast(old_len));

    if (delta > 0) {
        // Grow: insert extra bytes
        const extra: usize = @intCast(delta);
        // Extend the buffer
        try data.appendNTimes(alloc, 0, extra);
        // Shift tail right
        const tail_start = pos + old_len;
        const tail_end = data.items.len - extra;
        std.mem.copyBackwards(u8, data.items[tail_start + extra ..], data.items[tail_start..tail_end]);
        // Copy new data
        @memcpy(data.items[pos..][0..new_len], new_bytes);
    } else if (delta < 0) {
        // Shrink: remove bytes
        const shrink: usize = @intCast(-delta);
        const tail_start = pos + old_len;
        // Shift tail left
        std.mem.copyForwards(u8, data.items[pos + new_len ..], data.items[tail_start..]);
        // Copy new data
        @memcpy(data.items[pos..][0..new_len], new_bytes);
        // Shrink the buffer
        data.items.len -= shrink;
    } else {
        // Same size: just copy
        @memcpy(data.items[pos..][0..new_len], new_bytes);
    }

    return delta;
}

/// Adjust a self-relative u32 offset at `offset_pos` after a splice.
/// Only adjusts if the offset is a valid pointer (>= 4) and the target
/// it points to is at or past `splice_pos`.
pub fn patchOffset(data: []u8, offset_pos: usize, splice_pos: usize, delta: i32) void {
    if (data.len < offset_pos + 4) return;
    const offset_val = readU32(data, offset_pos);
    if (offset_val <= 1) return; // 0 = empty, 1 = None — never patch
    const target = offset_pos + offset_val;
    if (target >= splice_pos) {
        const new_val: u32 = @intCast(@as(i64, offset_val) + delta);
        writeU32(data, offset_pos, new_val);
    }
}

// ── Low-level helpers ──────────────────────────────────────────────────

fn readU16(data: []const u8, pos: usize) u16 {
    return mem.readInt(u16, data[pos..][0..2], .little);
}

fn readU32(data: []const u8, pos: usize) u32 {
    return mem.readInt(u32, data[pos..][0..4], .little);
}

fn writeU16(data: []u8, pos: usize, val: u16) void {
    mem.writeInt(u16, data[pos..][0..2], val, .little);
}

fn writeU32(data: []u8, pos: usize, val: u32) void {
    mem.writeInt(u32, data[pos..][0..4], val, .little);
}

/// Measure the packed size of a standalone value on the heap.
fn packedSizeAt(comptime T: type, data: []const u8, pos: usize) usize {
    if (comptime fp.isScalar(T)) return comptime fp.scalarSize(T);

    const info = @typeInfo(T);

    // String
    if (info == .pointer and info.pointer.size == .slice and info.pointer.child == u8) {
        const len = readU32(data, pos);
        return 4 + len;
    }

    // Vec
    if (info == .pointer and info.pointer.size == .slice) {
        const Child = info.pointer.child;
        const data_size = readU32(data, pos);
        if (!comptime fp.isVarField(Child)) {
            return 4 + data_size;
        }
        // Variable-size elements: walk to find extent
        const elem_fixed = comptime fp.fieldFixedSize(Child);
        const count = data_size / elem_fixed;
        var end = pos + 4 + data_size;
        var i: usize = 0;
        while (i < count) : (i += 1) {
            const elem_off_pos = pos + 4 + i * elem_fixed;
            const elem_off = readU32(data, elem_off_pos);
            if (elem_off > 1) {
                const elem_target = elem_off_pos + elem_off;
                const elem_size = packedSizeAt(Child, data, elem_target);
                const candidate = elem_target + elem_size;
                if (candidate > end) end = candidate;
            }
        }
        return end - pos;
    }

    // Struct / Tuple
    if (info == .@"struct") {
        const ext = comptime fp.isExtensible(T);
        const header_size: usize = if (ext) 2 else 0;
        var fixed_size: usize = comptime fp.totalFixedSize(T);
        if (ext) fixed_size = readU16(data, pos);
        const fixed_start = pos + header_size;
        var end = fixed_start + fixed_size;

        const fields = info.@"struct".fields;
        var field_off: usize = 0;
        inline for (fields) |f| {
            const fsize = comptime fp.fieldFixedSize(f.type);
            if (field_off < fixed_size and comptime fp.isVarField(f.type)) {
                const off_val = readU32(data, fixed_start + field_off);
                if (off_val > 1) {
                    const target = fixed_start + field_off + off_val;
                    const inner_size = packedSizeAt(f.type, data, target);
                    const candidate = target + inner_size;
                    if (candidate > end) end = candidate;
                }
            }
            field_off += fsize;
        }
        return end - pos;
    }

    // Optional (on heap, inner is stored directly)
    if (info == .optional) {
        return packedSizeAt(info.optional.child, data, pos);
    }

    // Variant
    if (info == .@"union") {
        const data_size = readU32(data, pos + 1);
        return 1 + 4 + data_size;
    }

    @compileError("unsupported type for packedSizeAt: " ++ @typeName(T));
}

// ── MutView ────────────────────────────────────────────────────────────

pub const Mode = enum { canonical, fast };

/// Mutable view over packed fracpack data. Allows reading and modifying
/// individual fields in-place.
pub fn MutView(comptime T: type) type {
    const fields = @typeInfo(T).@"struct".fields;
    const ext = comptime (fp.isExtensible(T) or fp.isTuple(T));
    const header_size: u32 = if (ext) 2 else 0;

    return struct {
        buf: *std.ArrayList(u8),
        alloc: Allocator,
        base: u32,
        mode: Mode,

        const Self = @This();

        pub fn init(buf: *std.ArrayList(u8), alloc: Allocator, mode: Mode) Self {
            return .{ .buf = buf, .alloc = alloc, .base = 0, .mode = mode };
        }

        pub fn initAt(buf: *std.ArrayList(u8), alloc: Allocator, base: u32, mode: Mode) Self {
            return .{ .buf = buf, .alloc = alloc, .base = base, .mode = mode };
        }

        /// Read a field value using the zero-copy view.
        pub fn get(self: Self, comptime field_name: []const u8) fp.Error!FieldViewType(field_name) {
            const field_off = comptime fieldOffset(field_name);
            const FieldType = comptime FieldNativeType(field_name);
            return fp.readViewField(FieldType, self.buf.items, self.base + header_size + field_off);
        }

        /// Set a field value in-place.
        pub fn set(self: *Self, comptime field_name: []const u8, value: FieldSetType(field_name)) !void {
            const field_off = comptime fieldOffset(field_name);
            const FieldType = comptime FieldNativeType(field_name);

            if (comptime fp.isScalar(FieldType)) {
                // Check for elision
                if (ext and self.isFieldElided(field_off)) {
                    try self.expandFixedRegion(field_off, comptime fp.fieldFixedSize(FieldType));
                }
                fp.writeScalar(FieldType, self.buf.items, self.base + header_size + field_off, value);
                return;
            }

            if (comptime @typeInfo(FieldType) == .array) {
                if (ext and self.isFieldElided(field_off)) {
                    try self.expandFixedRegion(field_off, comptime fp.fieldFixedSize(FieldType));
                }
                fp.marshalArray(FieldType, self.buf.items, self.base + header_size + field_off, value);
                return;
            }

            // Variable-size field
            switch (self.mode) {
                .canonical => try self.setVarCanonical(field_name, field_off, FieldType, value),
                .fast => try self.setVarFast(field_name, field_off, FieldType, value),
            }
        }

        /// Check if a field was elided due to trailing optional elision.
        fn isFieldElided(self: *Self, comptime field_off: u32) bool {
            if (!ext) return false;
            const declared_fixed = readU16(self.buf.items, self.base);
            return field_off >= declared_fixed;
        }

        /// Expand the fixed region to include a field that was elided.
        /// Inserts zero-initialized offset slots and patches existing sibling offsets.
        fn expandFixedRegion(self: *Self, comptime field_off: u32, comptime fsize: u32) !void {
            if (!ext) return;
            const declared_fixed: u32 = readU16(self.buf.items, self.base);
            const needed_fixed: u32 = field_off + fsize;
            if (needed_fixed <= declared_fixed) return;

            const insert_pos: usize = self.base + header_size + declared_fixed;
            const insert_size: usize = needed_fixed - declared_fixed;

            // Build inserted bytes using allocator
            const insert_bytes = try self.alloc.alloc(u8, insert_size);
            defer self.alloc.free(insert_bytes);
            @memset(insert_bytes, 0);

            // Set optional fields to 1 (None marker)
            inline for (fields) |f| {
                const foff = comptime fieldOffsetByName(f.name);
                if (comptime (fp.isVarField(f.type) and @typeInfo(f.type) == .optional)) {
                    if (foff >= declared_fixed and foff < needed_fixed) {
                        writeU32(insert_bytes, foff - declared_fixed, 1);
                    }
                }
            }

            const delta = try spliceBuffer(self.buf, self.alloc, insert_pos, 0, insert_bytes);

            // Update the header
            writeU16(self.buf.items, self.base, @intCast(needed_fixed));

            // Patch existing sibling offsets that point past the insert point
            inline for (fields) |f| {
                const foff = comptime fieldOffsetByName(f.name);
                if (comptime fp.isVarField(f.type)) {
                    if (foff < declared_fixed) {
                        patchOffset(self.buf.items, self.base + header_size + foff, insert_pos, delta);
                    }
                }
            }
        }

        fn setVarCanonical(self: *Self, comptime field_name: []const u8, comptime field_off: u32, comptime FieldType: type, value: FieldSetType(field_name)) !void {
            // Handle trailing optional elision
            if (self.isFieldElided(field_off)) {
                if (comptime @typeInfo(FieldType) == .optional) {
                    if (isNull(FieldType, value)) {
                        return; // Already elided = None, nothing to do
                    }
                }
                // Expand the fixed region to include this field
                try self.expandFixedRegion(field_off, comptime fp.fieldFixedSize(FieldType));
            }

            const offset_pos = self.base + header_size + field_off;

            if (comptime @typeInfo(FieldType) == .optional) {
                if (isNull(FieldType, value)) {
                    // Setting to None
                    const old_offset = readU32(self.buf.items, offset_pos);
                    if (old_offset <= 1) {
                        writeU32(self.buf.items, offset_pos, 1);
                        return;
                    }
                    // Remove existing heap data
                    const old_target: usize = offset_pos + old_offset;
                    const InnerType = @typeInfo(FieldType).optional.child;
                    const old_size = packedSizeAt(InnerType, self.buf.items, old_target);
                    const delta = try spliceBuffer(self.buf, self.alloc, old_target, old_size, &.{});
                    writeU32(self.buf.items, offset_pos, 1);
                    self.patchSiblings(field_off, old_target, delta);
                    return;
                }
            }

            // Pack new value
            var new_buf = fp.Buffer.init(self.alloc);
            defer new_buf.deinit();
            if (comptime @typeInfo(FieldType) == .optional) {
                try fp.marshalOptPayload(@typeInfo(FieldType).optional.child, unwrapOpt(FieldType, value), &new_buf);
            } else {
                try fp.marshalVarField(FieldType, value, &new_buf);
            }
            const new_bytes = new_buf.items();

            const old_offset = readU32(self.buf.items, offset_pos);

            if (new_bytes.len == 0) {
                // Setting to empty container
                if (old_offset <= 1) {
                    writeU32(self.buf.items, offset_pos, 0);
                    return;
                }
                // Remove existing heap data
                const old_target: usize = offset_pos + old_offset;
                const old_size = packedSizeAt(FieldType, self.buf.items, old_target);
                const delta = try spliceBuffer(self.buf, self.alloc, old_target, old_size, &.{});
                writeU32(self.buf.items, offset_pos, 0);
                self.patchSiblings(field_off, old_target, delta);
                return;
            }

            if (old_offset <= 1) {
                // No existing heap data — insert at end
                const splice_pos = self.buf.items.len;
                const delta = try spliceBuffer(self.buf, self.alloc, splice_pos, 0, new_bytes);
                _ = delta;
                writeU32(self.buf.items, offset_pos, @intCast(splice_pos - offset_pos));
                return;
            }

            // Replace existing heap data
            const old_target: usize = offset_pos + old_offset;
            const old_size = packedSizeAt(FieldType, self.buf.items, old_target);
            const delta = try spliceBuffer(self.buf, self.alloc, old_target, old_size, new_bytes);
            // Offset stays the same since we spliced at the target
            self.patchSiblings(field_off, old_target, delta);
        }

        fn setVarFast(self: *Self, comptime field_name: []const u8, comptime field_off: u32, comptime FieldType: type, value: FieldSetType(field_name)) !void {
            // Handle trailing optional elision
            if (self.isFieldElided(field_off)) {
                if (comptime @typeInfo(FieldType) == .optional) {
                    if (isNull(FieldType, value)) {
                        return; // Already elided = None
                    }
                }
                try self.expandFixedRegion(field_off, comptime fp.fieldFixedSize(FieldType));
            }

            const offset_pos = self.base + header_size + field_off;

            // Check if setting optional to null
            if (comptime @typeInfo(FieldType) == .optional) {
                if (isNull(FieldType, value)) {
                    writeU32(self.buf.items, offset_pos, 1);
                    return;
                }
            }

            // Pack new value
            var new_buf = fp.Buffer.init(self.alloc);
            defer new_buf.deinit();
            if (comptime @typeInfo(FieldType) == .optional) {
                try fp.marshalOptPayload(@typeInfo(FieldType).optional.child, unwrapOpt(FieldType, value), &new_buf);
            } else {
                try fp.marshalVarField(FieldType, value, &new_buf);
            }
            const new_bytes = new_buf.items();

            const old_offset = readU32(self.buf.items, offset_pos);

            if (new_bytes.len == 0) {
                writeU32(self.buf.items, offset_pos, 0);
                return;
            }

            if (old_offset <= 1) {
                // No existing data — append
                const append_pos = self.buf.items.len;
                try self.buf.appendSlice(self.alloc, new_bytes);
                writeU32(self.buf.items, offset_pos, @intCast(append_pos - offset_pos));
                return;
            }

            // Measure old data
            const old_target: usize = offset_pos + old_offset;
            const old_size = packedSizeAt(FieldType, self.buf.items, old_target);

            if (new_bytes.len <= old_size) {
                // Fits — overwrite in place
                @memcpy(self.buf.items[old_target..][0..new_bytes.len], new_bytes);
                return;
            }

            // Doesn't fit — append to end
            const append_pos = self.buf.items.len;
            try self.buf.appendSlice(self.alloc, new_bytes);
            writeU32(self.buf.items, offset_pos, @intCast(append_pos - offset_pos));
        }

        fn patchSiblings(self: *Self, comptime modified_field_off: u32, splice_pos: usize, delta: i32) void {
            if (delta == 0) return;
            const actual_fixed = if (ext) readU16(self.buf.items, self.base) else comptime fp.totalFixedSize(T);
            inline for (fields) |f| {
                const foff = comptime fieldOffsetByName(f.name);
                if (foff != modified_field_off and comptime fp.isVarField(f.type)) {
                    if (foff < actual_fixed) {
                        patchOffset(self.buf.items, self.base + header_size + foff, splice_pos, delta);
                    }
                }
            }
        }

        /// Repack from scratch to restore canonical form (useful after fast-mode mutations).
        pub fn compact(self: *Self) !void {
            const data_copy = try self.alloc.dupe(u8, self.buf.items);
            defer self.alloc.free(data_copy);

            const decoded = try fp.unmarshal(T, self.alloc, data_copy);
            defer fp.free(T, self.alloc, decoded);

            const repacked = try fp.marshal(T, self.alloc, decoded);
            defer self.alloc.free(repacked);

            self.buf.items.len = 0;
            try self.buf.appendSlice(self.alloc, repacked);
            self.base = 0;
        }

        // ── Comptime helpers ──────────────────────────────────────────────

        fn FieldNativeType(comptime name: []const u8) type {
            inline for (fields) |f| {
                if (std.mem.eql(u8, f.name, name)) return f.type;
            }
            @compileError("no field '" ++ name ++ "' in type");
        }

        fn FieldViewType(comptime name: []const u8) type {
            return fp.ViewFieldType(FieldNativeType(name));
        }

        fn FieldSetType(comptime name: []const u8) type {
            return FieldNativeType(name);
        }

        fn fieldOffset(comptime name: []const u8) u32 {
            return fieldOffsetByName(name);
        }

        fn fieldOffsetByName(comptime name: []const u8) u32 {
            var off: u32 = 0;
            inline for (fields) |f| {
                if (std.mem.eql(u8, f.name, name)) return off;
                off += comptime fp.fieldFixedSize(f.type);
            }
            @compileError("no field '" ++ name ++ "' in type");
        }

        fn isNull(comptime FT: type, value: FT) bool {
            if (@typeInfo(FT) != .optional) return false;
            return value == null;
        }

        fn unwrapOpt(comptime FT: type, value: FT) @typeInfo(FT).optional.child {
            return value.?;
        }
    };
}

// ── Tests ──────────────────────────────────────────────────────────────

const testing = std.testing;

test "spliceBuffer grow" {
    const alloc = testing.allocator;
    var buf = std.ArrayList(u8).empty;
    defer buf.deinit(alloc);
    try buf.appendSlice(alloc, "hello world");

    const delta = try spliceBuffer(&buf, alloc, 5, 1, " big ");
    try testing.expectEqual(@as(i32, 4), delta);
    try testing.expectEqualSlices(u8, "hello big world", buf.items);
}

test "spliceBuffer shrink" {
    const alloc = testing.allocator;
    var buf = std.ArrayList(u8).empty;
    defer buf.deinit(alloc);
    try buf.appendSlice(alloc, "hello big world");

    const delta = try spliceBuffer(&buf, alloc, 5, 5, " ");
    try testing.expectEqual(@as(i32, -4), delta);
    try testing.expectEqualSlices(u8, "hello world", buf.items);
}

test "spliceBuffer same size" {
    const alloc = testing.allocator;
    var buf = std.ArrayList(u8).empty;
    defer buf.deinit(alloc);
    try buf.appendSlice(alloc, "hello world");

    const delta = try spliceBuffer(&buf, alloc, 6, 5, "earth");
    try testing.expectEqual(@as(i32, 0), delta);
    try testing.expectEqualSlices(u8, "hello earth", buf.items);
}

test "patchOffset adjusts forward target" {
    const alloc = testing.allocator;
    var data = [_]u8{ 0x0A, 0x00, 0x00, 0x00 }; // offset = 10
    _ = alloc;
    patchOffset(&data, 0, 5, 3);
    const new_off = readU32(&data, 0);
    try testing.expectEqual(@as(u32, 13), new_off);
}

test "patchOffset skips offset 0" {
    var data = [_]u8{ 0x00, 0x00, 0x00, 0x00 };
    patchOffset(&data, 0, 0, 5);
    try testing.expectEqual(@as(u32, 0), readU32(&data, 0));
}

test "patchOffset skips offset 1" {
    var data = [_]u8{ 0x01, 0x00, 0x00, 0x00 };
    patchOffset(&data, 0, 0, 5);
    try testing.expectEqual(@as(u32, 1), readU32(&data, 0));
}

test "MutView canonical set scalar" {
    const alloc = testing.allocator;
    const Inner = struct {
        value: u32,
        label: []const u8,
    };

    const data = try fp.marshal(Inner, alloc, .{ .value = 42, .label = "hello" });
    defer alloc.free(data);

    var buf = std.ArrayList(u8).empty;
    defer buf.deinit(alloc);
    try buf.appendSlice(alloc, data);

    var mv = MutView(Inner).init(&buf, alloc, .canonical);

    // Read
    const val = try mv.get("value");
    try testing.expectEqual(@as(u32, 42), val);

    // Write scalar
    try mv.set("value", 99);
    const new_val = try mv.get("value");
    try testing.expectEqual(@as(u32, 99), new_val);

    // Verify the label is still intact
    const label = try mv.get("label");
    try testing.expectEqualSlices(u8, "hello", label);
}

test "MutView canonical set string" {
    const alloc = testing.allocator;
    const S = struct {
        name: []const u8,
        age: u32,
    };

    const data = try fp.marshal(S, alloc, .{ .name = "Alice", .age = 30 });
    defer alloc.free(data);

    var buf = std.ArrayList(u8).empty;
    defer buf.deinit(alloc);
    try buf.appendSlice(alloc, data);

    var mv = MutView(S).init(&buf, alloc, .canonical);

    // Read original
    const name = try mv.get("name");
    try testing.expectEqualSlices(u8, "Alice", name);

    // Set to shorter string
    try mv.set("name", "Bob");
    const new_name = try mv.get("name");
    try testing.expectEqualSlices(u8, "Bob", new_name);

    // Age should still be intact
    const age = try mv.get("age");
    try testing.expectEqual(@as(u32, 30), age);

    // Verify canonical form by repacking
    const repacked = try fp.marshal(S, alloc, .{ .name = "Bob", .age = 30 });
    defer alloc.free(repacked);
    try testing.expectEqualSlices(u8, repacked, buf.items);
}

test "MutView canonical set string grow" {
    const alloc = testing.allocator;
    const S = struct {
        name: []const u8,
        age: u32,
    };

    const data = try fp.marshal(S, alloc, .{ .name = "Bob", .age = 25 });
    defer alloc.free(data);

    var buf = std.ArrayList(u8).empty;
    defer buf.deinit(alloc);
    try buf.appendSlice(alloc, data);

    var mv = MutView(S).init(&buf, alloc, .canonical);

    // Set to longer string
    try mv.set("name", "Alexander");
    const new_name = try mv.get("name");
    try testing.expectEqualSlices(u8, "Alexander", new_name);

    // Verify canonical form
    const repacked = try fp.marshal(S, alloc, .{ .name = "Alexander", .age = 25 });
    defer alloc.free(repacked);
    try testing.expectEqualSlices(u8, repacked, buf.items);
}

test "MutView canonical optional None to Some" {
    const alloc = testing.allocator;
    const S = struct {
        required: u32,
        opt_str: ?[]const u8,
    };

    const data = try fp.marshal(S, alloc, .{ .required = 1, .opt_str = null });
    defer alloc.free(data);

    var buf = std.ArrayList(u8).empty;
    defer buf.deinit(alloc);
    try buf.appendSlice(alloc, data);

    var mv = MutView(S).init(&buf, alloc, .canonical);

    // Set optional from null to value
    try mv.set("opt_str", "hello");
    const result = try mv.get("opt_str");
    const s = result orelse return error.BufferTooShort;
    try testing.expectEqualSlices(u8, "hello", s);
}

test "MutView canonical optional Some to None" {
    const alloc = testing.allocator;
    const S = struct {
        required: u32,
        opt_str: ?[]const u8,
    };

    const data = try fp.marshal(S, alloc, .{ .required = 1, .opt_str = "hello" });
    defer alloc.free(data);

    var buf = std.ArrayList(u8).empty;
    defer buf.deinit(alloc);
    try buf.appendSlice(alloc, data);

    var mv = MutView(S).init(&buf, alloc, .canonical);

    // Set to None
    try mv.set("opt_str", null);
    const result = try mv.get("opt_str");
    try testing.expectEqual(@as(?[]const u8, null), result);
}

test "MutView fast mode overwrite" {
    const alloc = testing.allocator;
    const S = struct {
        name: []const u8,
        age: u32,
    };

    const data = try fp.marshal(S, alloc, .{ .name = "Alice", .age = 30 });
    defer alloc.free(data);

    var buf = std.ArrayList(u8).empty;
    defer buf.deinit(alloc);
    try buf.appendSlice(alloc, data);

    var mv = MutView(S).init(&buf, alloc, .fast);

    // Set to shorter string (fits in place)
    try mv.set("name", "Bob");
    const new_name = try mv.get("name");
    try testing.expectEqualSlices(u8, "Bob", new_name);
}

test "MutView fast mode append" {
    const alloc = testing.allocator;
    const S = struct {
        name: []const u8,
        age: u32,
    };

    const data = try fp.marshal(S, alloc, .{ .name = "Bob", .age = 25 });
    defer alloc.free(data);

    var buf = std.ArrayList(u8).empty;
    defer buf.deinit(alloc);
    try buf.appendSlice(alloc, data);

    var mv = MutView(S).init(&buf, alloc, .fast);

    // Set to longer string (append)
    try mv.set("name", "Alexander the Great");
    const new_name = try mv.get("name");
    try testing.expectEqualSlices(u8, "Alexander the Great", new_name);

    // Age should still be readable
    const age = try mv.get("age");
    try testing.expectEqual(@as(u32, 25), age);
}

test "MutView compact" {
    const alloc = testing.allocator;
    const S = struct {
        name: []const u8,
        age: u32,
    };

    const data = try fp.marshal(S, alloc, .{ .name = "Alice", .age = 30 });
    defer alloc.free(data);

    var buf = std.ArrayList(u8).empty;
    defer buf.deinit(alloc);
    try buf.appendSlice(alloc, data);

    var mv = MutView(S).init(&buf, alloc, .fast);

    // Fast-mode mutations leave dead bytes
    try mv.set("name", "Alexander the Great");
    try mv.set("name", "Bob"); // Now old data is dead

    // Compact
    try mv.compact();

    // Verify matches canonical form
    const repacked = try fp.marshal(S, alloc, .{ .name = "Bob", .age = 30 });
    defer alloc.free(repacked);
    try testing.expectEqualSlices(u8, repacked, buf.items);
}

test "MutView multiple field mutations canonical" {
    const alloc = testing.allocator;
    const S = struct {
        first: []const u8,
        second: []const u8,
        third: u32,
    };

    const data = try fp.marshal(S, alloc, .{ .first = "aaa", .second = "bbb", .third = 42 });
    defer alloc.free(data);

    var buf = std.ArrayList(u8).empty;
    defer buf.deinit(alloc);
    try buf.appendSlice(alloc, data);

    var mv = MutView(S).init(&buf, alloc, .canonical);

    // Modify first field (shrink)
    try mv.set("first", "x");
    const f1 = try mv.get("first");
    try testing.expectEqualSlices(u8, "x", f1);

    // Verify second is still valid after splice
    const s1 = try mv.get("second");
    try testing.expectEqualSlices(u8, "bbb", s1);

    // Modify second field (grow)
    try mv.set("second", "longer string");
    const s2 = try mv.get("second");
    try testing.expectEqualSlices(u8, "longer string", s2);

    // Verify round-trip
    const repacked = try fp.marshal(S, alloc, .{ .first = "x", .second = "longer string", .third = 42 });
    defer alloc.free(repacked);
    try testing.expectEqualSlices(u8, repacked, buf.items);
}
