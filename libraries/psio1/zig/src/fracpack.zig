//! Fracpack binary serialization — comptime generic API.
//!
//! Pack native Zig structs directly:
//!   const data = try fracpack.marshal(allocator, Person, .{ .name = "Alice", .age = 30 });
//!
//! Zero-copy views with direct field access:
//!   const v = try fracpack.view(Person, data);
//!   v.name   // []const u8 (zero-copy slice into data)
//!   v.age    // u32

const std = @import("std");
const mem = std.mem;
const Allocator = std.mem.Allocator;

// ── Errors ──────────────────────────────────────────────────────────────

pub const Error = error{
    BufferTooShort,
    InvalidOffset,
} || Allocator.Error;

// ── Type introspection ──────────────────────────────────────────────────

/// Detect Zig tuple types (anonymous structs with numeric field names).
pub fn isTuple(comptime T: type) bool {
    const info = @typeInfo(T);
    if (info != .@"struct") return false;
    return info.@"struct".is_tuple;
}

/// A struct with `pub const fracpack_fixed = true;` is non-extensible (no u16 header).
pub fn isExtensible(comptime T: type) bool {
    const info = @typeInfo(T);
    if (info != .@"struct") return false;
    return !@hasDecl(T, "fracpack_fixed");
}

pub fn isScalar(comptime T: type) bool {
    return switch (@typeInfo(T)) {
        .bool, .int, .float => true,
        else => false,
    };
}

pub fn scalarSize(comptime T: type) u32 {
    return switch (@typeInfo(T)) {
        .bool => 1,
        .int => |i| i.bits / 8,
        .float => |f| f.bits / 8,
        else => @compileError("not a scalar type"),
    };
}

/// Size in bytes of a field's fixed-region slot.
pub fn fieldFixedSize(comptime T: type) u32 {
    if (isScalar(T)) return scalarSize(T);
    const info = @typeInfo(T);
    if (info == .optional) return 4; // offset
    if (info == .pointer and info.pointer.size == .slice) return 4; // offset
    if (info == .@"struct") return 4; // offset to nested object
    if (info == .@"union") return 4; // offset to variant
    if (info == .array) {
        const arr = info.array;
        return @as(u32, arr.len) * fieldFixedSize(arr.child);
    }
    @compileError("unsupported fracpack field type: " ++ @typeName(T));
}

/// Whether a field is variable-size (stored as offset in fixed region).
pub fn isVarField(comptime T: type) bool {
    return !isScalar(T) and @typeInfo(T) != .array;
}

/// Total fixed-region size for all fields of a struct (excluding header).
pub fn totalFixedSize(comptime T: type) u32 {
    const fields = @typeInfo(T).@"struct".fields;
    var total: u32 = 0;
    inline for (fields) |f| {
        total += fieldFixedSize(f.type);
    }
    return total;
}

// ── View type generation ────────────────────────────────────────────────

/// Maps a Zig type to its view equivalent.
pub fn ViewFieldType(comptime T: type) type {
    if (isScalar(T)) return T;
    const info = @typeInfo(T);
    if (info == .pointer and info.pointer.size == .slice) {
        if (info.pointer.child == u8) return []const u8; // string/bytes
        return VecView(info.pointer.child);
    }
    if (info == .optional) return ?ViewFieldType(info.optional.child);
    if (info == .@"struct") return View(T);
    if (info == .@"union") return VariantView(T);
    if (info == .array) {
        const arr = info.array;
        return [arr.len]ViewFieldType(arr.child);
    }
    @compileError("unsupported view field type: " ++ @typeName(T));
}

/// Generate a view struct/tuple with the same field names as T but view types.
/// For tuple types (is_tuple = true), produces a view tuple.
/// For regular structs, produces a view struct.
pub fn View(comptime T: type) type {
    const src_fields = @typeInfo(T).@"struct".fields;
    const is_tuple_type = comptime isTuple(T);
    var view_fields: [src_fields.len]std.builtin.Type.StructField = undefined;
    inline for (src_fields, 0..) |f, i| {
        const VFT = ViewFieldType(f.type);
        view_fields[i] = .{
            .name = f.name,
            .type = VFT,
            .default_value_ptr = null,
            .is_comptime = false,
            .alignment = @alignOf(VFT),
        };
    }
    return @Type(.{ .@"struct" = .{
        .layout = .auto,
        .fields = &view_fields,
        .decls = &.{},
        .is_tuple = is_tuple_type,
    } });
}

/// Zero-copy view over a fracpack vec.
pub fn VecView(comptime Elem: type) type {
    return struct {
        data: []const u8,
        fixed_bytes: u32,
        elem_fixed: u32,

        const Self = @This();

        pub fn len(self: Self) u32 {
            if (self.elem_fixed == 0) return 0;
            return self.fixed_bytes / self.elem_fixed;
        }

        pub fn get(self: Self, index: u32) Error!ViewFieldType(Elem) {
            if (index >= self.len()) return error.InvalidOffset;
            return readViewField(Elem, self.data, 4 + index * self.elem_fixed);
        }

        pub fn iter(self: Self) Iterator {
            return .{ .vec = self, .index = 0 };
        }

        pub const Iterator = struct {
            vec: Self,
            index: u32,

            pub fn next(it: *Iterator) ?ViewFieldType(Elem) {
                if (it.index >= it.vec.len()) return null;
                const val = it.vec.get(it.index) catch return null;
                it.index += 1;
                return val;
            }
        };
    };
}

/// Zero-copy view over a fracpack variant (tagged union).
pub fn VariantView(comptime T: type) type {
    const union_info = @typeInfo(T).@"union";
    const tag_type = union_info.tag_type.?;
    const ufields = union_info.fields;

    var view_fields: [ufields.len]std.builtin.Type.UnionField = undefined;
    inline for (ufields, 0..) |uf, i| {
        view_fields[i] = .{
            .name = uf.name,
            .type = ViewFieldType(uf.type),
            .alignment = 0,
        };
    }

    return @Type(.{ .@"union" = .{
        .layout = .auto,
        .tag_type = tag_type,
        .fields = &view_fields,
        .decls = &.{},
    } });
}

// ── Low-level read/write ────────────────────────────────────────────────

fn readInt(comptime T: type, data: []const u8, pos: u32) T {
    const bytes = @divExact(@typeInfo(T).int.bits, 8);
    return mem.readInt(T, data[pos..][0..bytes], .little);
}

fn writeInt(comptime T: type, buf: []u8, pos: u32, val: T) void {
    const bytes = @divExact(@typeInfo(T).int.bits, 8);
    mem.writeInt(T, buf[pos..][0..bytes], val, .little);
}

fn readScalar(comptime T: type, data: []const u8, pos: u32) T {
    if (T == bool) return data[pos] != 0;
    const info = @typeInfo(T);
    if (info == .int) return readInt(T, data, pos);
    if (info == .float) {
        if (T == f32) return @bitCast(readInt(u32, data, pos));
        if (T == f64) return @bitCast(readInt(u64, data, pos));
    }
    @compileError("not a scalar");
}

pub fn writeScalar(comptime T: type, buf: []u8, pos: u32, val: T) void {
    if (T == bool) {
        buf[pos] = if (val) 1 else 0;
        return;
    }
    const info = @typeInfo(T);
    if (info == .int) {
        writeInt(T, buf, pos, val);
        return;
    }
    if (info == .float) {
        if (T == f32) {
            writeInt(u32, buf, pos, @bitCast(val));
            return;
        }
        if (T == f64) {
            writeInt(u64, buf, pos, @bitCast(val));
            return;
        }
    }
}

// ── Buffer helper ───────────────────────────────────────────────────────

pub const Buffer = struct {
    list: std.ArrayList(u8),
    alloc: Allocator,

    pub fn init(a: Allocator) Buffer {
        return .{ .list = .empty, .alloc = a };
    }

    pub fn items(self: *Buffer) []u8 {
        return self.list.items;
    }

    pub fn pos(self: *const Buffer) u32 {
        return @intCast(self.list.items.len);
    }

    pub fn appendNZeros(self: *Buffer, n: u32) !void {
        try self.list.appendNTimes(self.alloc, 0, n);
    }

    pub fn appendSlice(self: *Buffer, data: []const u8) !void {
        try self.list.appendSlice(self.alloc, data);
    }

    pub fn toOwnedSlice(self: *Buffer) ![]u8 {
        return try self.list.toOwnedSlice(self.alloc);
    }

    pub fn deinit(self: *Buffer) void {
        self.list.deinit(self.alloc);
    }
};

