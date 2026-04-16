"""Tests for zero-copy fracpack validation.

Covers:
  - Valid data returns "valid"
  - Truncated data returns "invalid"
  - Extra trailing bytes returns "invalid"
  - Bad offset (past buffer) returns "invalid"
  - Bad UTF-8 in string returns "invalid"
  - Bad bool value (2) returns "invalid"
  - Bad variant tag returns "invalid"
  - Extended struct (extra fields) returns "extended"
  - Nested struct validation
  - Vec with wrong alignment returns "invalid"
  - Valid golden vectors all pass validation
"""

from __future__ import annotations

import json
import pathlib
import struct

import pytest

from psio import schema, types as t
from psio._codec import pack
from psio._validate import ValidationResult, validate


# ── Test helpers ────────────────────────────────────────────────────────────

@schema
class Point:
    x: t.f64
    y: t.f64


@schema
class Person:
    name: t.string
    age: t.u32
    active: t.bool_


@schema
class Inner:
    value: t.u32
    label: t.string


@schema
class Outer:
    inner: Inner
    extra: t.u32


@schema
class WithOptionals:
    name: t.string
    bio: t.optional[t.string]
    score: t.optional[t.u32]


@schema
class FixedPoint:
    class Meta:
        fixed = True
    x: t.f64
    y: t.f64


@schema
class WithVec:
    items: t.vec[t.u32]


@schema
class WithVecStrings:
    items: t.vec[t.string]


@schema
class WithVariant:
    data: t.variant(uint32=t.u32, text=t.string)


@schema
class WithBytes:
    data: t.bytes_


@schema
class WithBool:
    class Meta:
        fixed = True
    value: t.bool_


# ═══════════════════════════════════════════════════════════════════════════
# Basic valid data
# ═══════════════════════════════════════════════════════════════════════════

class TestValidData:
    def test_point_valid(self):
        data = Point(x=1.0, y=2.0).pack()
        result = validate(Point.__schema__, data)
        assert result.status == "valid"
        assert result.message is None

    def test_person_valid(self):
        data = Person(name="Alice", age=30, active=True).pack()
        result = validate(Person.__schema__, data)
        assert result.status == "valid"

    def test_fixed_struct_valid(self):
        data = FixedPoint(x=3.0, y=4.0).pack()
        result = validate(FixedPoint.__schema__, data)
        assert result.status == "valid"

    def test_empty_string_field(self):
        data = Person(name="", age=0, active=False).pack()
        result = validate(Person.__schema__, data)
        assert result.status == "valid"

    def test_vec_u32_valid(self):
        data = WithVec(items=[1, 2, 3]).pack()
        result = validate(WithVec.__schema__, data)
        assert result.status == "valid"

    def test_vec_empty_valid(self):
        data = WithVec(items=[]).pack()
        result = validate(WithVec.__schema__, data)
        assert result.status == "valid"

    def test_vec_strings_valid(self):
        data = WithVecStrings(items=["hello", "world"]).pack()
        result = validate(WithVecStrings.__schema__, data)
        assert result.status == "valid"

    def test_optional_none(self):
        data = WithOptionals(name="A", bio=None, score=None).pack()
        result = validate(WithOptionals.__schema__, data)
        assert result.status == "valid"

    def test_optional_present(self):
        data = WithOptionals(name="A", bio="Dev", score=100).pack()
        result = validate(WithOptionals.__schema__, data)
        assert result.status == "valid"

    def test_variant_valid(self):
        data = WithVariant(data={"type": "uint32", "value": 42}).pack()
        result = validate(WithVariant.__schema__, data)
        assert result.status == "valid"

    def test_variant_string_valid(self):
        data = WithVariant(data={"type": "text", "value": "hi"}).pack()
        result = validate(WithVariant.__schema__, data)
        assert result.status == "valid"

    def test_bytes_valid(self):
        data = WithBytes(data=b"\x00\x01\x02\xff").pack()
        result = validate(WithBytes.__schema__, data)
        assert result.status == "valid"

    def test_bytes_empty_valid(self):
        data = WithBytes(data=b"").pack()
        result = validate(WithBytes.__schema__, data)
        assert result.status == "valid"

    def test_nested_struct_valid(self):
        data = Outer(inner=Inner(value=42, label="test"), extra=99).pack()
        result = validate(Outer.__schema__, data)
        assert result.status == "valid"


