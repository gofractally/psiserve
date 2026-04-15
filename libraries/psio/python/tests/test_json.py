"""Tests for canonical JSON encoding/decoding.

Covers golden vector round-trips, type-specific encoding rules,
edge cases, and @schema integration (to_json / from_json methods).
"""

from __future__ import annotations

import json
import math
import pathlib
from typing import Any

import pytest

from psio import from_json, schema, to_json, types as t
from psio._codec import pack, unpack
from psio._json import _jsonable_to_value, _value_to_jsonable
from psio.types import (
    ArrayType,
    BoolType,
    BytesType,
    FloatType,
    FracType,
    IntType,
    OptionalType,
    StringType,
    StructType,
    TupleType,
    VariantType,
    VecType,
    extract_frac_type,
)


# ── Test schema classes ──────────────────────────────────────────────────────

@schema
class GFixedInts:
    class Meta:
        fixed = True
    x: t.i32
    y: t.i32


@schema
class GFixedMixed:
    class Meta:
        fixed = True
    b: t.bool_
    u8: t.u8
    u16: t.u16
    u32: t.u32
    u64: t.u64


@schema
class GAllPrimitives:
    b: t.bool_
    u8v: t.u8
    i8v: t.i8
    u16v: t.u16
    i16v: t.i16
    u32v: t.u32
    i32v: t.i32
    u64v: t.u64
    i64v: t.i64
    f32v: t.f32
    f64v: t.f64


@schema
class GSingleBool:
    class Meta:
        fixed = True
    value: t.bool_


@schema
class GSingleU32:
    class Meta:
        fixed = True
    value: t.u32


@schema
class GSingleString:
    value: t.string


@schema
class GWithStrings:
    empty_str: t.string
    hello: t.string
    unicode: t.string


@schema
class GWithVectors:
    ints: t.vec[t.u32]
    strings: t.vec[t.string]


@schema
class GWithOptionals:
    opt_int: t.optional[t.u32]
    opt_str: t.optional[t.string]


@schema
class GInner:
    value: t.u32
    label: t.string


@schema
class GOuter:
    inner: GInner
    name: t.string


@schema
class GWithVariant:
    data: t.variant(uint32=t.u32, string=t.string, Inner=GInner)


@schema
class GVecOfStructs:
    items: t.vec[GInner]


@schema
class GOptionalStruct:
    item: t.optional[GInner]


@schema
class GVecOfOptionals:
    items: t.vec[t.optional[t.u32]]


@schema
class GOptionalVec:
    items: t.optional[t.vec[t.u32]]


@schema
class GNestedVecs:
    matrix: t.vec[t.vec[t.u32]]


@schema
class GFixedArray:
    class Meta:
        fixed = True
    arr: t.array[t.u32, 3]


@schema
class GComplex:
    items: t.vec[GInner]
    opt_vec: t.optional[t.vec[t.u32]]
    vec_opt: t.vec[t.optional[t.string]]
    opt_struct: t.optional[GInner]


@schema
class GEmptyExtensible:
    dummy: t.u32


TYPE_MAP: dict[str, type] = {
    "FixedInts": GFixedInts,
    "FixedMixed": GFixedMixed,
    "AllPrimitives": GAllPrimitives,
    "SingleBool": GSingleBool,
    "SingleU32": GSingleU32,
    "SingleString": GSingleString,
    "WithStrings": GWithStrings,
    "WithVectors": GWithVectors,
    "WithOptionals": GWithOptionals,
    "Inner": GInner,
    "Outer": GOuter,
    "WithVariant": GWithVariant,
    "VecOfStructs": GVecOfStructs,
    "OptionalStruct": GOptionalStruct,
    "VecOfOptionals": GVecOfOptionals,
    "OptionalVec": GOptionalVec,
    "NestedVecs": GNestedVecs,
    "FixedArray": GFixedArray,
    "Complex": GComplex,
    "EmptyExtensible": GEmptyExtensible,
}


# ── Load golden vectors ──────────────────────────────────────────────────────

VECTORS_PATH = (
    pathlib.Path(__file__).resolve().parents[2] / "test_vectors" / "vectors.json"
)