// ── Marshal (pack) ──────────────────────────────────────────────────────

/// Pack a value of type T into a fracpack byte buffer.
pub fn marshal(comptime T: type, alloc: Allocator, value: T) Error![]u8 {
    var buf = Buffer.init(alloc);
    errdefer buf.deinit();
    try marshalObject(T, value, &buf);
    return try buf.toOwnedSlice();
}

fn marshalObject(comptime T: type, value: T, buf: *Buffer) Error!void {
    const fields = @typeInfo(T).@"struct".fields;
    const ext = comptime isExtensible(T);
    const header_size: u32 = if (ext) 2 else 0;
    const full_fixed = comptime totalFixedSize(T);

    var actual_fixed = full_fixed;
    if (ext) actual_fixed = computeActualFixed(T, value);

    const obj_start = buf.pos();
    try buf.appendNZeros(header_size + actual_fixed);
    if (ext) writeInt(u16, buf.items(), obj_start, @intCast(actual_fixed));

    // Pass 1: write fixed scalars
    var fixed_off: u32 = 0;
    inline for (fields) |f| {
        const fsize = comptime fieldFixedSize(f.type);
        if (fixed_off < actual_fixed) {
            if (comptime isScalar(f.type)) {
                writeScalar(f.type, buf.items(), obj_start + header_size + fixed_off, @field(value, f.name));
            } else if (comptime @typeInfo(f.type) == .array) {
                marshalArray(f.type, buf.items(), obj_start + header_size + fixed_off, @field(value, f.name));
            }
        }
        fixed_off += fsize;
    }

    // Pass 2: write var fields (heap data + patch offsets)
    fixed_off = 0;
    inline for (fields) |f| {
        const fsize = comptime fieldFixedSize(f.type);
        if (fixed_off < actual_fixed and comptime isVarField(f.type)) {
            const slot_pos = obj_start + header_size + fixed_off;
            const heap_before = buf.pos();
            try marshalVarField(f.type, @field(value, f.name), buf);
            const heap_after = buf.pos();
            const heap_bytes = heap_after - heap_before;

            if (heap_bytes == 0) {
                // Determine zero vs absent
                if (comptime @typeInfo(f.type) == .optional) {
                    const fval = @field(value, f.name);
                    if (fval == null) {
                        writeInt(u32, buf.items(), slot_pos, 1); // absent
                    } else {
                        writeInt(u32, buf.items(), slot_pos, 0); // empty container
                    }
                } else {
                    writeInt(u32, buf.items(), slot_pos, 0); // empty
                }
            } else {
                writeInt(u32, buf.items(), slot_pos, heap_before - slot_pos);
            }
        }
        fixed_off += fsize;
    }
}

pub fn marshalArray(comptime T: type, buf: []u8, pos: u32, value: T) void {
    const arr = @typeInfo(T).array;
    const elem_size = comptime fieldFixedSize(arr.child);
    for (0..arr.len) |i| {
        if (comptime isScalar(arr.child)) {
            writeScalar(arr.child, buf, pos + @as(u32, @intCast(i)) * elem_size, value[i]);
        }
    }
}

fn computeActualFixed(comptime T: type, value: T) u32 {
    const fields = @typeInfo(T).@"struct".fields;
    var last_present: i32 = -1;
    var idx: i32 = 0;
    inline for (fields) |f| {
        if (comptime @typeInfo(f.type) == .optional) {
            if (@field(value, f.name) != null) last_present = idx;
        } else {
            last_present = idx;
        }
        idx += 1;
    }
    if (last_present < 0) return 0;

    var size: u32 = 0;
    var j: i32 = 0;
    inline for (fields) |f| {
        if (j <= last_present) {
            size += comptime fieldFixedSize(f.type);
        }
        j += 1;
    }
    return size;
}

pub fn marshalVarField(comptime T: type, value: T, buf: *Buffer) Error!void {
    const info = @typeInfo(T);

    // String ([]const u8)
    if (info == .pointer and info.pointer.size == .slice and info.pointer.child == u8) {
        if (value.len == 0) return;
        const start = buf.pos();
        try buf.appendNZeros(4);
        writeInt(u32, buf.items(), start, @intCast(value.len));
        try buf.appendSlice(value);
        return;
    }

    // Vec ([]const T)
    if (info == .pointer and info.pointer.size == .slice) {
        if (value.len == 0) return;
        try marshalVec(info.pointer.child, value, buf);
        return;
    }

    // Optional
    if (info == .optional) {
        if (value) |inner| {
            try marshalOptPayload(@typeInfo(T).optional.child, inner, buf);
        }
        return;
    }

    // Nested object
    if (info == .@"struct") {
        try marshalObject(T, value, buf);
        return;
    }

    // Variant
    if (info == .@"union") {
        try marshalVariant(T, value, buf);
        return;
    }
}

fn marshalVec(comptime Elem: type, elems: []const Elem, buf: *Buffer) Error!void {
    const n: u32 = @intCast(elems.len);
    const elem_fixed = comptime fieldFixedSize(Elem);
    const fixed_bytes = n * elem_fixed;

    if (comptime isScalar(Elem)) {
        // Primitive vec: [u32 fixed_bytes][elements...]
        const start = buf.pos();
        try buf.appendNZeros(4 + fixed_bytes);
        writeInt(u32, buf.items(), start, fixed_bytes);
        for (0..n) |i| {
            writeScalar(Elem, buf.items(), start + 4 + @as(u32, @intCast(i)) * elem_fixed, elems[i]);
        }
        return;
    }

    if (comptime @typeInfo(Elem) == .array) {
        // Fixed-size array elements
        const start = buf.pos();
        try buf.appendNZeros(4 + fixed_bytes);
        writeInt(u32, buf.items(), start, fixed_bytes);
        for (0..n) |i| {
            marshalArray(Elem, buf.items(), start + 4 + @as(u32, @intCast(i)) * elem_fixed, elems[i]);
        }
        return;
    }

    // Variable-size elements: each gets an offset slot
    // Pack each element to temp buffers
    var temp_bufs: [][]u8 = try buf.alloc.alloc([]u8, n);
    defer {
        for (temp_bufs) |t| if (t.len > 0) buf.alloc.free(t);
        buf.alloc.free(temp_bufs);
    }
    var heap_total: u32 = 0;
    for (0..n) |i| {
        var inner = Buffer.init(buf.alloc);
        errdefer inner.deinit();
        try marshalVecElem(Elem, elems[i], &inner);
        temp_bufs[i] = try inner.toOwnedSlice();
        heap_total += @intCast(temp_bufs[i].len);
    }

    const start = buf.pos();
    try buf.appendNZeros(4 + fixed_bytes + heap_total);
    writeInt(u32, buf.items(), start, fixed_bytes);

    const heap_start = start + 4 + fixed_bytes;
    var heap_off: u32 = 0;
    for (0..n) |i| {
        const off_pos = start + 4 + @as(u32, @intCast(i)) * 4;
        if (temp_bufs[i].len == 0) {
            // For optional elements, 0-len means absent → offset=1
            if (comptime @typeInfo(Elem) == .optional) {
                writeInt(u32, buf.items(), off_pos, 1);
            } else {
                writeInt(u32, buf.items(), off_pos, 0);
            }
        } else {
            writeInt(u32, buf.items(), off_pos, heap_start + heap_off - off_pos);
            const tlen: u32 = @intCast(temp_bufs[i].len);
            @memcpy(buf.items()[heap_start + heap_off ..][0..tlen], temp_bufs[i]);
            heap_off += tlen;
        }
    }
}

fn marshalVecElem(comptime Elem: type, value: Elem, buf: *Buffer) Error!void {
    const info = @typeInfo(Elem);

    // String element
    if (info == .pointer and info.pointer.size == .slice and info.pointer.child == u8) {
        if (value.len == 0) return;
        const start = buf.pos();
        try buf.appendNZeros(4);
        writeInt(u32, buf.items(), start, @intCast(value.len));
        try buf.appendSlice(value);
        return;
    }

    // Slice element (nested vec)
    if (info == .pointer and info.pointer.size == .slice) {
        if (value.len == 0) return;
        try marshalVec(info.pointer.child, value, buf);
        return;
    }

    // Optional element
    if (info == .optional) {
        if (value) |inner| {
            try marshalOptPayload(info.optional.child, inner, buf);
        }
        return;
    }

    // Struct element
    if (info == .@"struct") {
        try marshalObject(Elem, value, buf);
        return;
    }

    // Variant element
    if (info == .@"union") {
        try marshalVariant(Elem, value, buf);
        return;
    }
}

