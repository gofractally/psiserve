// WIT (WebAssembly Interface Types) data structures.
//
// These mirror the Component Model's type system: records, variants, enums,
// flags, lists, options, results, tuples, and function signatures.
//
// Type references use index-based encoding:
//   - type_idx >= 0  -> index into WitWorld.types[]
//   - type_idx <  0  -> primitive: idxToPrim(type_idx)
//
// Helpers primIdx() and idxToPrim() convert between the two.

/// WIT primitive value types.
pub const WitPrim = enum(u8) {
    bool_ = 0,
    u8_ = 1,
    s8 = 2,
    u16_ = 3,
    s16 = 4,
    u32_ = 5,
    s32 = 6,
    u64_ = 7,
    s64 = 8,
    f32_ = 9,
    f64_ = 10,
    char_ = 11,
    string_ = 12,
};

/// WIT compound type kinds.
pub const WitTypeKind = enum(u8) {
    record = 0,
    variant = 1,
    enum_ = 2,
    flags = 3,
    list = 4,
    option = 5,
    result = 6,
    tuple = 7,
};

/// Encode a primitive as a negative type index.
pub fn primIdx(p: WitPrim) i32 {
    return -(@as(i32, @intCast(@intFromEnum(p))) + 1);
}

/// Decode a negative type index back to a primitive.
pub fn idxToPrim(idx: i32) WitPrim {
    return @enumFromInt(@as(u8, @intCast(-(idx + 1))));
}

/// True if the type index refers to a primitive (negative).
pub fn isPrimIdx(idx: i32) bool {
    return idx < 0;
}

/// A named field within a record, variant case, function param/result,
/// or label within an enum/flags.
pub const WitNamedType = struct {
    name: []const u8,
    type_idx: i32,
};

/// A compound type definition (record, variant, enum, flags, list, etc.).
pub const WitTypeDef = struct {
    name: []const u8,
    kind: WitTypeKind,
    fields: []const WitNamedType,
    element_type_idx: i32 = 0,
    error_type_idx: i32 = 0,
};

/// A WIT function signature with optional link to a WASM core export.
pub const WitFunc = struct {
    name: []const u8,
    params: []const WitNamedType,
    results: []const WitNamedType,
    core_func_idx: u32 = 0xFFFFFFFF,
};

/// A named group of types and functions (WIT interface).
pub const WitInterface = struct {
    name: []const u8,
    type_idxs: []const u32,
    func_idxs: []const u32,
};

/// Complete WIT world definition.
pub const WitWorld = struct {
    package: []const u8,
    name: []const u8,
    wit_source: []const u8 = "",
    types: []const WitTypeDef,
    funcs: []const WitFunc,
    exports: []const WitInterface,
    imports: []const WitInterface = &.{},
};
