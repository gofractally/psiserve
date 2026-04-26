"""Pure-Python fracpack binary encoder/decoder.

This module implements the fracpack wire format using only the stdlib.
When the Rust extension is available, its hot-path functions replace
the ones defined here.
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
    TupleType,
    VariantType,
    VecType,
)

# ── PackBuffer ───────────────────────────────────────────────────────────────

class PackBuffer:
    """Growable byte buffer with position tracking and offset patching."""

    __slots__ = ("_buf",)

    def __init__(self) -> None:
        self._buf = bytearray()

    @property
    def pos(self) -> int:
        return len(self._buf)

    def write(self, data: bytes | bytearray | memoryview) -> None:
        self._buf.extend(data)

    def write_u8(self, v: int) -> None:
        self._buf.append(v & 0xFF)

    def write_u16(self, v: int) -> None:
        self._buf.extend(_st.pack("<H", v))

    def write_u32(self, v: int) -> None:
        self._buf.extend(_st.pack("<I", v))

    def write_u64(self, v: int) -> None:
        self._buf.extend(_st.pack("<Q", v))

    def patch_u16(self, pos: int, v: int) -> None:
        _st.pack_into("<H", self._buf, pos, v)

    def patch_u32(self, pos: int, v: int) -> None:
        _st.pack_into("<I", self._buf, pos, v)

    def finish(self) -> bytes:
        return bytes(self._buf)


# ── Unpack cursor ────────────────────────────────────────────────────────────

class UnpackCursor:
    """Read cursor over a bytes-like buffer."""

    __slots__ = ("buf", "pos")

    def __init__(self, buf: bytes | memoryview, pos: int = 0) -> None:
        self.buf = buf if isinstance(buf, memoryview) else memoryview(buf)
        self.pos = pos

    def read_u8(self) -> int:
        v = self.buf[self.pos]
        self.pos += 1
        return v

    def read_u16(self) -> int:
        v = _st.unpack_from("<H", self.buf, self.pos)[0]
        self.pos += 2
        return v

    def read_u32(self) -> int:
        v = _st.unpack_from("<I", self.buf, self.pos)[0]
        self.pos += 4
        return v

    def read_u64(self) -> int:
        v = _st.unpack_from("<Q", self.buf, self.pos)[0]
        self.pos += 8
        return v

    def read_bytes(self, n: int) -> bytes:
        v = bytes(self.buf[self.pos : self.pos + n])
        self.pos += n
        return v

    def read_fmt(self, fmt: str, size: int) -> Any:
        v = _st.unpack_from(fmt, self.buf, self.pos)[0]
        self.pos += size
        return v


# ── Pack dispatch ────────────────────────────────────────────────────────────

def pack_value(ft: FracType, value: Any, w: PackBuffer) -> None:
    """Pack *value* as a standalone (top-level) object of type *ft*."""

    if isinstance(ft, BoolType):
        w.write_u8(1 if value else 0)

    elif isinstance(ft, IntType):
        w.write(_st.pack(ft.fmt, value))

    elif isinstance(ft, FloatType):
        w.write(_st.pack(ft.fmt, value))

    elif isinstance(ft, StringType):
        encoded = value.encode("utf-8") if isinstance(value, str) else value
        w.write_u32(len(encoded))
        w.write(encoded)

    elif isinstance(ft, BytesType):
        w.write_u32(len(value))
        w.write(value)

    elif isinstance(ft, VecType):
        _pack_vec(ft, value, w)

    elif isinstance(ft, OptionalType):
        _pack_optional_standalone(ft, value, w)

    elif isinstance(ft, ArrayType):
        _pack_array(ft, value, w)

    elif isinstance(ft, VariantType):
        _pack_variant(ft, value, w)

    elif isinstance(ft, TupleType):
        _pack_tuple(ft, value, w)

    elif isinstance(ft, StructType):
        _pack_struct(ft, value, w)

    else:
        raise TypeError(f"Cannot pack {ft!r}")


def _pack_vec(ft: VecType, value: list, w: PackBuffer) -> None:
    eft = ft.element
    if not eft.is_variable_size:
        # Fixed-size elements: data_size = len * element.fixed_size
        data_size = len(value) * eft.fixed_size
        w.write_u32(data_size)
        for item in value:
            pack_value(eft, item, w)
    else:
        # Variable-size elements: offsets in fixed region, data on heap
        data_size = len(value) * eft.fixed_size  # all offsets
        w.write_u32(data_size)
        fixed_start = w.pos
        # Write offset placeholders
        for item in value:
            _write_embedded_fixed(eft, item, w)
        # Write heap data and patch offsets
        fixed_pos = fixed_start
        for item in value:
            _write_embedded_heap(eft, item, fixed_pos, w)
            fixed_pos += eft.fixed_size


def _pack_array(ft: ArrayType, value: list, w: PackBuffer) -> None:
    if len(value) != ft.length:
        raise ValueError(
            f"Array expects {ft.length} elements, got {len(value)}"
        )
    eft = ft.element
    if not eft.is_variable_size:
        for item in value:
            pack_value(eft, item, w)
    else:
        fixed_start = w.pos
        for item in value:
            _write_embedded_fixed(eft, item, w)
        fixed_pos = fixed_start
        for item in value:
            _write_embedded_heap(eft, item, fixed_pos, w)
            fixed_pos += eft.fixed_size


def _pack_optional_standalone(ft: OptionalType, value: Any, w: PackBuffer) -> None:
    fixed_pos = w.pos
    if value is None:
        w.write_u32(1)  # None marker
    elif ft.inner.is_variable_size and ft.inner.is_empty(value):
        w.write_u32(0)  # empty container
    else:
        w.write_u32(0)  # placeholder
        heap_pos = w.pos
        pack_value(ft.inner, value, w)
        w.patch_u32(fixed_pos, heap_pos - fixed_pos)


def _pack_variant(ft: VariantType, value: dict, w: PackBuffer) -> None:
    if not isinstance(value, dict) or "type" not in value:
        raise TypeError("Variant values must be {'type': 'CaseName', 'value': ...}")
    case_name = value["type"]
    case_value = value.get("value")
    cases_list = list(ft.cases.items())
    tag = None
    case_ft = None
    for i, (name, cft) in enumerate(cases_list):
        if name == case_name:
            tag = i
            case_ft = cft
            break
    if tag is None:
        raise ValueError(f"Unknown variant case: {case_name!r}")
    w.write_u8(tag)
    if case_ft is None:
        w.write_u32(0)  # no data
    else:
        size_pos = w.pos
        w.write_u32(0)  # placeholder
        content_start = w.pos
        pack_value(case_ft, case_value, w)
        w.patch_u32(size_pos, w.pos - content_start)


def _pack_tuple(ft: TupleType, value: tuple | list, w: PackBuffer) -> None:
    # Tuples are always extensible: u16 fixed_size + fixed + heap
    fields_ft = ft.elements
    num_present = _count_present_tuple(fields_ft, value)

    members_fixed = sum(f.fixed_size for f in fields_ft[:num_present])
    w.write_u16(members_fixed)

    fixed_start = w.pos
    for i in range(num_present):
        _write_embedded_fixed(fields_ft[i], value[i], w)

    fixed_pos = fixed_start
    for i in range(num_present):
        _write_embedded_heap(fields_ft[i], value[i], fixed_pos, w)
        fixed_pos += fields_ft[i].fixed_size


def _pack_struct(ft: StructType, value: Any, w: PackBuffer) -> None:
    fields = ft.fields
    num_present = _count_present_struct(fields, value)

    members_fixed = sum(f.frac_type.fixed_size for f in fields[:num_present])

    if ft.extensible:
        w.write_u16(members_fixed)

    fixed_start = w.pos
    for i in range(num_present):
        f = fields[i]
        v = _get_field(value, f.name)
        _write_embedded_fixed(f.frac_type, v, w)

    fixed_pos = fixed_start
    for i in range(num_present):
        f = fields[i]
        v = _get_field(value, f.name)
        _write_embedded_heap(f.frac_type, v, fixed_pos, w)
        fixed_pos += f.frac_type.fixed_size


# ── Embedded pack helpers ────────────────────────────────────────────────────

def _write_embedded_fixed(ft: FracType, value: Any, w: PackBuffer) -> None:
    """Write the fixed-region contribution for a field."""
    if not ft.is_variable_size:
        pack_value(ft, value, w)
    elif isinstance(ft, OptionalType):
        if value is None:
            w.write_u32(1)  # None
        elif ft.inner.is_variable_size and not ft.inner.is_optional:
            # Reuse inner's offset slot
            _write_embedded_fixed(ft.inner, value, w)
        else:
            w.write_u32(1)  # placeholder (will be patched if value present)
    else:
        if ft.is_empty(value):
            w.write_u32(0)  # empty container
        else:
            w.write_u32(0)  # placeholder


def _write_embedded_heap(ft: FracType, value: Any, fixed_pos: int,
                         w: PackBuffer) -> None:
    """Write the heap-region contribution and patch the offset."""
    if not ft.is_variable_size:
        return  # nothing on heap

    if isinstance(ft, OptionalType):
        if value is None:
            return  # offset already 1
        if ft.inner.is_variable_size and not ft.inner.is_optional:
            # Offset was delegated to inner — delegate heap writing too
            if not ft.inner.is_empty(value):
                heap_pos = w.pos
                w.patch_u32(fixed_pos, heap_pos - fixed_pos)
                pack_value(ft.inner, value, w)
            # else: offset stays 0 (empty container), no heap data
            return
        # Inner is fixed-size or itself optional — we wrote 1 as placeholder
        if value is not None:
            heap_pos = w.pos
            w.patch_u32(fixed_pos, heap_pos - fixed_pos)
            pack_value(ft.inner, value, w)
        return

    if ft.is_empty(value):
        return  # offset already 0

    heap_pos = w.pos
    w.patch_u32(fixed_pos, heap_pos - fixed_pos)
    pack_value(ft, value, w)


# ── Trailing optional elision ────────────────────────────────────────────────

def _get_field(value: Any, name: str) -> Any:
    """Get a field from a dataclass instance or dict."""
    if isinstance(value, dict):
        return value.get(name)
    return getattr(value, name)


def _count_present_struct(fields: list[FieldInfo], value: Any) -> int:
    """Index past which all fields are optional-and-None."""
    num_present = 0
    for i, f in enumerate(fields):
        if not f.frac_type.is_optional:
            num_present = i + 1
        elif _get_field(value, f.name) is not None:
            num_present = i + 1
    return num_present


def _count_present_tuple(fields_ft: list[FracType], value: tuple | list) -> int:
    num_present = 0
    for i, ft in enumerate(fields_ft):
        if i >= len(value):
            break
        if not ft.is_optional:
            num_present = i + 1
        elif value[i] is not None:
            num_present = i + 1
    return num_present


# ── Unpack dispatch ──────────────────────────────────────────────────────────

def unpack_value(ft: FracType, c: UnpackCursor) -> Any:
    """Unpack one value of type *ft* from cursor *c*."""

    if isinstance(ft, BoolType):
        return c.read_u8() != 0

    if isinstance(ft, IntType):
        return c.read_fmt(ft.fmt, ft.fixed_size)

    if isinstance(ft, FloatType):
        return c.read_fmt(ft.fmt, ft.fixed_size)

    if isinstance(ft, StringType):
        length = c.read_u32()
        return c.read_bytes(length).decode("utf-8")

    if isinstance(ft, BytesType):
        length = c.read_u32()
        return c.read_bytes(length)

    if isinstance(ft, VecType):
        return _unpack_vec(ft, c)

    if isinstance(ft, OptionalType):
        return _unpack_optional_standalone(ft, c)

    if isinstance(ft, ArrayType):
        return _unpack_array(ft, c)

    if isinstance(ft, VariantType):
        return _unpack_variant(ft, c)

    if isinstance(ft, TupleType):
        return _unpack_tuple(ft, c)

    if isinstance(ft, StructType):
        return _unpack_struct_raw(ft, c)

    raise TypeError(f"Cannot unpack {ft!r}")


def _unpack_vec(ft: VecType, c: UnpackCursor) -> list:
    data_size = c.read_u32()
    eft = ft.element
    if not eft.is_variable_size:
        count = data_size // eft.fixed_size
        return [unpack_value(eft, c) for _ in range(count)]
    else:
        count = data_size // eft.fixed_size
        fixed_start = c.pos
        end_fixed = fixed_start + data_size
        result = []
        for _ in range(count):
            result.append(_read_embedded(eft, c, end_fixed))
        return result


def _unpack_array(ft: ArrayType, c: UnpackCursor) -> list:
    eft = ft.element
    if not eft.is_variable_size:
        return [unpack_value(eft, c) for _ in range(ft.length)]
    else:
        fixed_start = c.pos
        end_fixed = fixed_start + ft.length * eft.fixed_size
        return [_read_embedded(eft, c, end_fixed) for _ in range(ft.length)]


def _unpack_optional_standalone(ft: OptionalType, c: UnpackCursor) -> Any:
    start = c.pos
    offset = c.read_u32()
    if offset == 1:
        return None
    if offset == 0:
        if ft.inner.is_variable_size:
            # Empty container
            return _empty_value(ft.inner)
        return None  # shouldn't happen for fixed-size inner
    # Follow offset
    c.pos = start + offset
    return unpack_value(ft.inner, c)


def _unpack_variant(ft: VariantType, c: UnpackCursor) -> dict:
    tag = c.read_u8()
    data_size = c.read_u32()
    cases_list = list(ft.cases.items())
    if tag >= len(cases_list):
        raise ValueError(f"Variant tag {tag} out of range")
    case_name, case_ft = cases_list[tag]
    content_end = c.pos + data_size
    if case_ft is None:
        c.pos = content_end
        return {"type": case_name, "value": None}
    val = unpack_value(case_ft, c)
    c.pos = content_end
    return {"type": case_name, "value": val}


def _unpack_tuple(ft: TupleType, c: UnpackCursor) -> tuple:
    fixed_size = c.read_u16()
    fixed_start = c.pos
    end_fixed = fixed_start + fixed_size
    result = []
    for eft in ft.elements:
        if c.pos >= end_fixed and eft.is_optional:
            result.append(None)
        else:
            result.append(_read_embedded(eft, c, end_fixed))
    return tuple(result)


def _unpack_struct_raw(ft: StructType, c: UnpackCursor) -> dict:
    """Unpack struct fields into a dict (used when struct class is unknown)."""
    if ft.extensible:
        fixed_size = c.read_u16()
    else:
        fixed_size = ft.members_fixed_size
    fixed_start = c.pos
    end_fixed = fixed_start + fixed_size

    result = {}
    for f in ft.fields:
        if c.pos >= end_fixed and f.frac_type.is_optional:
            result[f.name] = None
        else:
            result[f.name] = _read_embedded(f.frac_type, c, end_fixed)
    return result


# ── Embedded unpack helpers ──────────────────────────────────────────────────

def _read_embedded(ft: FracType, c: UnpackCursor, end_fixed: int) -> Any:
    """Read a value from its embedding context (fixed region + heap)."""
    if not ft.is_variable_size:
        return unpack_value(ft, c)

    if isinstance(ft, OptionalType):
        return _read_embedded_optional(ft, c, end_fixed)

    # Variable-size: read offset
    start = c.pos
    offset = c.read_u32()
    if offset == 0:
        return _empty_value(ft)
    saved = c.pos
    c.pos = start + offset
    val = unpack_value(ft, c)
    c.pos = saved  # restore cursor to next field in fixed region
    return val


def _read_embedded_optional(ft: OptionalType, c: UnpackCursor,
                            end_fixed: int) -> Any:
    if ft.inner.is_variable_size and not ft.inner.is_optional:
        # Reuses inner's offset
        start = c.pos
        offset = c.read_u32()
        saved = c.pos
        if offset == 1:
            return None
        if offset == 0:
            return _empty_value(ft.inner)
        c.pos = start + offset
        val = unpack_value(ft.inner, c)
        c.pos = saved  # restore cursor to next field in fixed region
        return val
    else:
        start = c.pos
        offset = c.read_u32()
        saved = c.pos
        if offset == 1:
            return None
        c.pos = start + offset
        val = unpack_value(ft.inner, c)
        c.pos = saved  # restore cursor to next field in fixed region
        return val


def _empty_value(ft: FracType) -> Any:
    """Return the empty-container sentinel for a type."""
    if isinstance(ft, StringType):
        return ""
    if isinstance(ft, BytesType):
        return b""
    if isinstance(ft, VecType):
        return []
    if isinstance(ft, ArrayType):
        return []
    return None


# ── Public API ───────────────────────────────────────────────────────────────

def pack(ft: FracType, value: Any) -> bytes:
    """Encode *value* to fracpack bytes using type descriptor *ft*."""
    w = PackBuffer()
    pack_value(ft, value, w)
    return w.finish()


def unpack(ft: FracType, data: bytes | memoryview) -> Any:
    """Decode fracpack bytes into a Python value using type descriptor *ft*."""
    c = UnpackCursor(data)
    return unpack_value(ft, c)