pub fn marshalOptPayload(comptime Inner: type, value: Inner, buf: *Buffer) Error!void {
    const info = @typeInfo(Inner);

    if (comptime isScalar(Inner)) {
        const start = buf.pos();
        const sz = comptime scalarSize(Inner);
        try buf.appendNZeros(sz);
        writeScalar(Inner, buf.items(), start, value);
        return;
    }

    // String
    if (info == .pointer and info.pointer.size == .slice and info.pointer.child == u8) {
        const start = buf.pos();
        try buf.appendNZeros(4);
        writeInt(u32, buf.items(), start, @intCast(value.len));
        try buf.appendSlice(value);
        return;
    }

    // Vec
    if (info == .pointer and info.pointer.size == .slice) {
        if (value.len == 0) return;
        try marshalVec(info.pointer.child, value, buf);
        return;
    }

    // Struct
    if (info == .@"struct") {
        try marshalObject(Inner, value, buf);
        return;
    }

    // Variant
    if (info == .@"union") {
        try marshalVariant(Inner, value, buf);
        return;
    }
}

fn marshalVariant(comptime T: type, value: T, buf: *Buffer) Error!void {
    const union_info = @typeInfo(T).@"union";
    const tag_type = union_info.tag_type.?;

    // Pack content to temp buffer
    var content = Buffer.init(buf.alloc);
    defer content.deinit();

    const tag_val: u8 = @intFromEnum(@as(tag_type, value));

    inline for (union_info.fields, 0..) |uf, i| {
        if (tag_val == i) {
            const inner = @field(value, uf.name);
            try marshalVariantContent(uf.type, inner, &content);
        }
    }

    // [u8 tag][u32 content_size][content]
    const start = buf.pos();
    try buf.appendNZeros(1 + 4 + content.pos());
    buf.items()[start] = tag_val;
    writeInt(u32, buf.items(), start + 1, content.pos());
    if (content.pos() > 0) {
        @memcpy(buf.items()[start + 5 ..][0..content.pos()], content.items());
    }
}

fn marshalVariantContent(comptime T: type, value: T, buf: *Buffer) Error!void {
    if (comptime isScalar(T)) {
        const start = buf.pos();
        try buf.appendNZeros(comptime scalarSize(T));
        writeScalar(T, buf.items(), start, value);
        return;
    }
    const info = @typeInfo(T);
    if (info == .pointer and info.pointer.size == .slice and info.pointer.child == u8) {
        // String: [u32 len][bytes]
        const start = buf.pos();
        try buf.appendNZeros(4);
        writeInt(u32, buf.items(), start, @intCast(value.len));
        try buf.appendSlice(value);
        return;
    }
    if (info == .@"struct") {
        try marshalObject(T, value, buf);
        return;
    }
}

// ── View (zero-copy read) ───────────────────────────────────────────────

/// Create a zero-copy view over packed data.
pub fn view(comptime T: type, data: []const u8) Error!View(T) {
    return parseView(T, data, 0);
}

fn parseView(comptime T: type, data: []const u8, base: u32) Error!View(T) {
    const fields = @typeInfo(T).@"struct".fields;
    const ext = comptime isExtensible(T);
    const header_size: u32 = if (ext) 2 else 0;

    if (data.len < base + header_size) return error.BufferTooShort;

    var actual_fixed: u32 = comptime totalFixedSize(T);
    if (ext) {
        actual_fixed = readInt(u16, data, base);
    }

    var result: View(T) = undefined;
    var fixed_off: u32 = 0;

    inline for (fields) |f| {
        const fsize = comptime fieldFixedSize(f.type);
        if (fixed_off < actual_fixed) {
            @field(result, f.name) = try readViewField(f.type, data, base + header_size + fixed_off);
        } else {
            @field(result, f.name) = viewDefault(f.type);
        }
        fixed_off += fsize;
    }

    return result;
}

pub fn readViewField(comptime T: type, data: []const u8, pos: u32) Error!ViewFieldType(T) {
    if (comptime isScalar(T)) {
        if (data.len < pos + comptime scalarSize(T)) return error.BufferTooShort;
        return readScalar(T, data, pos);
    }

    const info = @typeInfo(T);

    // Array
    if (info == .array) {
        const arr = info.array;
        var result: [arr.len]ViewFieldType(arr.child) = undefined;
        for (0..arr.len) |i| {
            result[i] = try readViewField(arr.child, data, pos + @as(u32, @intCast(i)) * comptime fieldFixedSize(arr.child));
        }
        return result;
    }

    // String ([]const u8)
    if (info == .pointer and info.pointer.size == .slice and info.pointer.child == u8) {
        if (data.len < pos + 4) return error.BufferTooShort;
        const off = readInt(u32, data, pos);
        if (off == 0) return "";
        const hp = pos + off;
        if (data.len < hp + 4) return error.BufferTooShort;
        const slen = readInt(u32, data, hp);
        if (data.len < hp + 4 + slen) return error.BufferTooShort;
        return data[hp + 4 ..][0..slen];
    }

    // Vec ([]const T)
    if (info == .pointer and info.pointer.size == .slice) {
        if (data.len < pos + 4) return error.BufferTooShort;
        const off = readInt(u32, data, pos);
        if (off == 0) return VecView(info.pointer.child){ .data = data, .fixed_bytes = 0, .elem_fixed = comptime fieldFixedSize(info.pointer.child) };
        const hp = pos + off;
        if (data.len < hp + 4) return error.BufferTooShort;
        const fixed_bytes = readInt(u32, data, hp);
        return VecView(info.pointer.child){ .data = data[hp..], .fixed_bytes = fixed_bytes, .elem_fixed = comptime fieldFixedSize(info.pointer.child) };
    }

    // Optional
    if (info == .optional) {
        const Child = info.optional.child;
        if (data.len < pos + 4) return error.BufferTooShort;
        const off = readInt(u32, data, pos);
        if (off == 1) return null; // absent
        if (off == 0) {
            // present but empty container
            return viewDefault(Child);
        }
        const hp = pos + off;
        return try readOptPayload(Child, data, hp);
    }

    // Nested struct
    if (info == .@"struct") {
        if (data.len < pos + 4) return error.BufferTooShort;
        const off = readInt(u32, data, pos);
        if (off == 0) return error.InvalidOffset;
        return try parseView(T, data, pos + off);
    }

    // Variant
    if (info == .@"union") {
        if (data.len < pos + 4) return error.BufferTooShort;
        const off = readInt(u32, data, pos);
        if (off == 0) return error.InvalidOffset;
        return try readViewVariant(T, data, pos + off);
    }

    @compileError("unsupported view field type: " ++ @typeName(T));
}

fn readOptPayload(comptime T: type, data: []const u8, pos: u32) Error!?ViewFieldType(T) {
    if (comptime isScalar(T)) {
        if (data.len < pos + comptime scalarSize(T)) return error.BufferTooShort;
        return readScalar(T, data, pos);
    }

    const info = @typeInfo(T);
    // String
    if (info == .pointer and info.pointer.size == .slice and info.pointer.child == u8) {
        if (data.len < pos + 4) return error.BufferTooShort;
        const slen = readInt(u32, data, pos);
        if (data.len < pos + 4 + slen) return error.BufferTooShort;
        return data[pos + 4 ..][0..slen];
    }

    // Vec ([]const T)
    if (info == .pointer and info.pointer.size == .slice) {
        if (data.len < pos + 4) return error.BufferTooShort;
        const fixed_bytes = readInt(u32, data, pos);
        return VecView(info.pointer.child){ .data = data[pos..], .fixed_bytes = fixed_bytes, .elem_fixed = comptime fieldFixedSize(info.pointer.child) };
    }

    // Struct
    if (info == .@"struct") {
        return try parseView(T, data, pos);
    }

    @compileError("unsupported optional payload type: " ++ @typeName(T));
}

