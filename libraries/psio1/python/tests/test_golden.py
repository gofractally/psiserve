"""Golden test vectors — cross-language wire compatibility.

Loads ``test_vectors/vectors.json`` (the single source of truth shared
by C++, Rust, Go, Zig, JS, and MoonBit) and verifies that the Python
codec produces and consumes the exact same bytes.
"""

from __future__ import annotations

import json
import pathlib
from typing import Any

import pytest

from psio import schema, types as t
from psio.types import (
    FracType,
    IntType,
    FloatType,
    BoolType,
    StringType,
    BytesType,
    VecType,
    OptionalType,
    ArrayType,
    VariantType,
    StructType,
)

# ── Locate vectors.json ────────────────────────────────────────────────────

VECTORS_PATH = (
    pathlib.Path(__file__).resolve().parents[2] / "test_vectors" / "vectors.json"
)


def _load_vectors() -> list[dict]:
    with open(VECTORS_PATH) as f:
        data = json.load(f)
    assert data["format_version"] == 1
    return data["types"]


# ── Schema class definitions (one per golden type) ─────────────────────────
# The field names, types, and extensible/DWC flags MUST match the spec.

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


# ── Type name → class mapping ──────────────────────────────────────────────

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

# ── StructType → Python class registry (for nested struct construction) ────

_STRUCT_REG: dict[int, type] = {}
for _cls in TYPE_MAP.values():
    _STRUCT_REG[id(_cls.__schema__)] = _cls


# ── JSON → Python value conversion ────────────────────────────────────────

def _json_to_value(ft: FracType, val: Any) -> Any:
    """Convert a JSON value to the Python representation for *ft*."""
    if val is None:
        return None

    if isinstance(ft, BoolType):
        return bool(val)

    if isinstance(ft, IntType):
        return int(val)  # handles string u64/i64

    if isinstance(ft, FloatType):
        return float(val)

    if isinstance(ft, StringType):
        return str(val)

    if isinstance(ft, BytesType):
        if isinstance(val, list):
            return bytes(val)
        return val

    if isinstance(ft, VecType):
        return [_json_to_value(ft.element, elem) for elem in val]

    if isinstance(ft, OptionalType):
        if val is None:
            return None
        return _json_to_value(ft.inner, val)

    if isinstance(ft, ArrayType):
        return [_json_to_value(ft.element, elem) for elem in val]

    if isinstance(ft, VariantType):
        # JSON format: {"case_name": value}
        case_name = next(iter(val))
        case_val = val[case_name]
        case_ft = ft.cases.get(case_name)
        if case_ft is None:
            return {"type": case_name, "value": None}
        return {"type": case_name, "value": _json_to_value(case_ft, case_val)}

    if isinstance(ft, StructType):
        cls = _STRUCT_REG.get(id(ft))
        if cls is not None:
            return _json_to_instance(cls, val)
        return val

    raise TypeError(f"Cannot convert JSON for {ft!r}")


def _json_to_instance(cls: type, json_dict: dict) -> Any:
    """Construct a @schema class instance from a JSON dict."""
    st: StructType = cls.__schema__
    kwargs: dict[str, Any] = {}
    for fi in st.fields:
        raw = json_dict.get(fi.name)
        kwargs[fi.name] = _json_to_value(fi.frac_type, raw)
    return cls(**kwargs)


# ── Parametrize from vectors.json ──────────────────────────────────────────

def _collect_cases() -> list[tuple[str, str, type, dict]]:
    """Return (type_name, case_name, cls, case_dict) for pytest parametrize."""
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


@pytest.mark.parametrize(
    "type_name,case_name,cls,case",
    _GOLDEN_CASES,
    ids=[f"{t}/{c}" for t, c, _, _ in _GOLDEN_CASES],
)
class TestGoldenVectors:
    """Verify wire compatibility with the shared test vector suite."""

    def test_pack(self, type_name: str, case_name: str, cls: type, case: dict):
        """Construct from JSON, pack, compare hex to reference."""
        obj = _json_to_instance(cls, case["json"])
        actual_hex = obj.pack().hex().upper()
        expected_hex = case["packed_hex"]
        assert actual_hex == expected_hex, (
            f"{type_name}/{case_name}: pack mismatch\n"
            f"  expected: {expected_hex}\n"
            f"  actual:   {actual_hex}"
        )

    def test_unpack_repack(self, type_name: str, case_name: str, cls: type, case: dict):
        """Unpack reference bytes, re-pack, compare hex (round-trip)."""
        packed_bytes = bytes.fromhex(case["packed_hex"])
        unpacked = cls.unpack(packed_bytes)
        repacked_hex = unpacked.pack().hex().upper()
        expected_hex = case["packed_hex"]
        assert repacked_hex == expected_hex, (
            f"{type_name}/{case_name}: unpack→repack mismatch\n"
            f"  expected: {expected_hex}\n"
            f"  actual:   {repacked_hex}"
        )

    def test_view_fields(self, type_name: str, case_name: str, cls: type, case: dict):
        """View over reference bytes can read fields without crashing."""
        packed_bytes = bytes.fromhex(case["packed_hex"])
        if not hasattr(cls, "View"):
            pytest.skip("no View class")
        view = cls.view(packed_bytes)
        # Verify repr works (exercises all field readers)
        repr(view)