def _load_vectors() -> list[dict]:
    with open(VECTORS_PATH) as f:
        data = json.load(f)
    assert data["format_version"] == 1
    return data["types"]


def _collect_cases() -> list[tuple[str, str, type, dict]]:
    if not VECTORS_PATH.exists():
        return []
    cases = []
    for type_entry in _load_vectors():
        type_name = type_entry["name"]
        cls = TYPE_MAP.get(type_name)
        if cls is None:
            continue
        for case in type_entry["cases"]:
            cases.append((type_name, case["name"], cls, case))
    return cases


_GOLDEN_CASES = _collect_cases()


# ── 1. Golden vector round-trip tests ────────────────────────────────────────

@pytest.mark.parametrize(
    "type_name,case_name,cls,case",
    _GOLDEN_CASES,
    ids=[f"{t}/{c}" for t, c, _, _ in _GOLDEN_CASES],
)
class TestGoldenJsonRoundTrip:
    """Verify JSON canonical format matches test vectors."""

    def test_pack_to_json_matches_vector(
        self, type_name: str, case_name: str, cls: type, case: dict
    ):
        """pack bytes -> unpack -> to_json should match the vector's JSON."""
        packed_bytes = bytes.fromhex(case["packed_hex"])
        unpacked = cls.unpack(packed_bytes)
        actual_json_str = to_json(unpacked)
        expected_json_str = json.dumps(case["json"], separators=(",", ":"))
        assert actual_json_str == expected_json_str, (
            f"{type_name}/{case_name}: to_json mismatch\n"
            f"  expected: {expected_json_str}\n"
            f"  actual:   {actual_json_str}"
        )

    def test_from_json_pack_matches_vector(
        self, type_name: str, case_name: str, cls: type, case: dict
    ):
        """from_json -> pack should produce the same packed bytes as the vector."""
        json_str = json.dumps(case["json"], separators=(",", ":"))
        obj = from_json(json_str, cls)
        actual_hex = obj.pack().hex().upper()
        expected_hex = case["packed_hex"]
        assert actual_hex == expected_hex, (
            f"{type_name}/{case_name}: from_json->pack mismatch\n"
            f"  expected: {expected_hex}\n"
            f"  actual:   {actual_hex}"
        )

    def test_json_round_trip(
        self, type_name: str, case_name: str, cls: type, case: dict
    ):
        """JSON string -> from_json -> to_json should be identity."""
        json_str = json.dumps(case["json"], separators=(",", ":"))
        obj = from_json(json_str, cls)
        round_tripped = to_json(obj)
        assert round_tripped == json_str, (
            f"{type_name}/{case_name}: JSON round-trip mismatch\n"
            f"  expected: {json_str}\n"
            f"  actual:   {round_tripped}"
        )


# ── 2. Type-specific tests ──────────────────────────────────────────────────

class TestIntEncoding:
    """u64 and i64 encode as JSON strings; smaller ints as numbers."""

    def test_u64_as_string(self):
        ft = extract_frac_type(t.u64)
        result = _value_to_jsonable(ft, 12345678901234)
        assert result == "12345678901234"
        assert isinstance(result, str)

    def test_i64_as_string(self):
        ft = extract_frac_type(t.i64)
        result = _value_to_jsonable(ft, -9223372036854775808)
        assert result == "-9223372036854775808"
        assert isinstance(result, str)

    def test_u32_as_number(self):
        ft = extract_frac_type(t.u32)
        result = _value_to_jsonable(ft, 42)
        assert result == 42
        assert isinstance(result, int)

    def test_i32_as_number(self):
        ft = extract_frac_type(t.i32)
        result = _value_to_jsonable(ft, -1)
        assert result == -1
        assert isinstance(result, int)

    def test_u8_as_number(self):
        ft = extract_frac_type(t.u8)
        result = _value_to_jsonable(ft, 255)
        assert result == 255

    def test_u64_from_string(self):
        ft = extract_frac_type(t.u64)
        result = _jsonable_to_value(ft, "18446744073709551615")
        assert result == 18446744073709551615

    def test_i64_from_string(self):
        ft = extract_frac_type(t.i64)
        result = _jsonable_to_value(ft, "-1")
        assert result == -1