# ═══════════════════════════════════════════════════════════════════════════
# Truncated data
# ═══════════════════════════════════════════════════════════════════════════

class TestTruncated:
    def test_empty_buffer(self):
        result = validate(Point.__schema__, b"")
        assert result.status == "invalid"
        assert "past end" in result.message

    def test_one_byte(self):
        result = validate(Point.__schema__, b"\x00")
        assert result.status == "invalid"

    def test_truncated_fixed_struct(self):
        data = FixedPoint(x=1.0, y=2.0).pack()
        result = validate(FixedPoint.__schema__, data[:8])
        assert result.status == "invalid"

    def test_truncated_string(self):
        # Pack a string, then chop it
        data = Person(name="Hello World", age=30, active=True).pack()
        # Chop off part of the string data
        result = validate(Person.__schema__, data[:len(data) - 5])
        assert result.status == "invalid"

    def test_truncated_vec(self):
        data = WithVec(items=[1, 2, 3]).pack()
        result = validate(WithVec.__schema__, data[:len(data) - 2])
        assert result.status == "invalid"


# ═══════════════════════════════════════════════════════════════════════════
# Extra trailing bytes
# ═══════════════════════════════════════════════════════════════════════════

class TestExtraBytes:
    def test_extra_byte_fixed_struct(self):
        data = FixedPoint(x=1.0, y=2.0).pack() + b"\x00"
        result = validate(FixedPoint.__schema__, data)
        assert result.status == "invalid"
        assert "Extra data" in result.message

    def test_extra_byte_extensible_struct(self):
        data = Point(x=1.0, y=2.0).pack() + b"\x00"
        result = validate(Point.__schema__, data)
        assert result.status == "invalid"
        assert "Extra data" in result.message


# ═══════════════════════════════════════════════════════════════════════════
# Bad offsets
# ═══════════════════════════════════════════════════════════════════════════

class TestBadOffset:
    def test_offset_past_buffer(self):
        # Construct Person data, then corrupt the name offset to point past end
        data = bytearray(Person(name="Alice", age=30, active=True).pack())
        # The extensible struct has: u16 fixed_size, then fields
        # Field 0 (name) is at offset 2 in the packed data, a 4-byte offset
        # Overwrite with a huge offset
        struct.pack_into("<I", data, 2, 0xFFFFFFFF)
        result = validate(Person.__schema__, bytes(data))
        assert result.status == "invalid"

    def test_reserved_offset_2(self):
        # Manually build data with offset=2 (reserved)
        data = bytearray(Person(name="A", age=1, active=True).pack())
        # Corrupt the name offset to be 2 (reserved)
        struct.pack_into("<I", data, 2, 2)
        result = validate(Person.__schema__, bytes(data))
        assert result.status == "invalid"
        assert "Reserved" in result.message or "before heap" in result.message

    def test_reserved_offset_3(self):
        data = bytearray(Person(name="A", age=1, active=True).pack())
        struct.pack_into("<I", data, 2, 3)
        result = validate(Person.__schema__, bytes(data))
        assert result.status == "invalid"

    def test_backward_offset(self):
        # Build valid data, then make an offset point backward into the fixed region
        data = bytearray(WithVecStrings(items=["hello", "world"]).pack())
        # The vec has variable-size elements; corrupt one of the element offsets
        # to point backward. The exact layout:
        #   u16 fixed_size | u32 vec_offset | [vec data: u32 data_size | offsets... | heap...]
        # We need the vec to have at least 2 elements. Locate the second
        # element offset within the vec's fixed region and set it to 4 (minimum valid)
        # but it would be pointing before the heap start.
        # For a more direct test, just construct raw bytes.
        # Vec of 2 strings: data_size=8 (2 * 4-byte offsets), then heap
        # Element 0 offset should point forward to heap, element 1 also.
        # We corrupt element 1 offset to point backward.
        result = validate(WithVecStrings.__schema__, bytes(data))
        # This is valid data, should pass
        assert result.status == "valid"

        # Now construct deliberately bad data with backward offset
        # Struct with u16 header + 1 offset field (vec)
        # Build: u16(4) + u32(offset=6) => fixed_size=4, vec_offset at pos 2
        # Vec at pos 8: u32(data_size=8) + offset0 + offset1
        # offset0 should point to pos 20 (heap), offset1 should be >= offset0+data
        # Make offset1 point backward to before offset0's heap
        buf = bytearray()
        buf += struct.pack("<H", 4)   # fixed_size = 4
        buf += struct.pack("<I", 6)   # vec offset = 6 (pos 2 + 6 = 8)
        # Vec at pos 8
        buf += struct.pack("<I", 8)   # data_size = 8 (2 elements * 4 bytes each)
        # Element offsets at pos 12 and 16
        # Heap starts at pos 20
        buf += struct.pack("<I", 12)  # offset from pos 12 -> pos 24 (valid, points to heap)
        buf += struct.pack("<I", 8)   # offset from pos 16 -> pos 24 (must be >= previous end)
        # Heap data for element 0: string with length 3 "abc"
        buf += struct.pack("<I", 3) + b"abc"
        # Element 1 points to pos 24 which is the start of element 0's string data
        # This is technically a backward/overlapping reference if element 0 consumed
        # bytes up to pos 27.
        # The validator should detect this as pointing before heap.
        result = validate(WithVecStrings.__schema__, bytes(buf))
        assert result.status == "invalid"


