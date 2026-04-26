"""Canonical JSON encoding/decoding for fracpack types.

Python's json module handles encoding/decoding but doesn't know about fracpack
type semantics: u64/i64 overflow 53-bit JS-safe integers, bytes need hex encoding,
variants need single-key object wrapping. Without fracpack canonical JSON, users
must write manual encoders/decoders for each struct -- error-prone and inconsistent
across services. This module provides schema-driven JSON that matches the canonical
format used by all other fracpack implementations (C++, Rust, JS, Zig, Go, MoonBit).
"""

from __future__ import annotations

import json
import math
from typing import Any

from .types import (
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
)


# ── Value to JSON-able Python object ─────────────────────────────────────────

def _value_to_jsonable(ft: FracType, value: Any) -> Any:
    """Convert a Python value to a JSON-serialisable Python object.

    The result can be passed directly to ``json.dumps()``.
    """
    if isinstance(ft, BoolType):
        return bool(value)

    if isinstance(ft, IntType):
        if ft.bits == 64:
            return str(value)
        return int(value)

    if isinstance(ft, FloatType):
        if value is None or (isinstance(value, float) and (math.isnan(value) or math.isinf(value))):
            return None
        v = float(value)
        # Canonical format: whole-number floats serialize without ".0"
        # (e.g. 0.0 -> 0, 1.0 -> 1, -1.0 -> -1)
        if v.is_integer():
            return int(v)
        return v

    if isinstance(ft, StringType):
        return str(value)

    if isinstance(ft, BytesType):
        if isinstance(value, (bytes, bytearray, memoryview)):
            return bytes(value).hex()
        return value

    if isinstance(ft, OptionalType):
        if value is None:
            return None
        return _value_to_jsonable(ft.inner, value)

    if isinstance(ft, VecType):
        return [_value_to_jsonable(ft.element, item) for item in value]

    if isinstance(ft, ArrayType):
        return [_value_to_jsonable(ft.element, item) for item in value]

    if isinstance(ft, TupleType):
        return [_value_to_jsonable(ft.elements[i], value[i])
                for i in range(len(ft.elements))]

    if isinstance(ft, VariantType):
        # Internal representation: {"type": "CaseName", "value": ...}
        case_name = value["type"]
        case_value = value.get("value")
        case_ft = ft.cases.get(case_name)
        if case_ft is None:
            return {case_name: None}
        return {case_name: _value_to_jsonable(case_ft, case_value)}

    if isinstance(ft, StructType):
        result: dict[str, Any] = {}
        for fi in ft.fields:
            v = _get_field(value, fi.name)
            result[fi.name] = _value_to_jsonable(fi.frac_type, v)
        return result

    raise TypeError(f"Cannot convert to JSON: {ft!r}")


# ── JSON-able Python object to value ─────────────────────────────────────────

def _jsonable_to_value(ft: FracType, obj: Any) -> Any:
    """Convert a parsed JSON object to a fracpack-ready Python value."""
    if isinstance(ft, BoolType):
        return bool(obj)

    if isinstance(ft, IntType):
        if ft.bits == 64:
            return int(obj)  # handles string → int
        return int(obj) if isinstance(obj, str) else int(obj)

    if isinstance(ft, FloatType):
        if obj is None:
            return float('nan')
        return float(obj)

    if isinstance(ft, StringType):
        return str(obj)

    if isinstance(ft, BytesType):
        if isinstance(obj, str):
            return bytes.fromhex(obj)
        return bytes(obj)

    if isinstance(ft, OptionalType):
        if obj is None:
            return None
        return _jsonable_to_value(ft.inner, obj)

    if isinstance(ft, VecType):
        return [_jsonable_to_value(ft.element, item) for item in obj]

    if isinstance(ft, ArrayType):
        return [_jsonable_to_value(ft.element, item) for item in obj]

    if isinstance(ft, TupleType):
        return tuple(_jsonable_to_value(ft.elements[i], obj[i])
                     for i in range(len(ft.elements)))

    if isinstance(ft, VariantType):
        # JSON format: {"CaseName": value}
        if not isinstance(obj, dict) or len(obj) != 1:
            raise ValueError(
                f"Variant JSON must be a single-key object, got {type(obj).__name__}"
            )
        case_name = next(iter(obj))
        case_val = obj[case_name]
        if case_name not in ft.cases:
            raise ValueError(f"Unknown variant case: {case_name!r}")
        case_ft = ft.cases[case_name]
        if case_ft is None:
            return {"type": case_name, "value": None}
        return {"type": case_name, "value": _jsonable_to_value(case_ft, case_val)}

    if isinstance(ft, StructType):
        result: dict[str, Any] = {}
        for fi in ft.fields:
            raw = obj.get(fi.name) if isinstance(obj, dict) else getattr(obj, fi.name, None)
            if raw is None and fi.frac_type.is_optional:
                result[fi.name] = None
            elif raw is None and not fi.frac_type.is_optional:
                result[fi.name] = _jsonable_to_value(fi.frac_type, raw) if raw is not None else _default_value(fi.frac_type)
            else:
                result[fi.name] = _jsonable_to_value(fi.frac_type, raw)
        return result

    raise TypeError(f"Cannot convert from JSON: {ft!r}")