class TestBytesEncoding:
    """Bytes encode as lowercase hex strings."""

    def test_bytes_to_hex(self):
        ft = extract_frac_type(t.bytes_)
        result = _value_to_jsonable(ft, b"\xde\xad\xbe\xef")
        assert result == "deadbeef"

    def test_empty_bytes(self):
        ft = extract_frac_type(t.bytes_)
        result = _value_to_jsonable(ft, b"")
        assert result == ""

    def test_hex_to_bytes(self):
        ft = extract_frac_type(t.bytes_)
        result = _jsonable_to_value(ft, "deadbeef")
        assert result == b"\xde\xad\xbe\xef"

    def test_empty_hex_to_bytes(self):
        ft = extract_frac_type(t.bytes_)
        result = _jsonable_to_value(ft, "")
        assert result == b""


class TestVariantEncoding:
    """Variants encode as single-key JSON objects."""

    def test_variant_to_json(self):
        vt = extract_frac_type(t.variant(Circle=t.f64, Point=None))
        result = _value_to_jsonable(vt, {"type": "Circle", "value": 3.14})
        assert result == {"Circle": 3.14}

    def test_variant_none_case(self):
        vt = extract_frac_type(t.variant(Circle=t.f64, Point=None))
        result = _value_to_jsonable(vt, {"type": "Point", "value": None})
        assert result == {"Point": None}

    def test_variant_from_json(self):
        vt = extract_frac_type(t.variant(Circle=t.f64, Point=None))
        result = _jsonable_to_value(vt, {"Circle": 3.14})
        assert result == {"type": "Circle", "value": 3.14}

    def test_variant_none_from_json(self):
        vt = extract_frac_type(t.variant(Circle=t.f64, Point=None))
        result = _jsonable_to_value(vt, {"Point": None})
        assert result == {"type": "Point", "value": None}


class TestOptionalEncoding:
    """Optional None -> null, Some(x) -> recurse."""

    def test_optional_none(self):
        ft = extract_frac_type(t.optional[t.u32])
        result = _value_to_jsonable(ft, None)
        assert result is None

    def test_optional_some(self):
        ft = extract_frac_type(t.optional[t.u32])
        result = _value_to_jsonable(ft, 42)
        assert result == 42

    def test_optional_from_none(self):
        ft = extract_frac_type(t.optional[t.u32])
        result = _jsonable_to_value(ft, None)
        assert result is None

    def test_optional_from_some(self):
        ft = extract_frac_type(t.optional[t.u32])
        result = _jsonable_to_value(ft, 42)
        assert result == 42


class TestVecEncoding:
    """Vec and Array encode as JSON arrays."""

    def test_empty_vec(self):
        ft = extract_frac_type(t.vec[t.u32])
        result = _value_to_jsonable(ft, [])
        assert result == []

    def test_vec_of_ints(self):
        ft = extract_frac_type(t.vec[t.u32])
        result = _value_to_jsonable(ft, [1, 2, 3])
        assert result == [1, 2, 3]

    def test_nested_vecs(self):
        ft = extract_frac_type(t.vec[t.vec[t.u32]])
        result = _value_to_jsonable(ft, [[1, 2], [3]])
        assert result == [[1, 2], [3]]

    def test_vec_from_json(self):
        ft = extract_frac_type(t.vec[t.u32])
        result = _jsonable_to_value(ft, [1, 2, 3])
        assert result == [1, 2, 3]


class TestNestedStruct:
    """Nested structs encode as nested JSON objects."""

    def test_nested_to_json(self):
        inner = GInner(value=42, label="hello")
        outer = GOuter(inner=inner, name="test")
        result = json.loads(to_json(outer))
        assert result == {
            "inner": {"value": 42, "label": "hello"},
            "name": "test",
        }

    def test_nested_from_json(self):
        s = '{"inner":{"value":42,"label":"hello"},"name":"test"}'
        obj = from_json(s, GOuter)
        assert isinstance(obj, GOuter)
        assert isinstance(obj.inner, GInner)
        assert obj.inner.value == 42
        assert obj.inner.label == "hello"
        assert obj.name == "test"