fn readViewVariant(comptime T: type, data: []const u8, pos: u32) Error!VariantView(T) {
    const union_info = @typeInfo(T).@"union";

    if (data.len < pos + 5) return error.BufferTooShort;
    const tag = data[pos];
    const content_size = readInt(u32, data, pos + 1);
    const content_start = pos + 5;

    if (data.len < content_start + content_size) return error.BufferTooShort;

    inline for (union_info.fields, 0..) |uf, i| {
        if (tag == i) {
            const val = try readVariantContent(uf.type, data, content_start, content_size);
            return @unionInit(VariantView(T), uf.name, val);
        }
    }

    return error.InvalidOffset;
}

fn readVariantContent(comptime T: type, data: []const u8, pos: u32, _: u32) Error!ViewFieldType(T) {
    if (comptime isScalar(T)) {
        return readScalar(T, data, pos);
    }
    const info = @typeInfo(T);
    if (info == .pointer and info.pointer.size == .slice and info.pointer.child == u8) {
        // String in variant: [u32 len][bytes]
        if (data.len < pos + 4) return error.BufferTooShort;
        const slen = readInt(u32, data, pos);
        if (data.len < pos + 4 + slen) return error.BufferTooShort;
        return data[pos + 4 ..][0..slen];
    }
    if (info == .@"struct") {
        return try parseView(T, data, pos);
    }
    @compileError("unsupported variant content type: " ++ @typeName(T));
}

fn viewDefault(comptime T: type) ViewFieldType(T) {
    if (comptime isScalar(T)) {
        if (T == bool) return false;
        return 0;
    }
    const info = @typeInfo(T);
    if (info == .optional) return null;
    if (info == .pointer and info.pointer.size == .slice and info.pointer.child == u8) return "";
    if (info == .pointer and info.pointer.size == .slice) {
        return VecView(info.pointer.child){ .data = &.{}, .fixed_bytes = 0, .elem_fixed = comptime fieldFixedSize(info.pointer.child) };
    }
    if (info == .array) {
        const arr = info.array;
        var result: [arr.len]ViewFieldType(arr.child) = undefined;
        for (&result) |*r| r.* = viewDefault(arr.child);
        return result;
    }
    // Struct/union default: zero-initialized view
    return std.mem.zeroes(ViewFieldType(T));
}

// ── Unmarshal (allocating decode) ───────────────────────────────────────

/// Decode packed data into an owned value. Caller must call `free(T, alloc, result)`.
pub fn unmarshal(comptime T: type, alloc: Allocator, data: []const u8) Error!T {
    return unmarshalObject(T, alloc, data, 0);
}

fn unmarshalObject(comptime T: type, alloc: Allocator, data: []const u8, base: u32) Error!T {
    const fields = @typeInfo(T).@"struct".fields;
    const ext = comptime isExtensible(T);
    const header_size: u32 = if (ext) 2 else 0;

    if (data.len < base + header_size) return error.BufferTooShort;

    var actual_fixed: u32 = comptime totalFixedSize(T);
    if (ext) actual_fixed = readInt(u16, data, base);

    var result: T = undefined;
    var fixed_off: u32 = 0;

    inline for (fields) |f| {
        const fsize = comptime fieldFixedSize(f.type);
        if (fixed_off < actual_fixed) {
            @field(result, f.name) = try unmarshalField(f.type, alloc, data, base + header_size + fixed_off);
        } else {
            @field(result, f.name) = unmarshalDefault(f.type);
        }
        fixed_off += fsize;
    }

    return result;
}

fn unmarshalField(comptime T: type, alloc: Allocator, data: []const u8, pos: u32) Error!T {
    if (comptime isScalar(T)) return readScalar(T, data, pos);

    const info = @typeInfo(T);

    if (info == .array) {
        const arr = info.array;
        var result: [arr.len]arr.child = undefined;
        for (0..arr.len) |i| {
            result[i] = try unmarshalField(arr.child, alloc, data, pos + @as(u32, @intCast(i)) * comptime fieldFixedSize(arr.child));
        }
        return result;
    }

    // String
    if (info == .pointer and info.pointer.size == .slice and info.pointer.child == u8) {
        const off = readInt(u32, data, pos);
        if (off == 0) {
            const empty = try alloc.alloc(u8, 0);
            return empty;
        }
        const hp = pos + off;
        const slen = readInt(u32, data, hp);
        const copy = try alloc.alloc(u8, slen);
        @memcpy(copy, data[hp + 4 ..][0..slen]);
        return copy;
    }

    // Vec
    if (info == .pointer and info.pointer.size == .slice) {
        const Child = info.pointer.child;
        const off = readInt(u32, data, pos);
        if (off == 0) {
            const empty = try alloc.alloc(Child, 0);
            return empty;
        }
        const hp = pos + off;
        const fixed_bytes = readInt(u32, data, hp);
        const elem_fixed = comptime fieldFixedSize(Child);
        const n = fixed_bytes / elem_fixed;
        const result = try alloc.alloc(Child, n);
        for (0..n) |i| {
            result[i] = try unmarshalVecElem(Child, alloc, data[hp..], @as(u32, @intCast(4 + i * elem_fixed)));
        }
        return result;
    }

    // Optional
    if (info == .optional) {
        const Child = info.optional.child;
        const off = readInt(u32, data, pos);
        if (off == 1) return null; // absent
        if (off == 0) return unmarshalDefault(Child);
        const hp = pos + off;
        return try unmarshalOptPayload(Child, alloc, data, hp);
    }

    // Struct
    if (info == .@"struct") {
        const off = readInt(u32, data, pos);
        return try unmarshalObject(T, alloc, data, pos + off);
    }

    // Union
    if (info == .@"union") {
        const off = readInt(u32, data, pos);
        return try unmarshalVariant(T, alloc, data, pos + off);
    }

    @compileError("unsupported unmarshal type: " ++ @typeName(T));
}

fn unmarshalVecElem(comptime T: type, alloc: Allocator, vec_data: []const u8, pos: u32) Error!T {
    if (comptime isScalar(T)) return readScalar(T, vec_data, pos);

    const info = @typeInfo(T);
    // String
    if (info == .pointer and info.pointer.size == .slice and info.pointer.child == u8) {
        const off = readInt(u32, vec_data, pos);
        if (off == 0) return try alloc.alloc(u8, 0);
        const hp = pos + off;
        const slen = readInt(u32, vec_data, hp);
        const copy = try alloc.alloc(u8, slen);
        @memcpy(copy, vec_data[hp + 4 ..][0..slen]);
        return copy;
    }

    // Nested vec
    if (info == .pointer and info.pointer.size == .slice) {
        const Child = info.pointer.child;
        const off = readInt(u32, vec_data, pos);
        if (off == 0) return try alloc.alloc(Child, 0);
        const hp = pos + off;
        const fixed_bytes = readInt(u32, vec_data, hp);
        const elem_fixed = comptime fieldFixedSize(Child);
        const n = fixed_bytes / elem_fixed;
        const result = try alloc.alloc(Child, n);
        for (0..n) |i| {
            result[i] = try unmarshalVecElem(Child, alloc, vec_data[hp..], @as(u32, @intCast(4 + i * elem_fixed)));
        }
        return result;
    }

    // Optional element
    if (info == .optional) {
        const Child = info.optional.child;
        const off = readInt(u32, vec_data, pos);
        if (off == 1) return null;
        if (off == 0) return unmarshalDefault(Child);
        const hp = pos + off;
        return try unmarshalOptPayload(Child, alloc, vec_data, hp);
    }

    // Struct element
    if (info == .@"struct") {
        const off = readInt(u32, vec_data, pos);
        return try unmarshalObject(T, alloc, vec_data, pos + off);
    }

    @compileError("unsupported vec element type: " ++ @typeName(T));
}

