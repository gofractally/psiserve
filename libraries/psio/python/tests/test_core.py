"""Core tests for the psio fracpack library.

Tests cover:
  - Scalar round-trips (bool, int, float)
  - String and bytes
  - Vec (fixed-size and variable-size elements)
  - Optional (None, empty, populated)
  - Struct (extensible and DWC)
  - Nested structs
  - Trailing optional elision
  - View lazy field access
  - Variant pack/unpack
  - Array pack/unpack
  - Edge cases (empty strings, empty vecs, zero values)
"""

from __future__ import annotations

import struct

import pytest

from psio import schema, types as t
from psio._codec import PackBuffer, UnpackCursor, pack, unpack


# ── Helper: round-trip a raw FracType ──────────────────────────────────────

def rt(frac_type, value):
    """Pack then unpack *value* through *frac_type*; return the result."""
    data = pack(frac_type, value)
    return unpack(frac_type, data)


# ═══════════════════════════════════════════════════════════════════════════
# Scalar round-trips
# ═══════════════════════════════════════════════════════════════════════════

class TestScalars:
    def test_bool_true(self):
        assert rt(t._bool, True) is True

    def test_bool_false(self):
        assert rt(t._bool, False) is False

    def test_u8(self):
        assert rt(t._u8, 0) == 0
        assert rt(t._u8, 255) == 255

    def test_u16(self):
        assert rt(t._u16, 0) == 0
        assert rt(t._u16, 65535) == 65535

    def test_u32(self):
        assert rt(t._u32, 0) == 0
        assert rt(t._u32, 2**32 - 1) == 2**32 - 1

    def test_u64(self):
        assert rt(t._u64, 0) == 0
        assert rt(t._u64, 2**64 - 1) == 2**64 - 1

    def test_i8(self):
        assert rt(t._i8, -128) == -128
        assert rt(t._i8, 127) == 127

    def test_i16(self):
        assert rt(t._i16, -32768) == -32768
        assert rt(t._i16, 32767) == 32767

    def test_i32(self):
        assert rt(t._i32, -(2**31)) == -(2**31)
        assert rt(t._i32, 2**31 - 1) == 2**31 - 1

    def test_i64(self):
        assert rt(t._i64, -(2**63)) == -(2**63)
        assert rt(t._i64, 2**63 - 1) == 2**63 - 1

    def test_f32(self):
        # f32 has limited precision
        val = rt(t._f32, 3.14)
        assert abs(val - 3.14) < 1e-5

    def test_f64(self):
        assert rt(t._f64, 3.141592653589793) == 3.141592653589793

    def test_f32_zero(self):
        assert rt(t._f32, 0.0) == 0.0

    def test_f64_negative(self):
        assert rt(t._f64, -1e100) == -1e100


# ═══════════════════════════════════════════════════════════════════════════
# String and Bytes
# ═══════════════════════════════════════════════════════════════════════════

class TestStringBytes:
    def test_string_basic(self):
        assert rt(t._string, "hello") == "hello"

    def test_string_empty(self):
        assert rt(t._string, "") == ""

    def test_string_unicode(self):
        assert rt(t._string, "こんにちは") == "こんにちは"

    def test_string_emoji(self):
        assert rt(t._string, "🎉🚀") == "🎉🚀"

    def test_bytes_basic(self):
        assert rt(t._bytes, b"\x00\x01\x02\xff") == b"\x00\x01\x02\xff"

    def test_bytes_empty(self):
        assert rt(t._bytes, b"") == b""


# ═══════════════════════════════════════════════════════════════════════════
# Vec
# ═══════════════════════════════════════════════════════════════════════════

class TestVec:
    def test_vec_u32(self):
        vt = t.VecType(t._u32)
        assert rt(vt, [1, 2, 3]) == [1, 2, 3]

    def test_vec_u32_empty(self):
        vt = t.VecType(t._u32)
        assert rt(vt, []) == []

    def test_vec_string(self):
        vt = t.VecType(t._string)
        assert rt(vt, ["hello", "world"]) == ["hello", "world"]

    def test_vec_string_with_empty(self):
        vt = t.VecType(t._string)
        assert rt(vt, ["", "a", ""]) == ["", "a", ""]

    def test_vec_bool(self):
        vt = t.VecType(t._bool)
        assert rt(vt, [True, False, True]) == [True, False, True]

    def test_vec_nested(self):
        """Vec of vec of u32."""
        inner_vt = t.VecType(t._u32)
        outer_vt = t.VecType(inner_vt)
        assert rt(outer_vt, [[1, 2], [3]]) == [[1, 2], [3]]

    def test_vec_u8(self):
        vt = t.VecType(t._u8)
        assert rt(vt, [0, 127, 255]) == [0, 127, 255]


