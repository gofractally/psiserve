"""Zero-copy view generation for fracpack structs.

A view wraps a ``memoryview`` over packed data and decodes fields lazily
on attribute access.  For partial reads of large structs this is
significantly faster than full ``unpack()``.

Performance tiers (pure Python, per scalar field access):

* ``struct.unpack_from`` at a precomputed offset: ~200-400 ns
* Full ``unpack()`` of a 10-field struct: ~3-5 us
* Speedup for accessing 1 of 10 fields: ~10x

With the Rust extension, field access drops to ~100 ns.
"""

from __future__ import annotations

import struct as _st
from typing import Any

from .types import (
    ArrayType,
    BoolType,
    BytesType,
    FieldInfo,
    FloatType,
    FracType,
    IntType,
    OptionalType,
    StringType,
    StructType,
    VecType,
    VariantType,
)

# ── Scalar format strings (precompiled struct.Struct objects) ─────────────────

_STRUCT_CACHE: dict[str, _st.Struct] = {}


def _get_struct(fmt: str) -> _st.Struct:
    s = _STRUCT_CACHE.get(fmt)
    if s is None:
        s = _st.Struct(fmt)
        _STRUCT_CACHE[fmt] = s
    return s


# ── Field reader functions ───────────────────────────────────────────────────
# Each returns a callable(buf, base) -> value, where base is the absolute
# position of the struct's fixed region start.

def _make_scalar_reader(offset: int, fmt: str, size: int):
    """Read a fixed-size scalar at a compile-time-known offset."""
    s = _get_struct(fmt)
    def read(buf: memoryview, base: int) -> Any:
        return s.unpack_from(buf, base + offset)[0]
    return read


def _make_bool_reader(offset: int):
    def read(buf: memoryview, base: int) -> bool:
        return buf[base + offset] != 0
    return read


def _make_string_reader(offset: int):
    """Read a string via offset → u32 length → bytes → decode."""
    def read(buf: memoryview, base: int) -> str:
        pos = base + offset
        off = _st.unpack_from("<I", buf, pos)[0]
        if off == 0:
            return ""
        target = pos + off
        length = _st.unpack_from("<I", buf, target)[0]
        return bytes(buf[target + 4 : target + 4 + length]).decode("utf-8")
    return read


def _make_raw_string_reader(offset: int):
    """Read a string field as a zero-copy memoryview slice (no decode)."""
    def read(buf: memoryview, base: int) -> memoryview:
        pos = base + offset
        off = _st.unpack_from("<I", buf, pos)[0]
        if off == 0:
            return buf[0:0]  # empty memoryview
        target = pos + off
        length = _st.unpack_from("<I", buf, target)[0]
        return buf[target + 4 : target + 4 + length]
    return read


def _make_bytes_reader(offset: int):
    def read(buf: memoryview, base: int) -> bytes:
        pos = base + offset
        off = _st.unpack_from("<I", buf, pos)[0]
        if off == 0:
            return b""
        target = pos + off
        length = _st.unpack_from("<I", buf, target)[0]
        return bytes(buf[target + 4 : target + 4 + length])
    return read


def _make_vec_reader(offset: int, element: FracType):
    """Read a vec via offset → data_size → elements."""
    if not element.is_variable_size:
        elem_size = element.fixed_size
        if isinstance(element, IntType) or isinstance(element, FloatType):
            # Fast path: return memoryview.cast for numeric vecs
            cast_fmt = element.fmt[-1]  # strip '<'
            def read_numeric_vec(buf: memoryview, base: int) -> Any:
                pos = base + offset
                off = _st.unpack_from("<I", buf, pos)[0]
                if off == 0:
                    return []
                target = pos + off
                data_size = _st.unpack_from("<I", buf, target)[0]
                raw = buf[target + 4 : target + 4 + data_size]
                try:
                    return raw.cast(cast_fmt)
                except (TypeError, ValueError):
                    # Fallback if cast doesn't work (e.g., non-contiguous)
                    count = data_size // elem_size
                    s = _get_struct(f"<{count}{element.fmt[-1]}")
                    return list(s.unpack_from(buf, target + 4))
            return read_numeric_vec

        # Non-numeric fixed-size elements: decode one by one
        def read_fixed_vec(buf: memoryview, base: int) -> list:
            pos = base + offset
            off = _st.unpack_from("<I", buf, pos)[0]
            if off == 0:
                return []
            target = pos + off
            data_size = _st.unpack_from("<I", buf, target)[0]
            count = data_size // elem_size
            result = []
            p = target + 4
            for _ in range(count):
                result.append(_read_value_at(element, buf, p))
                p += elem_size
            return result
        return read_fixed_vec

    # Variable-size elements
    def read_var_vec(buf: memoryview, base: int) -> list:
        pos = base + offset
        off = _st.unpack_from("<I", buf, pos)[0]
        if off == 0:
            return []
        target = pos + off
        data_size = _st.unpack_from("<I", buf, target)[0]
        count = data_size // element.fixed_size
        result = []
        fixed_pos = target + 4
        for _ in range(count):
            result.append(_read_embedded_view(element, buf, fixed_pos))
            fixed_pos += element.fixed_size
        return result
    return read_var_vec