fn unmarshalOptPayload(comptime T: type, alloc: Allocator, data: []const u8, pos: u32) Error!?T {
    if (comptime isScalar(T)) return readScalar(T, data, pos);

    const info = @typeInfo(T);
    if (info == .pointer and info.pointer.size == .slice and info.pointer.child == u8) {
        const slen = readInt(u32, data, pos);
        const copy = try alloc.alloc(u8, slen);
        @memcpy(copy, data[pos + 4 ..][0..slen]);
        return copy;
    }

    if (info == .pointer and info.pointer.size == .slice) {
        const Child = info.pointer.child;
        const fixed_bytes = readInt(u32, data, pos);
        const elem_fixed = comptime fieldFixedSize(Child);
        const n = fixed_bytes / elem_fixed;
        const result = try alloc.alloc(Child, n);
        for (0..n) |i| {
            result[i] = try unmarshalVecElem(Child, alloc, data[pos..], @as(u32, @intCast(4 + i * elem_fixed)));
        }
        return result;
    }

    if (info == .@"struct") {
        return try unmarshalObject(T, alloc, data, pos);
    }

    @compileError("unsupported optional payload type: " ++ @typeName(T));
}

fn unmarshalVariant(comptime T: type, alloc: Allocator, data: []const u8, pos: u32) Error!T {
    const union_info = @typeInfo(T).@"union";

    if (data.len < pos + 5) return error.BufferTooShort;
    const tag = data[pos];
    const content_size = readInt(u32, data, pos + 1);
    _ = content_size;
    const content_start = pos + 5;

    inline for (union_info.fields, 0..) |uf, i| {
        if (tag == i) {
            const val = try unmarshalVariantContent(uf.type, alloc, data, content_start);
            return @unionInit(T, uf.name, val);
        }
    }

    return error.InvalidOffset;
}

fn unmarshalVariantContent(comptime T: type, alloc: Allocator, data: []const u8, pos: u32) Error!T {
    if (comptime isScalar(T)) return readScalar(T, data, pos);
    const info = @typeInfo(T);
    if (info == .pointer and info.pointer.size == .slice and info.pointer.child == u8) {
        const slen = readInt(u32, data, pos);
        const copy = try alloc.alloc(u8, slen);
        @memcpy(copy, data[pos + 4 ..][0..slen]);
        return copy;
    }
    if (info == .@"struct") {
        return try unmarshalObject(T, alloc, data, pos);
    }
    @compileError("unsupported variant content type: " ++ @typeName(T));
}

fn unmarshalDefault(comptime T: type) T {
    if (comptime isScalar(T)) {
        if (T == bool) return false;
        return 0;
    }
    const info = @typeInfo(T);
    if (info == .optional) return null;
    if (info == .pointer and info.pointer.size == .slice) return &.{};
    if (info == .array) {
        const arr = info.array;
        var result: T = undefined;
        for (&result) |*r| r.* = unmarshalDefault(arr.child);
        return result;
    }
    return std.mem.zeroes(T);
}

/// Recursively free an unmarshaled value.
pub fn free(comptime T: type, alloc: Allocator, value: T) void {
    const info = @typeInfo(T);
    if (comptime isScalar(T)) return;

    if (info == .array) {
        const arr = info.array;
        if (!comptime isScalar(arr.child)) {
            for (value) |elem| free(arr.child, alloc, elem);
        }
        return;
    }

    if (info == .pointer and info.pointer.size == .slice) {
        if (info.pointer.child == u8) {
            alloc.free(value);
            return;
        }
        for (value) |elem| free(info.pointer.child, alloc, elem);
        alloc.free(value);
        return;
    }

    if (info == .optional) {
        if (value) |inner| free(info.optional.child, alloc, inner);
        return;
    }

    if (info == .@"struct") {
        const fields = info.@"struct".fields;
        inline for (fields) |f| {
            free(f.type, alloc, @field(value, f.name));
        }
        return;
    }

    if (info == .@"union") {
        const union_fields = info.@"union".fields;
        inline for (union_fields, 0..) |uf, i| {
            if (@intFromEnum(value) == i) {
                free(uf.type, alloc, @field(value, uf.name));
            }
        }
        return;
    }
}

// ── Validation (zero-copy) ─────────────────────────────────────────────

pub const ValidationStatus = enum { valid, extended, invalid };

pub const ValidationResult = struct {
    status: ValidationStatus,
    message: ?[]const u8,
};

/// Validate packed fracpack data against a type schema without unpacking.
///
/// Returns:
/// - `{ .status = .valid }` — data matches schema exactly
/// - `{ .status = .extended, .message = ... }` — valid but has unknown fields
/// - `{ .status = .invalid, .message = ... }` — data is malformed
pub fn validate(comptime T: type, data: []const u8) ValidationResult {
    var v = Validator{ .data = data, .end = @intCast(data.len), .extended = false };
    const pos = v.validateTop(T) catch |e| {
        return .{ .status = .invalid, .message = @errorName(e) };
    };
    if (pos != v.end) {
        return .{ .status = .invalid, .message = "extra data after valid content" };
    }
    if (v.extended) {
        return .{ .status = .extended, .message = "unknown fields present" };
    }
    return .{ .status = .valid, .message = null };
}

