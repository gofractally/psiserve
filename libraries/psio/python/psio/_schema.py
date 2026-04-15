"""The ``@schema`` decorator — turns a class into a fracpack-serialisable type.

Usage::

    from psio import schema, types as t

    @schema
    class Person:
        name: t.string
        age: t.u32
        active: t.bool_
        tags: t.vec[t.string]
        bio: t.optional[t.string]

    alice = Person(name="Alice", age=30, active=True, tags=["eng"], bio=None)
    data = alice.pack()
    alice2 = Person.unpack(data)
    view = Person.view(data)
    print(view.name)
"""

from __future__ import annotations

import dataclasses
from typing import Any, ClassVar, get_type_hints

from . import _codec
from . import _json
from ._mutation import make_mut_view_class
from ._view import make_view_class
from .types import (
    FieldInfo,
    FracType,
    StructType,
    extract_frac_type,
    extract_python_type,
)


def _reconstruct_nested(st: StructType, raw: dict) -> None:
    """Recursively construct @schema class instances from unpacked dicts."""
    for fi in st.fields:
        val = raw.get(fi.name)
        if val is not None and isinstance(val, dict) and isinstance(fi.frac_type, StructType):
            nested_cls = fi.python_type
            if hasattr(nested_cls, "__schema__"):
                _reconstruct_nested(fi.frac_type, val)
                raw[fi.name] = nested_cls(**val)


def schema(cls: type | None = None, /) -> Any:
    """Class decorator that makes *cls* fracpack-serialisable.

    Adds ``pack()``, ``unpack()``, ``view()``, ``__schema__``, plus
    dataclass-style ``__init__``/``__eq__``/``__repr__``.
    """
    if cls is None:
        # Called as @schema() with parens — return the real decorator
        return schema

    # ── 1. Extract field metadata from annotations ───────────────────────

    # get_type_hints with include_extras preserves Annotated metadata
    hints = get_type_hints(cls, include_extras=True)

    # Detect Meta inner class
    meta = getattr(cls, "Meta", None)
    extensible = True
    if meta is not None:
        extensible = not getattr(meta, "fixed", False)

    # Filter out ClassVar, Meta, and non-annotated attributes
    field_infos: list[FieldInfo] = []
    offset = 0
    for i, (name, ann) in enumerate(hints.items()):
        ft = extract_frac_type(ann)
        if ft is None:
            continue  # skip non-psio annotations
        py_type = extract_python_type(ann)
        fi = FieldInfo(
            name=name,
            frac_type=ft,
            python_type=py_type,
            fixed_offset=offset,
            index=i,
        )
        field_infos.append(fi)
        offset += ft.fixed_size

    if not field_infos:
        raise TypeError(f"@schema class {cls.__name__} has no psio-typed fields")

    struct_type = StructType(field_infos, extensible=extensible)

    # ── 2. Apply @dataclass for __init__, __eq__, __repr__ ───────────────

    # Build a clean annotations dict with just the Python types
    # so that dataclasses generates the right __init__ signature.
    clean_annotations: dict[str, Any] = {}
    for fi in field_infos:
        clean_annotations[fi.name] = fi.python_type

    # Temporarily replace annotations for dataclass processing
    original_annotations = cls.__annotations__.copy()
    cls.__annotations__ = clean_annotations

    # Preserve any defaults defined on the class
    cls = dataclasses.dataclass(eq=True, repr=True, slots=True)(cls)

    # Restore the original annotations (with Annotated metadata)
    cls.__annotations__ = original_annotations

    # ── 3. Attach schema metadata ────────────────────────────────────────

    cls.__schema__: StructType = struct_type  # type: ignore[attr-defined]
    cls.__frac_type__: StructType = struct_type  # type: ignore[attr-defined]

    # ── 4. Add pack() instance method ────────────────────────────────────

    def _pack(self: Any) -> bytes:
        w = _codec.PackBuffer()
        _codec.pack_value(struct_type, self, w)
        return w.finish()

    cls.pack = _pack

    # ── 5. Add unpack() class method ─────────────────────────────────────

    @classmethod  # type: ignore[misc]
    def _unpack(klass: type, data: bytes | memoryview) -> Any:
        c = _codec.UnpackCursor(data)
        raw = _codec.unpack_value(struct_type, c)
        # Recursively construct nested @schema classes
        _reconstruct_nested(struct_type, raw)
        return klass(**raw)

    cls.unpack = _unpack

    # ── 6. Add view() class method ───────────────────────────────────────

    view_cls = make_view_class(cls.__name__, struct_type)
    cls.View = view_cls

    @classmethod  # type: ignore[misc]
    def _view(klass: type, data: bytes | memoryview) -> Any:
        buf = memoryview(data) if isinstance(data, bytes) else data
        return view_cls(buf, 0)

    cls.view = _view

    # ── 7. Add mut_view() class method ────────────────────────────────────

    mut_view_cls = make_mut_view_class(cls.__name__, struct_type)
    cls.MutView = mut_view_cls

    @classmethod  # type: ignore[misc]
    def _mut_view(klass: type, data: bytes | bytearray,
                  fast: bool = False) -> Any:
        buf = bytearray(data) if isinstance(data, bytes) else data
        return mut_view_cls(buf, 0, fast=fast)

    cls.mut_view = _mut_view

    # ── 8. Add read_field() class method ─────────────────────────────────

    @classmethod  # type: ignore[misc]
    def _read_field(klass: type, data: bytes | memoryview, name: str) -> Any:
        v = klass.view(data)
        return getattr(v, name)

    cls.read_field = _read_field

    # ── 9. Add validate() class method ───────────────────────────────────

    @classmethod  # type: ignore[misc]
    def _validate(klass: type, data: bytes | memoryview):
        from ._validate import validate
        return validate(struct_type, data)

    cls.validate = _validate

    # ── 10. Add to_json() instance method ────────────────────────────────

    def _to_json(self: Any) -> str:
        return _json.to_json(self, struct_type)

    cls.to_json = _to_json

    # ── 11. Add from_json() class method ─────────────────────────────────

    @classmethod  # type: ignore[misc]
    def _from_json(klass: type, json_str: str) -> Any:
        return _json.from_json(json_str, klass)

    cls.from_json = _from_json

    return cls