class TestFloatEncoding:
    """Floats encode as JSON numbers; NaN/Inf -> null."""

    def test_float_normal(self):
        ft = extract_frac_type(t.f64)
        assert _value_to_jsonable(ft, 3.14) == 3.14

    def test_float_zero(self):
        ft = extract_frac_type(t.f32)
        assert _value_to_jsonable(ft, 0.0) == 0.0

    def test_float_nan(self):
        ft = extract_frac_type(t.f64)
        assert _value_to_jsonable(ft, float("nan")) is None

    def test_float_inf(self):
        ft = extract_frac_type(t.f64)
        assert _value_to_jsonable(ft, float("inf")) is None

    def test_float_neg_inf(self):
        ft = extract_frac_type(t.f64)
        assert _value_to_jsonable(ft, float("-inf")) is None


# ── 3. Edge cases ────────────────────────────────────────────────────────────

class TestEdgeCases:
    """Edge cases: empty strings, unicode, zero values, max values."""

    def test_empty_string(self):
        obj = GSingleString(value="")
        assert json.loads(to_json(obj)) == {"value": ""}

    def test_unicode_string(self):
        obj = GSingleString(value="cafe\u0301 \u2615 \u65e5\u672c\u8a9e")
        rt = from_json(to_json(obj), GSingleString)
        assert rt.value == "cafe\u0301 \u2615 \u65e5\u672c\u8a9e"

    def test_zero_u32(self):
        obj = GSingleU32(value=0)
        assert json.loads(to_json(obj)) == {"value": 0}

    def test_max_u32(self):
        obj = GSingleU32(value=4294967295)
        assert json.loads(to_json(obj)) == {"value": 4294967295}

    def test_special_chars_in_string(self):
        obj = GSingleString(value='quote"backslash\\')
        s = to_json(obj)
        rt = from_json(s, GSingleString)
        assert rt.value == 'quote"backslash\\'

    def test_newlines_and_tabs(self):
        obj = GSingleString(value="tab\there\nnewline")
        s = to_json(obj)
        rt = from_json(s, GSingleString)
        assert rt.value == "tab\there\nnewline"


# ── 4. @schema integration ──────────────────────────────────────────────────

class TestSchemaIntegration:
    """@schema classes gain to_json() and from_json() methods."""

    def test_instance_to_json(self):
        obj = GInner(value=1, label="test")
        s = obj.to_json()
        assert json.loads(s) == {"value": 1, "label": "test"}

    def test_class_from_json(self):
        s = '{"value":1,"label":"test"}'
        obj = GInner.from_json(s)
        assert isinstance(obj, GInner)
        assert obj.value == 1
        assert obj.label == "test"

    def test_round_trip_via_methods(self):
        original = GOuter(
            inner=GInner(value=42, label="hello"),
            name="world",
        )
        s = original.to_json()
        restored = GOuter.from_json(s)
        assert restored.inner.value == 42
        assert restored.inner.label == "hello"
        assert restored.name == "world"

    def test_to_json_standalone_function(self):
        """to_json() as a standalone function also works."""
        obj = GInner(value=7, label="x")
        assert to_json(obj) == to_json(obj, obj.__schema__)

    def test_from_json_with_frac_type(self):
        """from_json() with a raw FracType returns a dict, not a class instance."""
        ft = extract_frac_type(t.vec[t.u32])
        result = from_json("[1,2,3]", ft)
        assert result == [1, 2, 3]

    def test_variant_via_schema(self):
        obj = GWithVariant(data={"type": "uint32", "value": 42})
        s = obj.to_json()
        assert json.loads(s) == {"data": {"uint32": 42}}
        restored = GWithVariant.from_json(s)
        assert restored.data == {"type": "uint32", "value": 42}

    def test_complex_schema(self):
        obj = GComplex(
            items=[GInner(value=1, label="a")],
            opt_vec=[10, 20],
            vec_opt=["x", None],
            opt_struct=GInner(value=99, label="p"),
        )
        s = obj.to_json()
        parsed = json.loads(s)
        assert parsed["items"] == [{"value": 1, "label": "a"}]
        assert parsed["opt_vec"] == [10, 20]
        assert parsed["vec_opt"] == ["x", None]
        assert parsed["opt_struct"] == {"value": 99, "label": "p"}