# ═══════════════════════════════════════════════════════════════════════════
# Bad UTF-8
# ═══════════════════════════════════════════════════════════════════════════

class TestBadUtf8:
    def test_invalid_continuation(self):
        # Build a string with invalid UTF-8: 0xC0 0x01 (overlong / invalid)
        ft = t._string
        # Pack a raw string manually: u32 length + raw bytes
        bad_bytes = bytes([0xC0, 0x80])  # overlong 2-byte
        data = struct.pack("<I", len(bad_bytes)) + bad_bytes
        result = validate(ft, data)
        assert result.status == "invalid"
        assert "UTF-8" in result.message

    def test_truncated_multibyte(self):
        # Start of 3-byte sequence but only 1 continuation byte
        bad_bytes = bytes([0xE0, 0x80])
        data = struct.pack("<I", len(bad_bytes)) + bad_bytes
        result = validate(t._string, data)
        assert result.status == "invalid"
        assert "UTF-8" in result.message

    def test_invalid_lead_byte(self):
        # 0xFF is never valid in UTF-8
        bad_bytes = bytes([0xFF])
        data = struct.pack("<I", len(bad_bytes)) + bad_bytes
        result = validate(t._string, data)
        assert result.status == "invalid"
        assert "UTF-8" in result.message

    def test_valid_utf8(self):
        # Valid multi-byte UTF-8
        good = "Hello \u00e9\u00e8 \u4e16\u754c \U0001f600"
        data = pack(t._string, good)
        result = validate(t._string, data)
        assert result.status == "valid"

    def test_invalid_utf8_in_struct_field(self):
        # Pack a valid Person, then corrupt the string bytes
        data = bytearray(Person(name="Hello", age=30, active=True).pack())
        # Find where "Hello" starts in the buffer and corrupt it
        # The struct is: u16(fixed_size) + u32(name_offset) + u32(age) + u8(active) + string_data
        # fixed_size = 4 + 4 + 1 = 9
        # name_offset is at pos 2, points to pos 2 + offset
        name_offset = struct.unpack_from("<I", data, 2)[0]
        string_start = 2 + name_offset
        # At string_start there's a u32 length then the string bytes
        str_len = struct.unpack_from("<I", data, string_start)[0]
        # Corrupt first byte of actual string to invalid UTF-8
        data[string_start + 4] = 0xFF
        result = validate(Person.__schema__, bytes(data))
        assert result.status == "invalid"
        assert "UTF-8" in result.message


# ═══════════════════════════════════════════════════════════════════════════
# Bad bool value
# ═══════════════════════════════════════════════════════════════════════════

class TestBadBool:
    def test_bool_value_2(self):
        data = bytearray(WithBool(value=True).pack())
        # Fixed struct: just 1 byte for bool
        data[0] = 2
        result = validate(WithBool.__schema__, bytes(data))
        assert result.status == "invalid"
        assert "bool" in result.message.lower()

    def test_bool_value_255(self):
        data = bytearray(WithBool(value=True).pack())
        data[0] = 255
        result = validate(WithBool.__schema__, bytes(data))
        assert result.status == "invalid"

    def test_standalone_bool_valid(self):
        result = validate(t._bool, b"\x00")
        assert result.status == "valid"
        result = validate(t._bool, b"\x01")
        assert result.status == "valid"

    def test_standalone_bool_invalid(self):
        result = validate(t._bool, b"\x02")
        assert result.status == "invalid"