# ═══════════════════════════════════════════════════════════════════════════
# Optional
# ═══════════════════════════════════════════════════════════════════════════

class TestOptional:
    def test_optional_u32_none(self):
        ot = t.OptionalType(t._u32)
        assert rt(ot, None) is None

    def test_optional_u32_value(self):
        ot = t.OptionalType(t._u32)
        assert rt(ot, 42) == 42

    def test_optional_string_none(self):
        ot = t.OptionalType(t._string)
        assert rt(ot, None) is None

    def test_optional_string_value(self):
        ot = t.OptionalType(t._string)
        assert rt(ot, "hello") == "hello"

    def test_optional_string_empty(self):
        ot = t.OptionalType(t._string)
        assert rt(ot, "") == ""

    def test_optional_vec_none(self):
        ot = t.OptionalType(t.VecType(t._u32))
        assert rt(ot, None) is None

    def test_optional_vec_empty(self):
        ot = t.OptionalType(t.VecType(t._u32))
        assert rt(ot, []) == []

    def test_optional_vec_value(self):
        ot = t.OptionalType(t.VecType(t._u32))
        assert rt(ot, [1, 2, 3]) == [1, 2, 3]


# ═══════════════════════════════════════════════════════════════════════════
# Struct: basic extensible
# ═══════════════════════════════════════════════════════════════════════════

@schema
class Point:
    x: t.f64
    y: t.f64


@schema
class Person:
    name: t.string
    age: t.u32
    active: t.bool_


class TestStructBasic:
    def test_point_round_trip(self):
        p = Point(x=1.5, y=2.5)
        data = p.pack()
        p2 = Point.unpack(data)
        assert p2.x == 1.5
        assert p2.y == 2.5

    def test_person_round_trip(self):
        p = Person(name="Alice", age=30, active=True)
        data = p.pack()
        p2 = Person.unpack(data)
        assert p2.name == "Alice"
        assert p2.age == 30
        assert p2.active is True

    def test_person_empty_name(self):
        p = Person(name="", age=0, active=False)
        data = p.pack()
        p2 = Person.unpack(data)
        assert p2.name == ""
        assert p2.age == 0
        assert p2.active is False

    def test_equality(self):
        p1 = Person(name="Bob", age=25, active=True)
        p2 = Person(name="Bob", age=25, active=True)
        assert p1 == p2

    def test_inequality(self):
        p1 = Person(name="Bob", age=25, active=True)
        p2 = Person(name="Bob", age=26, active=True)
        assert p1 != p2


# ═══════════════════════════════════════════════════════════════════════════
# Struct: DWC (definitionWillNotChange / fixed)
# ═══════════════════════════════════════════════════════════════════════════

@schema
class FixedPoint:
    class Meta:
        fixed = True
    x: t.f64
    y: t.f64


class TestStructDWC:
    def test_fixed_round_trip(self):
        p = FixedPoint(x=3.0, y=4.0)
        data = p.pack()
        p2 = FixedPoint.unpack(data)
        assert p2.x == 3.0
        assert p2.y == 4.0

    def test_fixed_no_header(self):
        """DWC structs should have no u16 header — exactly 16 bytes for two f64."""
        p = FixedPoint(x=1.0, y=2.0)
        data = p.pack()
        assert len(data) == 16  # two f64, no header


# ═══════════════════════════════════════════════════════════════════════════
# Struct with containers
# ═══════════════════════════════════════════════════════════════════════════

@schema
class WithContainers:
    tags: t.vec[t.string]
    data: t.bytes_
    count: t.u32


class TestStructContainers:
    def test_round_trip(self):
        obj = WithContainers(tags=["a", "b"], data=b"\x01\x02", count=42)
        data = obj.pack()
        obj2 = WithContainers.unpack(data)
        assert obj2.tags == ["a", "b"]
        assert obj2.data == b"\x01\x02"
        assert obj2.count == 42

    def test_empty_containers(self):
        obj = WithContainers(tags=[], data=b"", count=0)
        data = obj.pack()
        obj2 = WithContainers.unpack(data)
        assert obj2.tags == []
        assert obj2.data == b""
        assert obj2.count == 0


# ═══════════════════════════════════════════════════════════════════════════
# Trailing optional elision
# ═══════════════════════════════════════════════════════════════════════════

@schema
class WithOptionals:
    name: t.string
    bio: t.optional[t.string]
    score: t.optional[t.u32]