def _make_optional_has_value_reader(offset: int):
    """Check if an optional field has a value without materializing the inner."""
    def read(buf: memoryview, base: int) -> bool:
        pos = base + offset
        off = _st.unpack_from("<I", buf, pos)[0]
        # offset==1 means None/empty; offset==0 means empty-default; >=4 means present
        return off != 1
    return read


def _make_vec_len_reader(offset: int, element: FracType):
    """Return the element count of a vec without materializing any elements."""
    elem_size = element.fixed_size
    def read(buf: memoryview, base: int) -> int:
        pos = base + offset
        off = _st.unpack_from("<I", buf, pos)[0]
        if off == 0:
            return 0
        target = pos + off
        data_size = _st.unpack_from("<I", buf, target)[0]
        return data_size // elem_size
    return read


def _make_optional_reader(offset: int, inner: FracType):
    """Read an optional via its offset encoding."""
    if inner.is_variable_size and not inner.is_optional:
        # Reuses inner's offset
        inner_reader = _make_reader_for_type(offset, inner)
        def read_opt_reuse(buf: memoryview, base: int) -> Any:
            pos = base + offset
            off = _st.unpack_from("<I", buf, pos)[0]
            if off == 1:
                return None
            return inner_reader(buf, base)
        return read_opt_reuse

    def read_opt(buf: memoryview, base: int) -> Any:
        pos = base + offset
        off = _st.unpack_from("<I", buf, pos)[0]
        if off == 1:
            return None
        if off == 0:
            return _empty_for(inner)
        target = pos + off
        return _read_value_at(inner, buf, target)
    return read_opt


def _make_struct_reader(offset: int, inner_struct: StructType):
    """Read a nested struct (always variable-size when extensible)."""
    def read_struct(buf: memoryview, base: int) -> Any:
        pos = base + offset
        if inner_struct.is_variable_size:
            off = _st.unpack_from("<I", buf, pos)[0]
            target = pos + off
        else:
            target = pos
        # Return a nested view
        view_cls = _view_class_cache.get(id(inner_struct))
        if view_cls is not None:
            return view_cls(buf, target)
        # Fallback: full decode
        from ._codec import UnpackCursor, unpack_value
        c = UnpackCursor(buf, target)
        return unpack_value(inner_struct, c)
    return read_struct


# ── Generic reader dispatch ──────────────────────────────────────────────────

def _make_reader_for_type(offset: int, ft: FracType):
    """Create an optimised reader callable for field type *ft* at *offset*."""

    if isinstance(ft, BoolType):
        return _make_bool_reader(offset)

    if isinstance(ft, IntType):
        return _make_scalar_reader(offset, ft.fmt, ft.fixed_size)

    if isinstance(ft, FloatType):
        return _make_scalar_reader(offset, ft.fmt, ft.fixed_size)

    if isinstance(ft, StringType):
        return _make_string_reader(offset)

    if isinstance(ft, BytesType):
        return _make_bytes_reader(offset)

    if isinstance(ft, VecType):
        return _make_vec_reader(offset, ft.element)

    if isinstance(ft, OptionalType):
        return _make_optional_reader(offset, ft.inner)

    if isinstance(ft, StructType):
        return _make_struct_reader(offset, ft)

    # Fallback: use the full codec
    def read_fallback(buf: memoryview, base: int) -> Any:
        from ._codec import UnpackCursor, unpack_value
        pos = base + offset
        if ft.is_variable_size:
            off = _st.unpack_from("<I", buf, pos)[0]
            pos = pos + off
        c = UnpackCursor(buf, pos)
        return unpack_value(ft, c)
    return read_fallback


# ── Helpers ──────────────────────────────────────────────────────────────────

def _read_value_at(ft: FracType, buf: memoryview, pos: int) -> Any:
    """Read a standalone value at absolute position *pos*."""
    if isinstance(ft, BoolType):
        return buf[pos] != 0
    if isinstance(ft, (IntType, FloatType)):
        return _st.unpack_from(ft.fmt, buf, pos)[0]
    if isinstance(ft, StringType):
        length = _st.unpack_from("<I", buf, pos)[0]
        return bytes(buf[pos + 4 : pos + 4 + length]).decode("utf-8")
    if isinstance(ft, BytesType):
        length = _st.unpack_from("<I", buf, pos)[0]
        return bytes(buf[pos + 4 : pos + 4 + length])
    # Complex types: fall back to codec
    from ._codec import UnpackCursor, unpack_value
    c = UnpackCursor(buf, pos)
    return unpack_value(ft, c)