# ═══════════════════════════════════════════════════════════════════════════
# Bad variant tag
# ═══════════════════════════════════════════════════════════════════════════

class TestBadVariantTag:
    def test_tag_out_of_range(self):
        data = bytearray(
            WithVariant(data={"type": "uint32", "value": 42}).pack()
        )
        # The struct is: u16(fixed_size) + u32(variant_offset) + variant_data
        # variant_data = u8(tag) + u32(size) + payload
        var_offset = struct.unpack_from("<I", data, 2)[0]
        variant_pos = 2 + var_offset
        # Corrupt tag to an out-of-range value (variant has 2 cases)
        data[variant_pos] = 5
        result = validate(WithVariant.__schema__, bytes(data))
        assert result.status == "invalid"
        assert "out of range" in result.message

    def test_standalone_variant(self):
        vt = t.VariantType({"A": t._u32, "B": None})
        # Valid tag 0 (A) with u32 payload
        data = struct.pack("<B", 0) + struct.pack("<I", 4) + struct.pack("<I", 42)
        result = validate(vt, data)
        assert result.status == "valid"

        # Invalid tag 2
        data = struct.pack("<B", 2) + struct.pack("<I", 0)
        result = validate(vt, data)
        assert result.status == "invalid"


# ═══════════════════════════════════════════════════════════════════════════
# Extended struct
# ═══════════════════════════════════════════════════════════════════════════

class TestExtended:
    def test_extended_struct(self):
        # Pack a Point (2 x f64), then manually extend the fixed region
        # by adding extra bytes (simulating a newer schema version)
        data = bytearray(Point(x=1.0, y=2.0).pack())
        # Current layout: u16(16) + f64 + f64
        # Change fixed_size from 16 to 20 and add 4 extra bytes
        struct.pack_into("<H", data, 0, 20)
        data.extend(b"\x00\x00\x00\x00")  # 4 extra fixed bytes
        result = validate(Point.__schema__, bytes(data))
        assert result.status == "extended"
        assert result.message is not None

    def test_not_extended_when_exact(self):
        data = Point(x=1.0, y=2.0).pack()
        result = validate(Point.__schema__, data)
        assert result.status == "valid"


# ═══════════════════════════════════════════════════════════════════════════
# Nested struct validation
# ═══════════════════════════════════════════════════════════════════════════

class TestNestedValidation:
    def test_nested_valid(self):
        data = Outer(inner=Inner(value=42, label="test"), extra=99).pack()
        result = validate(Outer.__schema__, data)
        assert result.status == "valid"

    def test_nested_inner_corrupted(self):
        data = bytearray(Outer(inner=Inner(value=42, label="test"), extra=99).pack())
        # Corrupt the inner struct's string data with bad UTF-8
        # Find the inner struct offset
        inner_offset = struct.unpack_from("<I", data, 2)[0]
        inner_pos = 2 + inner_offset
        # Inner struct: u16(fixed_size) + u32(value) + u32(label_offset) + label_data
        inner_fixed_size = struct.unpack_from("<H", data, inner_pos)[0]
        # label offset is at inner_pos + 2 + 4 (after fixed_size + u32 value)
        label_field_pos = inner_pos + 2 + 4
        label_offset = struct.unpack_from("<I", data, label_field_pos)[0]
        label_data_pos = label_field_pos + label_offset
        # At label_data_pos: u32(len) + string bytes
        str_len = struct.unpack_from("<I", data, label_data_pos)[0]
        # Corrupt first byte to invalid UTF-8
        data[label_data_pos + 4] = 0xFF
        result = validate(Outer.__schema__, bytes(data))
        assert result.status == "invalid"
        assert "UTF-8" in result.message


# ═══════════════════════════════════════════════════════════════════════════
# Vec alignment
# ═══════════════════════════════════════════════════════════════════════════