class TestTrailingOptionalElision:
    def test_all_present(self):
        obj = WithOptionals(name="Alice", bio="Dev", score=100)
        data = obj.pack()
        obj2 = WithOptionals.unpack(data)
        assert obj2.name == "Alice"
        assert obj2.bio == "Dev"
        assert obj2.score == 100

    def test_trailing_none_elided(self):
        """When trailing optionals are None, fixed region should be smaller."""
        all_present = WithOptionals(name="A", bio="B", score=1)
        trailing_none = WithOptionals(name="A", bio=None, score=None)

        data_all = all_present.pack()
        data_none = trailing_none.pack()

        # Both should round-trip correctly
        obj_all = WithOptionals.unpack(data_all)
        obj_none = WithOptionals.unpack(data_none)
        assert obj_all.score == 1
        assert obj_none.bio is None
        assert obj_none.score is None

        # The elided version should be smaller
        assert len(data_none) < len(data_all)

    def test_middle_none(self):
        """Non-trailing None optionals can't be elided."""
        obj = WithOptionals(name="A", bio=None, score=42)
        data = obj.pack()
        obj2 = WithOptionals.unpack(data)
        assert obj2.bio is None
        assert obj2.score == 42


# ═══════════════════════════════════════════════════════════════════════════
# Nested structs
# ═══════════════════════════════════════════════════════════════════════════

@schema
class Inner:
    value: t.u32
    label: t.string


@schema
class Outer:
    inner: Inner
    extra: t.u32


class TestNestedStruct:
    def test_nested_round_trip(self):
        obj = Outer(inner=Inner(value=42, label="test"), extra=99)
        data = obj.pack()
        obj2 = Outer.unpack(data)
        assert obj2.inner.value == 42
        assert obj2.inner.label == "test"
        assert obj2.extra == 99


# ═══════════════════════════════════════════════════════════════════════════
# Views
# ═══════════════════════════════════════════════════════════════════════════

class TestView:
    def test_view_scalar(self):
        p = Point(x=1.5, y=2.5)
        data = p.pack()
        v = Point.view(data)
        assert v.x == 1.5
        assert v.y == 2.5

    def test_view_string(self):
        p = Person(name="Alice", age=30, active=True)
        data = p.pack()
        v = Person.view(data)
        assert v.name == "Alice"
        assert v.age == 30
        assert v.active is True

    def test_view_read_only(self):
        p = Point(x=1.0, y=2.0)
        v = Point.view(p.pack())
        with pytest.raises(AttributeError, match="read-only"):
            v.x = 3.0

    def test_view_bad_field(self):
        p = Point(x=1.0, y=2.0)
        v = Point.view(p.pack())
        with pytest.raises(AttributeError, match="no field"):
            _ = v.z

    def test_view_repr(self):
        p = Point(x=1.0, y=2.0)
        v = Point.view(p.pack())
        r = repr(v)
        assert "Point.View" in r
        assert "x=1.0" in r

    def test_view_unpack(self):
        p = Person(name="Alice", age=30, active=True)
        v = Person.view(p.pack())
        d = v.unpack()
        assert d == {"name": "Alice", "age": 30, "active": True}

    def test_view_eq(self):
        data = Point(x=1.0, y=2.0).pack()
        v1 = Point.view(data)
        v2 = Point.view(data)
        assert v1 == v2

    def test_view_containers(self):
        obj = WithContainers(tags=["a", "b"], data=b"\x01\x02", count=42)
        data = obj.pack()
        v = WithContainers.view(data)
        assert v.count == 42
        assert v.tags == ["a", "b"]
        assert v.data == b"\x01\x02"

    def test_view_optional_present(self):
        obj = WithOptionals(name="A", bio="Dev", score=100)
        v = WithOptionals.view(obj.pack())
        assert v.bio == "Dev"
        assert v.score == 100

    def test_view_optional_none(self):
        obj = WithOptionals(name="A", bio=None, score=None)
        v = WithOptionals.view(obj.pack())
        assert v.bio is None
        assert v.score is None

    def test_read_field(self):
        data = Person(name="Alice", age=30, active=True).pack()
        assert Person.read_field(data, "name") == "Alice"
        assert Person.read_field(data, "age") == 30

    def test_validate_good(self):
        data = Point(x=1.0, y=2.0).pack()
        result = Point.validate(data)
        assert result.status == "valid"

    def test_validate_bad(self):
        result = Point.validate(b"\x00")
        assert result.status == "invalid"