const Validator = struct {
    data: []const u8,
    end: u32,
    extended: bool,

    const ValError = error{
        BufferTooShort,
        InvalidBool,
        InvalidOffset,
        InvalidUtf8,
        InvalidVariantTag,
        SizeMismatch,
    };

    fn check(self: *const Validator, pos: u32, size: u32) ValError!void {
        if (pos > self.end or self.end - pos < size)
            return error.BufferTooShort;
    }

    fn readU8(self: *const Validator, pos: u32) ValError!u8 {
        try self.check(pos, 1);
        return self.data[pos];
    }

    fn readU16(self: *const Validator, pos: u32) ValError!u16 {
        try self.check(pos, 2);
        return mem.readInt(u16, self.data[pos..][0..2], .little);
    }

    fn readU32(self: *const Validator, pos: u32) ValError!u32 {
        try self.check(pos, 4);
        return mem.readInt(u32, self.data[pos..][0..4], .little);
    }

    /// Top-level validation: validate the root object and return the position after it.
    fn validateTop(self: *Validator, comptime T: type) ValError!u32 {
        const info = @typeInfo(T);
        if (info == .@"struct") {
            return self.validateObject(T, 0);
        }
        @compileError("validate requires a struct type at the top level");
    }

    /// Validate an object (struct or tuple) starting at `base`. Returns position after all data.
    fn validateObject(self: *Validator, comptime T: type, base: u32) ValError!u32 {
        const fields = @typeInfo(T).@"struct".fields;
        const ext = comptime isExtensible(T);
        const header_size: u32 = if (ext) 2 else 0;

        try self.check(base, header_size);

        var actual_fixed: u32 = comptime totalFixedSize(T);
        if (ext) {
            actual_fixed = try self.readU16(base);
        }

        const fixed_start = base + header_size;
        try self.check(fixed_start, actual_fixed);
        const heap_start = fixed_start + actual_fixed;

        // Detect extension: more fixed bytes than we know about
        if (ext) {
            const expected_fixed = comptime totalFixedSize(T);
            if (actual_fixed > expected_fixed) {
                self.extended = true;
            }
        }

        // Validate fields
        var end_pos = heap_start;
        var fixed_off: u32 = 0;
        inline for (fields) |f| {
            const fsize = comptime fieldFixedSize(f.type);
            if (fixed_off + fsize <= actual_fixed) {
                end_pos = try self.validateEmbedded(f.type, fixed_start + fixed_off, end_pos);
            }
            fixed_off += fsize;
        }

        return end_pos;
    }

    /// Validate an embedded field. Fixed-size fields are validated inline at `fixed_pos`.
    /// Variable-size fields read an offset at `fixed_pos` and validate heap data at `heap_pos`.
    /// Returns the new heap end position.
    fn validateEmbedded(self: *Validator, comptime T: type, fixed_pos: u32, heap_pos: u32) ValError!u32 {
        if (comptime isScalar(T)) {
            try self.validateScalar(T, fixed_pos);
            return heap_pos;
        }

        const info = @typeInfo(T);

        // Fixed-size array: validate each element inline
        if (info == .array) {
            const arr = info.array;
            const elem_size = comptime fieldFixedSize(arr.child);
            if (comptime !isScalar(arr.child) and isVarField(arr.child)) {
                // Variable-size array elements use offsets
                var ep = heap_pos;
                for (0..arr.len) |i| {
                    ep = try self.validateEmbedded(arr.child, fixed_pos + @as(u32, @intCast(i)) * elem_size, ep);
                }
                return ep;
            } else {
                for (0..arr.len) |i| {
                    _ = try self.validateEmbedded(arr.child, fixed_pos + @as(u32, @intCast(i)) * elem_size, heap_pos);
                }
                return heap_pos;
            }
        }

        // All remaining types are variable-size: read offset
        const offset = try self.readU32(fixed_pos);

        // Optional
        if (info == .optional) {
            const Child = info.optional.child;
            if (offset == 1) return heap_pos; // None
            if (offset == 0) return heap_pos; // empty container (Some("") etc.)
            if (offset < 4) return error.InvalidOffset; // reserved values 2, 3
            const target = fixed_pos + offset;
            if (target < heap_pos) return error.InvalidOffset; // backward offset
            return self.validateOptPayload(Child, target);
        }

        // String ([]const u8)
        if (info == .pointer and info.pointer.size == .slice and info.pointer.child == u8) {
            if (offset == 0) return heap_pos; // empty string
            if (offset < 4) return error.InvalidOffset;
            const target = fixed_pos + offset;
            if (target < heap_pos) return error.InvalidOffset;
            return self.validateString(target);
        }

        // Vec ([]const T)
        if (info == .pointer and info.pointer.size == .slice) {
            if (offset == 0) return heap_pos; // empty vec
            if (offset < 4) return error.InvalidOffset;
            const target = fixed_pos + offset;
            if (target < heap_pos) return error.InvalidOffset;
            return self.validateVec(info.pointer.child, target);
        }

        // Nested struct
        if (info == .@"struct") {
            if (offset == 0) return error.InvalidOffset;
            if (offset < 4) return error.InvalidOffset;
            const target = fixed_pos + offset;
            if (target < heap_pos) return error.InvalidOffset;
            return self.validateObject(T, target);
        }

        // Variant (tagged union)
        if (info == .@"union") {
            if (offset == 0) return error.InvalidOffset;
            if (offset < 4) return error.InvalidOffset;
            const target = fixed_pos + offset;
            if (target < heap_pos) return error.InvalidOffset;
            return self.validateVariant(T, target);
        }

        @compileError("unsupported validate field type: " ++ @typeName(T));
    }

    fn validateScalar(self: *Validator, comptime T: type, pos: u32) ValError!void {
        const size = comptime scalarSize(T);
        try self.check(pos, size);
        if (T == bool) {
            const v = self.data[pos];
            if (v > 1) return error.InvalidBool;
        }
    }

    fn validateString(self: *Validator, pos: u32) ValError!u32 {
        const byte_count = try self.readU32(pos);
        try self.check(pos + 4, byte_count);
        // Validate UTF-8
        const str_bytes = self.data[pos + 4 ..][0..byte_count];
        if (!std.unicode.utf8ValidateSlice(str_bytes)) {
            return error.InvalidUtf8;
        }
        return pos + 4 + byte_count;
    }

    fn validateVec(self: *Validator, comptime Elem: type, pos: u32) ValError!u32 {
        const data_size = try self.readU32(pos);
        const elem_fixed = comptime fieldFixedSize(Elem);
        if (elem_fixed > 0 and data_size % elem_fixed != 0) {
            return error.SizeMismatch;
        }
        try self.check(pos + 4, data_size);
        const n = if (elem_fixed > 0) data_size / elem_fixed else 0;
        const fixed_start = pos + 4;

        if (comptime !isVarField(Elem)) {
            // Fixed-size elements: validate each inline
            for (0..n) |i| {
                _ = try self.validateEmbedded(Elem, fixed_start + @as(u32, @intCast(i)) * elem_fixed, fixed_start + data_size);
            }
            return fixed_start + data_size;
        }

        // Variable-size elements: follow offsets
        var end_pos = fixed_start + data_size;
        for (0..n) |i| {
            const fp = fixed_start + @as(u32, @intCast(i)) * elem_fixed;
            end_pos = try self.validateEmbedded(Elem, fp, end_pos);
        }
        return end_pos;
    }

    fn validateOptPayload(self: *Validator, comptime T: type, pos: u32) ValError!u32 {
        if (comptime isScalar(T)) {
            try self.validateScalar(T, pos);
            return pos + comptime scalarSize(T);
        }

        const info = @typeInfo(T);

        // String
        if (info == .pointer and info.pointer.size == .slice and info.pointer.child == u8) {
            return self.validateString(pos);
        }

        // Vec
        if (info == .pointer and info.pointer.size == .slice) {
            return self.validateVec(info.pointer.child, pos);
        }

        // Struct
        if (info == .@"struct") {
            return self.validateObject(T, pos);
        }

        // Variant
        if (info == .@"union") {
            return self.validateVariant(T, pos);
        }

        @compileError("unsupported optional payload type for validation: " ++ @typeName(T));
    }

    fn validateVariant(self: *Validator, comptime T: type, pos: u32) ValError!u32 {
        const union_info = @typeInfo(T).@"union";
        const num_cases = union_info.fields.len;

        try self.check(pos, 5);
        const tag = self.data[pos];
        const data_size = try self.readU32(pos + 1);
        const content_start = pos + 5;

        if (tag >= num_cases) return error.InvalidVariantTag;

        try self.check(content_start, data_size);

        // Validate the content of the active case
        inline for (union_info.fields, 0..) |uf, i| {
            if (tag == i) {
                _ = try self.validateVariantContent(uf.type, content_start);
            }
        }

        return content_start + data_size;
    }

    fn validateVariantContent(self: *Validator, comptime T: type, pos: u32) ValError!u32 {
        if (comptime isScalar(T)) {
            try self.validateScalar(T, pos);
            return pos + comptime scalarSize(T);
        }

        const info = @typeInfo(T);

        // String
        if (info == .pointer and info.pointer.size == .slice and info.pointer.child == u8) {
            return self.validateString(pos);
        }

        // Struct
        if (info == .@"struct") {
            return self.validateObject(T, pos);
        }

        @compileError("unsupported variant content type for validation: " ++ @typeName(T));
    }
};

// ── Canonical JSON (de)serialization ────────────────────────────────────
//
// Zig's std.json serializes all integers as numbers, which silently overflows
// JavaScript's 53-bit safe integer range for u64/i64. It has no concept of
// hex-encoded bytes or canonical variant encoding. Custom (de)serializers
// require manual fn implementations per type. This module provides comptime-
// generated canonical JSON matching all fracpack implementations.

pub const JsonError = error{
    BufferTooShort,
    InvalidOffset,
    InvalidJson,
    InvalidHex,
    UnknownVariantCase,
    Overflow,
    InvalidCharacter,
} || Allocator.Error;

/// A marker type for binary data that should be hex-encoded in JSON.
/// Use `Bytes` instead of `[]const u8` for fields that represent raw bytes
/// rather than UTF-8 strings.
pub const Bytes = struct {
    data: []const u8,

    pub fn init(d: []const u8) Bytes {
        return .{ .data = d };
    }
};

/// Convert a fracpack-serializable struct to canonical JSON string.
/// Caller owns returned slice.
pub fn toJson(comptime T: type, value: T, allocator: Allocator) JsonError![]u8 {
    var list: std.ArrayList(u8) = .empty;
    errdefer list.deinit(allocator);
    try writeJsonValue(T, value, &list, allocator);
    return try list.toOwnedSlice(allocator);
}

const JsonWriteError = Allocator.Error;