def _read_embedded_view(ft: FracType, buf: memoryview, fixed_pos: int) -> Any:
    """Read a variable-size value from its embedding offset."""
    off = _st.unpack_from("<I", buf, fixed_pos)[0]
    if off == 0:
        return _empty_for(ft)
    if isinstance(ft, OptionalType) and off == 1:
        return None
    target = fixed_pos + off
    return _read_value_at(ft if not isinstance(ft, OptionalType) else ft.inner,
                          buf, target)


def _empty_for(ft: FracType) -> Any:
    if isinstance(ft, StringType):
        return ""
    if isinstance(ft, BytesType):
        return b""
    if isinstance(ft, VecType):
        return []
    return None


# ── Trailing optional elision guard ─────────────────────────────────────

def _make_elision_checked_reader(inner_reader, field_offset: int, ft: FracType):
    """Wrap a reader with a bounds check against the extensible struct header.

    If the field's offset is past the declared fixed_size, the field was
    elided (trailing optional) and returns None.
    """
    def read(buf: memoryview, base: int) -> Any:
        fixed_size = _st.unpack_from("<H", buf, base)[0]
        if field_offset >= fixed_size:
            return None
        return inner_reader(buf, base)
    return read


# ── View class cache (for nested struct views) ───────────────────────────────

_view_class_cache: dict[int, type] = {}


# ── View class factory ───────────────────────────────────────────────────────

def make_view_class(class_name: str, struct_type: StructType) -> type:
    """Create a view class with precomputed per-field reader descriptors."""

    # Build reader table: field_name → callable(buf, base)
    readers: dict[str, Any] = {}
    header_size = 2 if struct_type.extensible else 0

    for field in struct_type.fields:
        # Offset within fixed region, after the u16 header (if extensible)
        abs_offset = header_size + field.fixed_offset
        reader = _make_reader_for_type(abs_offset, field.frac_type)
        # Wrap optional fields with bounds check for trailing elision
        if struct_type.extensible and field.frac_type.is_optional:
            reader = _make_elision_checked_reader(
                reader, field.fixed_offset, field.frac_type,
            )
        readers[field.name] = reader

        # Zero-copy accessors for string, optional, and vec fields
        ft = field.frac_type
        if isinstance(ft, StringType):
            readers[field.name + "_raw"] = _make_raw_string_reader(abs_offset)
        if isinstance(ft, OptionalType):
            has_reader = _make_optional_has_value_reader(abs_offset)
            if struct_type.extensible:
                has_reader = _make_elision_checked_reader(
                    has_reader, field.fixed_offset, ft,
                )
            readers["has_" + field.name] = has_reader
        if isinstance(ft, VecType):
            readers[field.name + "_len"] = _make_vec_len_reader(
                abs_offset, ft.element,
            )

    # Names of actual struct fields (excludes synthetic _raw/has_/_len accessors)
    field_names = tuple(f.name for f in struct_type.fields)

    class _ViewMeta(type):
        """Metaclass that makes View classes have nice repr."""
        def __repr__(cls) -> str:
            return f"<{class_name}.View>"

    class ViewBase(metaclass=_ViewMeta):
        __slots__ = ("_buf", "_base")

        def __init__(self, buf: memoryview, base: int) -> None:
            object.__setattr__(self, "_buf", buf)
            object.__setattr__(self, "_base", base)

        def __getattr__(self, name: str) -> Any:
            reader = readers.get(name)
            if reader is None:
                raise AttributeError(
                    f"'{class_name}.View' has no field '{name}'"
                )
            buf = object.__getattribute__(self, "_buf")
            base = object.__getattribute__(self, "_base")
            return reader(buf, base)

        def __setattr__(self, name: str, value: Any) -> None:
            raise AttributeError("View objects are read-only")

        def __repr__(self) -> str:
            parts = []
            buf = object.__getattribute__(self, "_buf")
            base = object.__getattribute__(self, "_base")
            for name in field_names:
                try:
                    val = readers[name](buf, base)
                    parts.append(f"{name}={val!r}")
                except Exception:
                    parts.append(f"{name}=<error>")
            return f"{class_name}.View({', '.join(parts)})"

        def unpack(self) -> Any:
            """Materialise the full object from the view."""
            buf = object.__getattribute__(self, "_buf")
            base = object.__getattribute__(self, "_base")
            values = {}
            for name in field_names:
                values[name] = readers[name](buf, base)
            return values

        def __eq__(self, other: Any) -> bool:
            if isinstance(other, ViewBase):
                buf1 = object.__getattribute__(self, "_buf")
                base1 = object.__getattribute__(self, "_base")
                buf2 = object.__getattribute__(other, "_buf")
                base2 = object.__getattribute__(other, "_base")
                for name in field_names:
                    if readers[name](buf1, base1) != readers[name](buf2, base2):
                        return False
                return True
            return NotImplemented

    ViewBase.__name__ = f"{class_name}View"
    ViewBase.__qualname__ = f"{class_name}.View"

    # Register in cache for nested view lookups
    _view_class_cache[id(struct_type)] = ViewBase

    return ViewBase