# ═══════════════════════════════════════════════════════════════════════════
# Variant
# ═══════════════════════════════════════════════════════════════════════════

class TestVariant:
    def test_variant_with_value(self):
        Shape = t.variant(Circle=t.f64, Point=None)
        ft = t.extract_frac_type(Shape)
        circle = {"type": "Circle", "value": 5.0}
        assert rt(ft, circle) == circle

    def test_variant_no_value(self):
        Shape = t.variant(Circle=t.f64, Point=None)
        ft = t.extract_frac_type(Shape)
        point = {"type": "Point", "value": None}
        assert rt(ft, point) == point

    def test_variant_unknown_case(self):
        Shape = t.variant(A=t.u32)
        ft = t.extract_frac_type(Shape)
        with pytest.raises(ValueError, match="Unknown variant case"):
            pack(ft, {"type": "Z", "value": 0})


# ═══════════════════════════════════════════════════════════════════════════
# Array
# ═══════════════════════════════════════════════════════════════════════════

class TestArray:
    def test_array_fixed(self):
        arr_t = t.ArrayType(t._u32, 3)
        assert rt(arr_t, [10, 20, 30]) == [10, 20, 30]

    def test_array_wrong_length(self):
        arr_t = t.ArrayType(t._u32, 3)
        with pytest.raises(ValueError, match="expects 3 elements"):
            pack(arr_t, [1, 2])

    def test_array_bool(self):
        arr_t = t.ArrayType(t._bool, 2)
        assert rt(arr_t, [True, False]) == [True, False]


# ═══════════════════════════════════════════════════════════════════════════
# PackBuffer / UnpackCursor
# ═══════════════════════════════════════════════════════════════════════════

class TestBufferCursor:
    def test_write_read_u8(self):
        w = PackBuffer()
        w.write_u8(42)
        c = UnpackCursor(w.finish())
        assert c.read_u8() == 42

    def test_write_read_u32(self):
        w = PackBuffer()
        w.write_u32(0xDEADBEEF)
        c = UnpackCursor(w.finish())
        assert c.read_u32() == 0xDEADBEEF

    def test_write_read_u64(self):
        w = PackBuffer()
        w.write_u64(2**64 - 1)
        c = UnpackCursor(w.finish())
        assert c.read_u64() == 2**64 - 1

    def test_patch_u32(self):
        w = PackBuffer()
        pos = w.pos
        w.write_u32(0)
        w.write_u32(0xFF)
        w.patch_u32(pos, 123)
        c = UnpackCursor(w.finish())
        assert c.read_u32() == 123
        assert c.read_u32() == 0xFF


# ═══════════════════════════════════════════════════════════════════════════
# Edge cases
# ═══════════════════════════════════════════════════════════════════════════

class TestEdgeCases:
    def test_large_string(self):
        s = "x" * 10_000
        assert rt(t._string, s) == s

    def test_large_vec(self):
        vt = t.VecType(t._u32)
        vals = list(range(1000))
        assert rt(vt, vals) == vals

    def test_vec_of_optional(self):
        ot = t.OptionalType(t._u32)
        vt = t.VecType(ot)
        vals = [1, None, 3, None]
        assert rt(vt, vals) == vals

    def test_nested_optional(self):
        """Optional of optional is not a standard fracpack construct,
        but the codec should handle it if explicitly constructed."""
        inner = t.OptionalType(t._u32)
        outer = t.OptionalType(inner)
        assert rt(outer, None) is None
        assert rt(outer, 42) == 42

    def test_struct_with_all_types(self):
        @schema
        class AllTypes:
            b: t.bool_
            u8_: t.u8
            u16_: t.u16
            u32_: t.u32
            u64_: t.u64
            i8_: t.i8
            i16_: t.i16
            i32_: t.i32
            i64_: t.i64
            f32_: t.f32
            f64_: t.f64
            s: t.string
            by: t.bytes_

        obj = AllTypes(
            b=True, u8_=1, u16_=2, u32_=3, u64_=4,
            i8_=-1, i16_=-2, i32_=-3, i64_=-4,
            f32_=1.5, f64_=2.5, s="test", by=b"\xff",
        )
        data = obj.pack()
        obj2 = AllTypes.unpack(data)
        assert obj2.b is True
        assert obj2.u32_ == 3
        assert obj2.i64_ == -4
        assert obj2.s == "test"
        assert obj2.by == b"\xff"

    def test_schema_no_fields_raises(self):
        with pytest.raises(TypeError, match="no psio-typed fields"):
            @schema
            class Empty:
                x: int  # plain int, not a psio type