fn writeJsonValue(comptime T: type, value: T, list: *std.ArrayList(u8), allocator: Allocator) JsonWriteError!void {
    const info = @typeInfo(T);

    // Bool
    if (T == bool) {
        if (value) {
            try list.appendSlice(allocator, "true");
        } else {
            try list.appendSlice(allocator, "false");
        }
        return;
    }

    // Bytes marker type → hex string
    if (T == Bytes) {
        try list.append(allocator, '"');
        for (value.data) |b| {
            try list.append(allocator, HEX_CHARS[b >> 4]);
            try list.append(allocator, HEX_CHARS[b & 0x0f]);
        }
        try list.append(allocator, '"');
        return;
    }

    // Integer types
    if (info == .int) {
        const bits = info.int.bits;
        if (bits == 64) {
            // u64/i64 → JSON string
            try list.append(allocator, '"');
            try writeDecimalInt(T, value, list, allocator);
            try list.append(allocator, '"');
        } else {
            // u8-u32, i8-i32 → JSON number
            try writeDecimalInt(T, value, list, allocator);
        }
        return;
    }

    // Float types
    if (info == .float) {
        if (std.math.isNan(value) or std.math.isInf(value)) {
            try list.appendSlice(allocator, "null");
        } else {
            var buf: [32]u8 = undefined;
            const slice = formatFloat(T, value, &buf);
            try list.appendSlice(allocator, slice);
        }
        return;
    }

    // Optional
    if (info == .optional) {
        if (value) |inner| {
            try writeJsonValue(info.optional.child, inner, list, allocator);
        } else {
            try list.appendSlice(allocator, "null");
        }
        return;
    }

    // String ([]const u8)
    if (info == .pointer and info.pointer.size == .slice and info.pointer.child == u8) {
        try writeJsonString(value, list, allocator);
        return;
    }

    // Vec/Slice ([]const T)
    if (info == .pointer and info.pointer.size == .slice) {
        try list.append(allocator, '[');
        for (value, 0..) |elem, i| {
            if (i > 0) try list.append(allocator, ',');
            try writeJsonValue(info.pointer.child, elem, list, allocator);
        }
        try list.append(allocator, ']');
        return;
    }

    // Fixed-size array [N]T
    if (info == .array) {
        try list.append(allocator, '[');
        for (value, 0..) |elem, i| {
            if (i > 0) try list.append(allocator, ',');
            try writeJsonValue(info.array.child, elem, list, allocator);
        }
        try list.append(allocator, ']');
        return;
    }

    // Union (variant) → {"CaseName": value}
    if (info == .@"union") {
        const union_info = info.@"union";
        const tag_type = union_info.tag_type.?;
        const tag_val: u8 = @intFromEnum(@as(tag_type, value));
        try list.append(allocator, '{');
        inline for (union_info.fields, 0..) |uf, idx| {
            if (tag_val == idx) {
                try writeJsonString(uf.name, list, allocator);
                try list.append(allocator, ':');
                try writeJsonValue(uf.type, @field(value, uf.name), list, allocator);
            }
        }
        try list.append(allocator, '}');
        return;
    }

    // Struct or tuple
    if (info == .@"struct") {
        const fields = info.@"struct".fields;
        if (info.@"struct".is_tuple) {
            // Tuple → JSON array
            try list.append(allocator, '[');
            inline for (fields, 0..) |f, i| {
                if (i > 0) try list.append(allocator, ',');
                try writeJsonValue(f.type, @field(value, f.name), list, allocator);
            }
            try list.append(allocator, ']');
        } else {
            // Struct → JSON object
            try list.append(allocator, '{');
            inline for (fields, 0..) |f, i| {
                if (i > 0) try list.append(allocator, ',');
                try writeJsonString(f.name, list, allocator);
                try list.append(allocator, ':');
                try writeJsonValue(f.type, @field(value, f.name), list, allocator);
            }
            try list.append(allocator, '}');
        }
        return;
    }

    @compileError("unsupported JSON type: " ++ @typeName(T));
}

const HEX_CHARS = "0123456789abcdef";

fn writeDecimalInt(comptime T: type, value: T, list: *std.ArrayList(u8), allocator: Allocator) JsonWriteError!void {
    var buf: [21]u8 = undefined; // max i64 is 20 digits + sign
    const info = @typeInfo(T);
    if (info.int.signedness == .signed) {
        const slice = std.fmt.bufPrint(&buf, "{d}", .{value}) catch unreachable;
        try list.appendSlice(allocator, slice);
    } else {
        const slice = std.fmt.bufPrint(&buf, "{d}", .{value}) catch unreachable;
        try list.appendSlice(allocator, slice);
    }
}

fn formatFloat(comptime T: type, value: T, buf: *[32]u8) []const u8 {
    // Use Zig's standard float formatting which produces the shortest
    // accurate representation (matches JavaScript's behavior).
    if (T == f32) {
        return std.fmt.bufPrint(buf, "{d}", .{@as(f64, value)}) catch "0";
    } else {
        return std.fmt.bufPrint(buf, "{d}", .{value}) catch "0";
    }
}

fn writeJsonString(s: []const u8, list: *std.ArrayList(u8), allocator: Allocator) JsonWriteError!void {
    try list.append(allocator, '"');
    for (s) |c| {
        switch (c) {
            '"' => try list.appendSlice(allocator, "\\\""),
            '\\' => try list.appendSlice(allocator, "\\\\"),
            '\n' => try list.appendSlice(allocator, "\\n"),
            '\t' => try list.appendSlice(allocator, "\\t"),
            '\r' => try list.appendSlice(allocator, "\\r"),
            0x08 => try list.appendSlice(allocator, "\\b"),
            0x0C => try list.appendSlice(allocator, "\\f"),
            else => {
                if (c < 0x20) {
                    // Other control characters: \u00XX
                    try list.appendSlice(allocator, "\\u00");
                    try list.append(allocator, HEX_CHARS[c >> 4]);
                    try list.append(allocator, HEX_CHARS[c & 0x0f]);
                } else {
                    try list.append(allocator, c);
                }
            },
        }
    }
    try list.append(allocator, '"');
}

/// Parse canonical JSON string to a fracpack-serializable struct.
pub fn fromJson(comptime T: type, json: []const u8, allocator: Allocator) JsonError!T {
    // Parse the JSON string into a dynamic value tree
    const parsed = std.json.parseFromSlice(std.json.Value, allocator, json, .{}) catch {
        return error.InvalidJson;
    };
    defer parsed.deinit();
    return jsonValueToType(T, parsed.value, allocator);
}

fn jsonValueToType(comptime T: type, jval: std.json.Value, allocator: Allocator) JsonError!T {
    const info = @typeInfo(T);

    // Bool
    if (T == bool) {
        return switch (jval) {
            .bool => |b| b,
            else => error.InvalidJson,
        };
    }

    // Bytes marker type
    if (T == Bytes) {
        const hex_str = switch (jval) {
            .string => |s| s,
            else => return error.InvalidJson,
        };
        const data = runtimeHexToBytes(hex_str, allocator) catch return error.InvalidHex;
        return .{ .data = data };
    }

    // Integer types
    if (info == .int) {
        const bits = info.int.bits;
        if (bits == 64) {
            // u64/i64 are JSON strings
            const str = switch (jval) {
                .string => |s| s,
                .integer => |i| {
                    // Also accept numeric values for flexibility
                    return @as(T, @intCast(i));
                },
                else => return error.InvalidJson,
            };
            return std.fmt.parseInt(T, str, 10) catch return error.Overflow;
        } else {
            return switch (jval) {
                .integer => |i| @as(T, @intCast(i)),
                .string => |s| std.fmt.parseInt(T, s, 10) catch return error.Overflow,
                else => error.InvalidJson,
            };
        }
    }

    // Float types
    if (info == .float) {
        return switch (jval) {
            .float => |f| @floatCast(f),
            .integer => |i| @floatFromInt(i),
            .null => if (T == f32) std.math.nan(f32) else std.math.nan(f64),
            else => error.InvalidJson,
        };
    }

    // Optional
    if (info == .optional) {
        return switch (jval) {
            .null => null,
            else => try jsonValueToType(info.optional.child, jval, allocator),
        };
    }

    // String ([]const u8)
    if (info == .pointer and info.pointer.size == .slice and info.pointer.child == u8) {
        const str = switch (jval) {
            .string => |s| s,
            else => return error.InvalidJson,
        };
        const copy = try allocator.alloc(u8, str.len);
        @memcpy(copy, str);
        return copy;
    }

    // Vec/Slice ([]const T)
    if (info == .pointer and info.pointer.size == .slice) {
        const arr = switch (jval) {
            .array => |a| a,
            else => return error.InvalidJson,
        };
        const Child = info.pointer.child;
        const result = try allocator.alloc(Child, arr.items.len);
        errdefer allocator.free(result);
        for (arr.items, 0..) |item, i| {
            result[i] = try jsonValueToType(Child, item, allocator);
        }
        return result;
    }

    // Fixed-size array [N]T
    if (info == .array) {
        const arr = switch (jval) {
            .array => |a| a,
            else => return error.InvalidJson,
        };
        if (arr.items.len != info.array.len) return error.InvalidJson;
        var result: T = undefined;
        for (0..info.array.len) |i| {
            result[i] = try jsonValueToType(info.array.child, arr.items[i], allocator);
        }
        return result;
    }

    // Union (variant) — {"CaseName": value}
    if (info == .@"union") {
        const obj = switch (jval) {
            .object => |o| o,
            else => return error.InvalidJson,
        };
        if (obj.count() != 1) return error.InvalidJson;

        var it = obj.iterator();
        const entry = it.next() orelse return error.InvalidJson;
        const key = entry.key_ptr.*;
        const val = entry.value_ptr.*;

        inline for (info.@"union".fields) |uf| {
            if (mem.eql(u8, key, uf.name)) {
                const inner = try jsonValueToType(uf.type, val, allocator);
                return @unionInit(T, uf.name, inner);
            }
        }
        return error.UnknownVariantCase;
    }

    // Struct or tuple
    if (info == .@"struct") {
        const fields = info.@"struct".fields;
        if (info.@"struct".is_tuple) {
            // Tuple ← JSON array
            const arr = switch (jval) {
                .array => |a| a,
                else => return error.InvalidJson,
            };
            if (arr.items.len != fields.len) return error.InvalidJson;
            var result: T = undefined;
            inline for (fields, 0..) |f, i| {
                @field(result, f.name) = try jsonValueToType(f.type, arr.items[i], allocator);
            }
            return result;
        } else {
            // Struct ← JSON object
            const obj = switch (jval) {
                .object => |o| o,
                else => return error.InvalidJson,
            };
            var result: T = undefined;
            inline for (fields) |f| {
                if (obj.get(f.name)) |field_val| {
                    @field(result, f.name) = try jsonValueToType(f.type, field_val, allocator);
                } else {
                    // Missing field: use default (null for optional, zero for scalars)
                    @field(result, f.name) = jsonDefault(f.type);
                }
            }
            return result;
        }
    }

    @compileError("unsupported JSON type: " ++ @typeName(T));
}