class TestVecAlignment:
    def test_vec_u32_misaligned(self):
        # Vec of u32: data_size must be divisible by 4
        # Build a struct with vec, then corrupt data_size
        data = bytearray(WithVec(items=[1, 2]).pack())
        # Struct: u16(fixed_size=4) + u32(vec_offset) + vec_data
        vec_offset = struct.unpack_from("<I", data, 2)[0]
        vec_pos = 2 + vec_offset
        # At vec_pos: u32(data_size=8) for 2 elements
        # Change data_size to 7 (not divisible by 4)
        struct.pack_into("<I", data, vec_pos, 7)
        result = validate(WithVec.__schema__, bytes(data))
        assert result.status == "invalid"
        assert "divisible" in result.message

    def test_standalone_vec_misaligned(self):
        vt = t.VecType(t._u32)
        # data_size = 3, not divisible by 4
        data = struct.pack("<I", 3) + b"\x00\x00\x00"
        result = validate(vt, data)
        assert result.status == "invalid"
        assert "divisible" in result.message


# ═══════════════════════════════════════════════════════════════════════════
# Scalar types (standalone)
# ═══════════════════════════════════════════════════════════════════════════

class TestScalarValidation:
    def test_u8_valid(self):
        assert validate(t._u8, b"\x42").status == "valid"

    def test_u16_valid(self):
        assert validate(t._u16, struct.pack("<H", 1234)).status == "valid"

    def test_u32_valid(self):
        assert validate(t._u32, struct.pack("<I", 123456)).status == "valid"

    def test_u64_valid(self):
        assert validate(t._u64, struct.pack("<Q", 2**63)).status == "valid"

    def test_i8_valid(self):
        assert validate(t._i8, struct.pack("<b", -1)).status == "valid"

    def test_f32_valid(self):
        assert validate(t._f32, struct.pack("<f", 3.14)).status == "valid"

    def test_f64_valid(self):
        assert validate(t._f64, struct.pack("<d", 2.718)).status == "valid"

    def test_u8_empty(self):
        assert validate(t._u8, b"").status == "invalid"

    def test_u32_truncated(self):
        assert validate(t._u32, b"\x00\x00").status == "invalid"

    def test_u32_extra(self):
        result = validate(t._u32, struct.pack("<I", 1) + b"\x00")
        assert result.status == "invalid"
        assert "Extra data" in result.message


# ═══════════════════════════════════════════════════════════════════════════
# Schema class integration
# ═══════════════════════════════════════════════════════════════════════════

class TestSchemaIntegration:
    """Test that @schema.validate() now returns ValidationResult."""

    def test_validate_returns_result(self):
        data = Point(x=1.0, y=2.0).pack()
        result = Point.validate(data)
        assert isinstance(result, ValidationResult)
        assert result.status == "valid"

    def test_validate_invalid_returns_result(self):
        result = Point.validate(b"\x00")
        assert isinstance(result, ValidationResult)
        assert result.status == "invalid"


# ═══════════════════════════════════════════════════════════════════════════
# Optional edge cases
# ═══════════════════════════════════════════════════════════════════════════

class TestOptionalEdgeCases:
    def test_standalone_optional_none(self):
        ot = t.OptionalType(t._u32)
        data = pack(ot, None)
        assert validate(ot, data).status == "valid"

    def test_standalone_optional_value(self):
        ot = t.OptionalType(t._u32)
        data = pack(ot, 42)
        assert validate(ot, data).status == "valid"

    def test_standalone_optional_string_empty(self):
        ot = t.OptionalType(t._string)
        data = pack(ot, "")
        assert validate(ot, data).status == "valid"

    def test_standalone_optional_string_value(self):
        ot = t.OptionalType(t._string)
        data = pack(ot, "hello")
        assert validate(ot, data).status == "valid"


# ═══════════════════════════════════════════════════════════════════════════
# Golden vector validation
# ═══════════════════════════════════════════════════════════════════════════

VECTORS_PATH = (
    pathlib.Path(__file__).resolve().parents[2] / "test_vectors" / "vectors.json"
)


# Import golden schema classes from test_golden
from tests.test_golden import TYPE_MAP, _GOLDEN_CASES


@pytest.mark.parametrize(
    "type_name,case_name,cls,case",
    _GOLDEN_CASES,
    ids=[f"{tn}/{cn}" for tn, cn, _, _ in _GOLDEN_CASES],
)
class TestGoldenValidation:
    """Every golden vector must validate as 'valid'."""

    def test_golden_validates(self, type_name: str, case_name: str,
                              cls: type, case: dict):
        packed_bytes = bytes.fromhex(case["packed_hex"])
        result = validate(cls.__schema__, packed_bytes)
        assert result.status == "valid", (
            f"{type_name}/{case_name}: expected valid, got {result.status}: "
            f"{result.message}"
        )