def _default_value(ft: FracType) -> Any:
    """Return a sensible default for missing non-optional fields."""
    if isinstance(ft, BoolType):
        return False
    if isinstance(ft, IntType):
        return 0
    if isinstance(ft, FloatType):
        return 0.0
    if isinstance(ft, StringType):
        return ""
    if isinstance(ft, BytesType):
        return b""
    if isinstance(ft, VecType):
        return []
    if isinstance(ft, ArrayType):
        return []
    return None


# ── Field access helper ──────────────────────────────────────────────────────

def _get_field(value: Any, name: str) -> Any:
    """Get a field from a dataclass instance or dict."""
    if isinstance(value, dict):
        return value.get(name)
    return getattr(value, name, None)


# ── Public API ───────────────────────────────────────────────────────────────

def to_json(value: Any, frac_type: FracType | None = None) -> str:
    """Convert a Python value to a canonical JSON string.

    If *value* has ``__schema__`` (i.e. a ``@schema``-decorated instance),
    its schema is used automatically and *frac_type* can be omitted.

    Returns a JSON string (not a dict).
    """
    if frac_type is None:
        ft = getattr(value, "__schema__", None)
        if ft is None:
            ft = getattr(type(value), "__schema__", None)
        if ft is None:
            raise TypeError(
                "Cannot infer schema; pass frac_type= or use a @schema instance"
            )
        frac_type = ft

    jsonable = _value_to_jsonable(frac_type, value)
    return json.dumps(jsonable, separators=(",", ":"), allow_nan=False)


def from_json(json_str: str, frac_type_or_cls: Any) -> Any:
    """Parse a canonical JSON string into a Python value.

    *frac_type_or_cls* can be:
    - A ``FracType`` instance -- returns a plain Python value (dict, list, etc.)
    - A ``@schema``-decorated class -- constructs and returns a class instance
    """
    parsed = json.loads(json_str)

    # Determine FracType
    if isinstance(frac_type_or_cls, FracType):
        return _jsonable_to_value(frac_type_or_cls, parsed)

    # Must be a @schema class
    ft = getattr(frac_type_or_cls, "__frac_type__", None)
    if ft is None:
        raise TypeError(
            f"{frac_type_or_cls!r} is not a FracType or @schema class"
        )
    raw = _jsonable_to_value(ft, parsed)
    # raw is a dict; construct the class instance
    _reconstruct_nested(ft, frac_type_or_cls, raw)
    return frac_type_or_cls(**raw)


def _reconstruct_nested(st: StructType, cls: Any, raw: dict) -> None:
    """Recursively construct @schema class instances from parsed dicts."""
    for fi in st.fields:
        val = raw.get(fi.name)
        if val is not None and isinstance(val, dict) and isinstance(fi.frac_type, StructType):
            nested_cls = fi.python_type
            if hasattr(nested_cls, "__schema__"):
                _reconstruct_nested(fi.frac_type, nested_cls, val)
                raw[fi.name] = nested_cls(**val)