fn jsonDefault(comptime T: type) T {
    if (T == bool) return false;
    if (T == Bytes) return .{ .data = &.{} };
    const info = @typeInfo(T);
    if (info == .int) return 0;
    if (info == .float) return 0;
    if (info == .optional) return null;
    if (info == .pointer and info.pointer.size == .slice) return &.{};
    if (info == .@"union") {
        // Default to first variant case with a zero/default value
        const first_field = info.@"union".fields[0];
        return @unionInit(T, first_field.name, jsonDefault(first_field.type));
    }
    if (info == .@"struct") {
        var result: T = undefined;
        inline for (info.@"struct".fields) |f| {
            @field(result, f.name) = jsonDefault(f.type);
        }
        return result;
    }
    if (info == .array) {
        var result: T = undefined;
        for (&result) |*r| r.* = jsonDefault(info.array.child);
        return result;
    }
    @compileError("unsupported jsonDefault type: " ++ @typeName(T));
}

fn runtimeHexToBytes(hex: []const u8, allocator: Allocator) ![]u8 {
    if (hex.len % 2 != 0) return error.InvalidCharacter;
    const result = try allocator.alloc(u8, hex.len / 2);
    errdefer allocator.free(result);
    for (0..hex.len / 2) |i| {
        const hi = runtimeHexDigit(hex[2 * i]) orelse return error.InvalidCharacter;
        const lo = runtimeHexDigit(hex[2 * i + 1]) orelse return error.InvalidCharacter;
        result[i] = (@as(u8, hi) << 4) | @as(u8, lo);
    }
    return result;
}

fn runtimeHexDigit(c: u8) ?u4 {
    return switch (c) {
        '0'...'9' => @intCast(c - '0'),
        'a'...'f' => @intCast(c - 'a' + 10),
        'A'...'F' => @intCast(c - 'A' + 10),
        else => null,
    };
}

// ── Hex helpers for tests ───────────────────────────────────────────────

pub fn hexToBytes(comptime hex: []const u8) [hex.len / 2]u8 {
    var result: [hex.len / 2]u8 = undefined;
    for (0..hex.len / 2) |i| {
        result[i] = @as(u8, hexDigit(hex[2 * i])) << 4 | @as(u8, hexDigit(hex[2 * i + 1]));
    }
    return result;
}

fn hexDigit(c: u8) u4 {
    return switch (c) {
        '0'...'9' => @intCast(c - '0'),
        'a'...'f' => @intCast(c - 'a' + 10),
        'A'...'F' => @intCast(c - 'A' + 10),
        else => @panic("invalid hex digit"),
    };
}

// ── Inline tests ────────────────────────────────────────────────────────

test "marshal and view FixedInts" {
    const FixedInts = struct {
        pub const fracpack_fixed = true;
        x: i32,
        y: i32,
    };

    const data = try marshal(FixedInts, std.testing.allocator, .{ .x = 42, .y = 100 });
    defer std.testing.allocator.free(data);

    const expected = hexToBytes("2A00000064000000");
    try std.testing.expectEqualSlices(u8, &expected, data);

    const v = try view(FixedInts, data);
    try std.testing.expectEqual(@as(i32, 42), v.x);
    try std.testing.expectEqual(@as(i32, 100), v.y);
}

test "marshal and view Inner" {
    const Inner = struct {
        value: u32,
        label: []const u8,
    };

    const data = try marshal(Inner, std.testing.allocator, .{ .value = 42, .label = "hello" });
    defer std.testing.allocator.free(data);

    const expected = hexToBytes("08002A000000040000000500000068656C6C6F");
    try std.testing.expectEqualSlices(u8, &expected, data);

    const v = try view(Inner, data);
    try std.testing.expectEqual(@as(u32, 42), v.value);
    try std.testing.expectEqualSlices(u8, "hello", v.label);
}

test "marshal and view Outer" {
    const Inner = struct {
        value: u32,
        label: []const u8,
    };
    const Outer = struct {
        inner: Inner,
        name: []const u8,
    };

    const data = try marshal(Outer, std.testing.allocator, .{
        .inner = .{ .value = 1, .label = "inner" },
        .name = "outer",
    });
    defer std.testing.allocator.free(data);

    const expected = hexToBytes("080008000000170000000800010000000400000005000000696E6E6572050000006F75746572");
    try std.testing.expectEqualSlices(u8, &expected, data);

    const v = try view(Outer, data);
    try std.testing.expectEqual(@as(u32, 1), v.inner.value);
    try std.testing.expectEqualSlices(u8, "inner", v.inner.label);
    try std.testing.expectEqualSlices(u8, "outer", v.name);
}

test "marshal and view WithOptionals both_null" {
    const WithOptionals = struct {
        opt_int: ?u32,
        opt_str: ?[]const u8,
    };

    const data = try marshal(WithOptionals, std.testing.allocator, .{ .opt_int = null, .opt_str = null });
    defer std.testing.allocator.free(data);

    const expected = hexToBytes("0000");
    try std.testing.expectEqualSlices(u8, &expected, data);

    const v = try view(WithOptionals, data);
    try std.testing.expectEqual(@as(?u32, null), v.opt_int);
    try std.testing.expectEqual(@as(?[]const u8, null), v.opt_str);
}

test "marshal and view WithOptionals both_present" {
    const WithOptionals = struct {
        opt_int: ?u32,
        opt_str: ?[]const u8,
    };

    const data = try marshal(WithOptionals, std.testing.allocator, .{ .opt_int = 99, .opt_str = "world" });
    defer std.testing.allocator.free(data);

    const expected = hexToBytes("080008000000080000006300000005000000776F726C64");
    try std.testing.expectEqualSlices(u8, &expected, data);

    const v = try view(WithOptionals, data);
    try std.testing.expectEqual(@as(?u32, 99), v.opt_int);
    const s = v.opt_str orelse return error.BufferTooShort;
    try std.testing.expectEqualSlices(u8, "world", s);
}

test "marshal FixedArray" {
    const FixedArray = struct {
        pub const fracpack_fixed = true;
        arr: [3]u32,
    };

    const data = try marshal(FixedArray, std.testing.allocator, .{ .arr = .{ 1, 2, 3 } });
    defer std.testing.allocator.free(data);

    const expected = hexToBytes("010000000200000003000000");
    try std.testing.expectEqualSlices(u8, &expected, data);

    const v = try view(FixedArray, data);
    try std.testing.expectEqual(@as(u32, 1), v.arr[0]);
    try std.testing.expectEqual(@as(u32, 2), v.arr[1]);
    try std.testing.expectEqual(@as(u32, 3), v.arr[2]);
}
