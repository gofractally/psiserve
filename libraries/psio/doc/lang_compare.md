# Fracpack Language Implementation Parity

This document tracks feature parity across all fracpack/psio language implementations.
All implementations must pass the shared golden test vectors in `test_vectors/vectors.json`.

## Core Wire Format

| Feature                    | C++  | Rust | Python | JS   | Zig  | Go   | MoonBit |
|----------------------------|:----:|:----:|:------:|:----:|:----:|:----:|:-------:|
| bool                       |  Y   |  Y   |   Y    |  Y   |  Y   |  Y   |    Y    |
| u8 / i8                    |  Y   |  Y   |   Y    |  Y   |  Y   |  Y   |    Y    |
| u16 / i16                  |  Y   |  Y   |   Y    |  Y   |  Y   |  Y   |    Y    |
| u32 / i32                  |  Y   |  Y   |   Y    |  Y   |  Y   |  Y   |    Y    |
| u64 / i64                  |  Y   |  Y   |   Y    |  Y   |  Y   |  Y   |    Y    |
| f32 / f64                  |  Y   |  Y   |   Y    |  Y   |  Y   |  Y   |    Y    |
| String (UTF-8)             |  Y   |  Y   |   Y    |  Y   |  Y   |  Y   |    Y    |
| Bytes                      |  Y   |  Y   |   Y    |  Y   |  Y   |  Y   |    Y    |
| Vec (fixed elements)       |  Y   |  Y   |   Y    |  Y   |  Y   |  Y   |    Y    |
| Vec (variable elements)    |  Y   |  Y   |   Y    |  Y   |  Y   |  Y   |    Y    |
| Nested Vec                 |  Y   |  Y   |   Y    |  Y   |  Y   |  Y   |    Y    |
| Optional                   |  Y   |  Y   |   Y    |  Y   |  Y   |  Y   |    Y    |
| Array (fixed-length)       |  Y   |  Y   |   Y    |  Y   |  Y   |  Y   |    Y    |
| Variant (tagged union)     |  Y   |  Y   |   Y    |  Y   |  Y   |  Y   |    Y    |
| Tuple                      |  Y   |  Y   |   Y    |  Y   |  Y   |  Y   |    Y    |
| Extensible struct          |  Y   |  Y   |   Y    |  Y   |  Y   |  Y   |    Y    |
| DWC (fixed) struct         |  Y   |  Y   |   Y    |  Y   |  Y   |  Y   |    Y    |
| Nested structs             |  Y   |  Y   |   Y    |  Y   |  Y   |  Y   |    Y    |
| Trailing optional elision  |  Y   |  Y   |   Y    |  Y   |  Y   |  Y   |    Y    |

## Advanced Features

| Feature                    | C++  | Rust | Python | JS   | Zig  | Go   | MoonBit |
|----------------------------|:----:|:----:|:------:|:----:|:----:|:----:|:-------:|
| Zero-copy views            |  Y   |  Y   |   Y    |  Y   |  Y   |  Y   |    Y    |
| Nested views               |  Y   |  Y   |   Y    |  Y   |  Y   |  Y   |    Y    |
| In-place mutation          |  Y   |  Y   |   Y    |  Y   |  Y   |  Y   |    Y    |
| Golden test vectors        |  Y   |  Y   |   Y    |  Y   |  Y   |  Y   |    Y    |
| Validation                 |  Y   |  Y   |   Y    |  Y   |  Y   |  Y   |    Y    |
| Canonical JSON             |  Y   |  Y   |   Y    |  Y   |  Y   |  Y   |    Y    |

## Schema Definition

| Language | Mechanism                                      |
|----------|-------------------------------------------------|
| C++      | `PSIO_REFLECT` macro + C++23 concepts           |
| Rust     | `#[derive(Pack, Unpack)]` proc macros           |
| Python   | `@schema` decorator + `typing.Annotated`        |
| JS       | Type builder functions                          |
| Zig      | Comptime reflection                             |
| Go       | Manual `TypeDef` structs                        |
| MoonBit  | Manual `TypeSpec` / `FieldSpec` builders         |

## Canonical JSON: Why Not Just Use the Platform JSON Library?

Every language has a standard JSON library, but none of them produce the
**canonical** fracpack JSON format out of the box. Without schema-driven
conversion, users must write manual encoders/decoders per type — error-prone
and inconsistent across services.

| Language | Platform JSON | Key gap fracpack JSON solves |
|----------|---------------|------------------------------|
| C++ | RapidJSON (3rd party) | No schema-driven traversal; manual `AddMember` per field |
| Rust | `serde_json` | u64/i64 serialize as numbers (overflow JS 2^53), `Vec<u8>` as number arrays not hex, enum tagging varies by `#[serde(tag)]` |
| Python | `json` (stdlib) | u64/i64 as numbers (no JS-safe overflow), `bytes` not hex-encoded, no variant/optional semantics |
| JS | `JSON.stringify` | BigInt throws on stringify, `Uint8Array` becomes object not hex, no variant convention |
| Zig | `std.json` | All integers as numbers, no hex bytes, no union → `{"Case": value}` convention |
| Go | `encoding/json` | u64/i64 as numbers, `[]byte` as base64 (not hex!), no variant/union type, custom `MarshalJSON` per type |
| MoonBit | (ecosystem maturing) | No established convention for hex bytes, u64 strings, or variant encoding |

## Remaining Gaps

No feature gaps remain — all 7 implementations have full parity.
