"""In-place mutation system for fracpack packed data.

Provides ``MutView`` classes that allow modifying individual fields of packed
fracpack data without a full repack.  Two modes are available:

**Canonical mode** (default):
    Mutations splice the buffer in place, shifting all subsequent bytes and
    patching self-relative offsets of sibling variable-size fields.  The
    resulting buffer is always valid canonical fracpack.

**Fast mode** (``fast=True``):
    Mutations that fit in the old slot overwrite in place.  Mutations that
    grow append to the end of the buffer and update the offset.  Setting a
    field to ``None`` sets the offset marker to 1 without reclaiming space.
    Call ``compact()`` to produce a canonical buffer.

Usage::

    from psio._mutation import make_mut_view_class

    # Typically called by the @schema decorator:
    MutPerson = make_mut_view_class("Person", Person.__schema__)

    mv = MutPerson(bytearray(packed_data), base=0, fast=False)
    mv.name = "Bob"
    result = mv.to_bytes()
"""

from __future__ import annotations

import struct as _st
from typing import Any

from ._codec import PackBuffer, UnpackCursor, pack_value, unpack_value
from ._view import _make_reader_for_type, _make_elision_checked_reader
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


# ── Buffer primitives ──────────────────────────────────────────────────────

def _read_u16(data: bytearray, pos: int) -> int:
    return _st.unpack_from("<H", data, pos)[0]


def _read_u32(data: bytearray, pos: int) -> int:
    return _st.unpack_from("<I", data, pos)[0]


def _write_u16(data: bytearray, pos: int, val: int) -> None:
    _st.pack_into("<H", data, pos, val)


def _write_u32(data: bytearray, pos: int, val: int) -> None:
    _st.pack_into("<I", data, pos, val)


# ── Splice and offset patching ─────────────────────────────────────────────

def splice_buffer(data: bytearray, pos: int, old_len: int,
                  new_bytes: bytes | bytearray) -> int:
    """Replace ``data[pos:pos+old_len]`` with *new_bytes*, shifting the tail.

    Returns the delta (positive = grew, negative = shrank, 0 = same size).
    """
    new_len = len(new_bytes)
    delta = new_len - old_len
    if delta == 0:
        data[pos:pos + old_len] = new_bytes
    elif delta > 0:
        # Grow: insert extra bytes
        data[pos:pos + old_len] = new_bytes
    else:
        # Shrink
        data[pos:pos + old_len] = new_bytes
    return delta


def patch_offset(data: bytearray, offset_pos: int, splice_pos: int,
                 delta: int) -> None:
    """Adjust a self-relative u32 offset at *offset_pos* after a splice.

    Only adjusts if the offset is a valid pointer (>= 4) and the target
    it points to is at or past *splice_pos*.
    """
    offset_val = _read_u32(data, offset_pos)
    if offset_val <= 1:
        # 0 = empty container, 1 = None marker — never patch
        return
    target = offset_pos + offset_val
    if target >= splice_pos:
        new_val = offset_val + delta
        _write_u32(data, offset_pos, new_val)


# ── Measure packed size at a position ──────────────────────────────────────

def _packed_size_at(ft: FracType, data: bytearray, pos: int) -> int:
    """Return the number of heap bytes consumed by a standalone value at *pos*.

    For variable-size types this is the complete serialised size starting at
    *pos* (e.g. length prefix + payload for strings).
    """
    if isinstance(ft, BoolType):
        return 1

    if isinstance(ft, (IntType, FloatType)):
        return ft.fixed_size

    if isinstance(ft, StringType):
        length = _read_u32(data, pos)
        return 4 + length

    if isinstance(ft, BytesType):
        length = _read_u32(data, pos)
        return 4 + length

    if isinstance(ft, VecType):
        data_size = _read_u32(data, pos)
        if not ft.element.is_variable_size:
            return 4 + data_size
        # Variable-size elements: walk each element's heap data
        count = data_size // ft.element.fixed_size
        total = 4 + data_size  # length prefix + fixed region
        fixed_pos = pos + 4
        for i in range(count):
            elem_offset_pos = fixed_pos + i * ft.element.fixed_size
            elem_offset = _read_u32(data, elem_offset_pos)
            if elem_offset > 1:
                elem_target = elem_offset_pos + elem_offset
                elem_size = _packed_size_at(ft.element, data, elem_target)
                end = elem_target + elem_size
                if end - pos > total:
                    total = end - pos
        return total

    if isinstance(ft, OptionalType):
        # When measuring on heap, the optional's inner value is stored
        # directly (the offset indirection was in the fixed region).
        return _packed_size_at(ft.inner, data, pos)

    if isinstance(ft, StructType):
        if ft.extensible:
            fixed_size = _read_u16(data, pos)
            fixed_start = pos + 2
        else:
            fixed_size = ft.members_fixed_size
            fixed_start = pos
        end = fixed_start + fixed_size
        # Walk variable-size fields to find the extent of heap data
        field_pos = fixed_start
        for f in ft.fields:
            if field_pos >= fixed_start + fixed_size:
                break
            if f.frac_type.is_variable_size:
                off_val = _read_u32(data, field_pos)
                if off_val > 1:
                    target = field_pos + off_val
                    inner_size = _packed_size_at(f.frac_type, data, target)
                    candidate = target + inner_size
                    if candidate > end:
                        end = candidate
            field_pos += f.frac_type.fixed_size
        return end - pos

    if isinstance(ft, VariantType):
        # tag(1) + data_size(4) + data
        data_size = _read_u32(data, pos + 1)
        return 1 + 4 + data_size

    # Fallback: pack the unpacked value and measure
    c = UnpackCursor(data, pos)
    val = unpack_value(ft, c)
    return c.pos - pos


# ── Pack a value into standalone bytes ─────────────────────────────────────

def _pack_standalone(ft: FracType, value: Any) -> bytes:
    """Pack *value* as a standalone blob using *ft*."""
    w = PackBuffer()
    pack_value(ft, value, w)
    return w.finish()


# ── Effective FracType for offset purposes ─────────────────────────────────

def _effective_offset_type(ft: FracType) -> FracType:
    """For optional wrapping a variable-size inner, the offset slot is
    shared with the inner type. Return the type whose offset semantics
    apply."""
    if isinstance(ft, OptionalType) and ft.inner.is_variable_size and not ft.inner.is_optional:
        return ft.inner
    return ft


# ── Writer factories ───────────────────────────────────────────────────────

def _make_scalar_writer(abs_offset: int, fmt: str, size: int):
    """Writer for a fixed-size scalar field."""
    def write(mv: Any, value: Any) -> None:
        _st.pack_into(fmt, mv._data, mv._base + abs_offset, value)
    return write


def _make_bool_writer(abs_offset: int):
    def write(mv: Any, value: Any) -> None:
        mv._data[mv._base + abs_offset] = 1 if value else 0
    return write


def _is_field_elided(data: bytearray, base: int, header_size: int,
                     field: FieldInfo) -> bool:
    """Check whether a field was elided due to trailing optional elision.

    Only applies to extensible structs (header_size == 2).  Returns True
    if the field's fixed offset is beyond the declared fixed region size.
    """
    if header_size != 2:
        return False
    declared_fixed = _read_u16(data, base)
    return field.fixed_offset >= declared_fixed


def _expand_fixed_region(data: bytearray, base: int, header_size: int,
                         field: FieldInfo, all_fields: list[FieldInfo]) -> int:
    """Expand the extensible struct's fixed region to include *field*.

    Inserts zero-initialised offset slots for all fields between the
    current end of the fixed region and *field* (inclusive).  Updates
    the u16 fixed_size header.  Returns the byte delta of the expansion.

    All inserted optional offsets are set to 1 (None marker).
    """
    declared_fixed = _read_u16(data, base)
    needed_fixed = field.fixed_offset + field.frac_type.fixed_size

    if needed_fixed <= declared_fixed:
        return 0  # already large enough

    # Bytes to insert at the end of the current fixed region
    insert_pos = base + header_size + declared_fixed
    insert_size = needed_fixed - declared_fixed

    # Build the inserted bytes: 4-byte slots initialised to 1 (None)
    # for optional fields, 0 for others
    insert_bytes = bytearray(insert_size)
    for f in all_fields:
        if f.fixed_offset >= declared_fixed and f.fixed_offset < needed_fixed:
            slot_start = f.fixed_offset - declared_fixed
            if f.frac_type.is_optional:
                _st.pack_into("<I", insert_bytes, slot_start, 1)

    delta = splice_buffer(data, insert_pos, 0, bytes(insert_bytes))

    # Update the header
    _write_u16(data, base, needed_fixed)

    # Patch existing sibling offsets that point past the insert point
    for f in all_fields:
        if f.fixed_offset >= declared_fixed:
            continue  # This field was just inserted
        if not f.frac_type.is_variable_size:
            continue
        sibling_offset_pos = base + header_size + f.fixed_offset
        patch_offset(data, sibling_offset_pos, insert_pos, delta)

    return delta


def _make_variable_writer_canonical(field: FieldInfo, header_size: int,
                                    all_fields: list[FieldInfo],
                                    struct_type: StructType):
    """Writer for a variable-size field in canonical mode.

    Steps:
    1. If the field was elided (trailing optional), expand the fixed region.
    2. Locate old data via the self-relative offset.
    3. Measure old data size.
    4. Pack new value.
    5. Splice the buffer.
    6. Patch all sibling variable-size field offsets whose targets are at
       or past the splice point.
    """
    ft = field.frac_type
    eff_ft = _effective_offset_type(ft)

    def write(mv: Any, value: Any) -> None:
        base = mv._base

        # Determine if we're setting to None (optional only)
        setting_none = (value is None and isinstance(ft, OptionalType))

        # Check for trailing optional elision
        if _is_field_elided(mv._data, base, header_size, field):
            if setting_none:
                # Already elided and setting to None — nothing to do
                return
            # Expand the fixed region to include this field
            _expand_fixed_region(mv._data, base, header_size, field,
                                 all_fields)

        offset_pos = base + header_size + field.fixed_offset

        # Read current offset
        old_offset = _read_u32(mv._data, offset_pos)

        # Determine if we're setting to an empty container
        setting_empty = False
        if not setting_none and ft.is_variable_size:
            eff = _effective_offset_type(ft)
            if hasattr(eff, 'is_empty') and eff.is_empty(value):
                setting_empty = True

        if setting_none:
            # Setting optional to None
            if old_offset <= 1:
                # Already None or empty — just set the marker
                _write_u32(mv._data, offset_pos, 1)
                return
            # There is existing heap data — remove it
            old_target = offset_pos + old_offset
            old_size = _packed_size_at(eff_ft, mv._data, old_target)
            splice_pos = old_target
            delta = splice_buffer(mv._data, splice_pos, old_size, b"")
            _write_u32(mv._data, offset_pos, 1)
            # Patch siblings
            _patch_siblings(mv._data, base, header_size, field, all_fields,
                            splice_pos, delta)
            return

        if setting_empty:
            # Setting to empty container
            if old_offset <= 1:
                _write_u32(mv._data, offset_pos, 0)
                return
            # Remove existing heap data
            old_target = offset_pos + old_offset
            old_size = _packed_size_at(eff_ft, mv._data, old_target)
            splice_pos = old_target
            delta = splice_buffer(mv._data, splice_pos, old_size, b"")
            _write_u32(mv._data, offset_pos, 0)
            _patch_siblings(mv._data, base, header_size, field, all_fields,
                            splice_pos, delta)
            return

        # Pack new value as standalone bytes
        if isinstance(ft, OptionalType):
            new_bytes = _pack_standalone(ft.inner, value)
        else:
            new_bytes = _pack_standalone(ft, value)

        if old_offset <= 1:
            # No existing heap data — insert at end of buffer
            splice_pos = len(mv._data)
            delta = splice_buffer(mv._data, splice_pos, 0, new_bytes)
            # Set offset to point to the new data
            _write_u32(mv._data, offset_pos, splice_pos - offset_pos)
            # No siblings to patch since we appended at the end
            return

        # Replace existing heap data
        old_target = offset_pos + old_offset
        old_size = _packed_size_at(eff_ft, mv._data, old_target)
        splice_pos = old_target
        delta = splice_buffer(mv._data, splice_pos, old_size, new_bytes)
        # The offset_pos -> target distance doesn't change because we
        # spliced at the target position; the offset stays the same.
        # But we must patch siblings.
        _patch_siblings(mv._data, base, header_size, field, all_fields,
                        splice_pos, delta)

    return write


def _make_variable_writer_fast(field: FieldInfo, header_size: int,
                               struct_type: StructType,
                               all_fields: list[FieldInfo] | None = None):
    """Writer for a variable-size field in fast mode.

    If the new data fits in the old slot, overwrite in place.
    If larger, append to end and update the offset.
    Setting to None just sets offset to 1 (old data becomes dead bytes).
    """
    ft = field.frac_type
    eff_ft = _effective_offset_type(ft)
    if all_fields is None:
        all_fields = struct_type.fields

    def write(mv: Any, value: Any) -> None:
        base = mv._base

        # Setting to None
        if value is None and isinstance(ft, OptionalType):
            if _is_field_elided(mv._data, base, header_size, field):
                return  # Already elided = None
            _write_u32(mv._data, base + header_size + field.fixed_offset, 1)
            return

        # Check for trailing optional elision — expand if needed
        if _is_field_elided(mv._data, base, header_size, field):
            _expand_fixed_region(mv._data, base, header_size, field,
                                 all_fields)

        offset_pos = base + header_size + field.fixed_offset
        old_offset = _read_u32(mv._data, offset_pos)

        # Setting to empty container
        if ft.is_variable_size:
            eff = _effective_offset_type(ft)
            if hasattr(eff, 'is_empty') and eff.is_empty(value):
                _write_u32(mv._data, offset_pos, 0)
                return

        # Pack new value
        if isinstance(ft, OptionalType):
            new_bytes = _pack_standalone(ft.inner, value)
        else:
            new_bytes = _pack_standalone(ft, value)

        if old_offset <= 1:
            # No existing data — append
            append_pos = len(mv._data)
            mv._data.extend(new_bytes)
            _write_u32(mv._data, offset_pos, append_pos - offset_pos)
            return

        # Measure old data
        old_target = offset_pos + old_offset
        old_size = _packed_size_at(eff_ft, mv._data, old_target)

        if len(new_bytes) <= old_size:
            # Fits — overwrite in place (dead bytes at the end are OK)
            mv._data[old_target:old_target + len(new_bytes)] = new_bytes
            # Remaining bytes are dead but harmless in fast mode
            return

        # Doesn't fit — append to end, update offset
        append_pos = len(mv._data)
        mv._data.extend(new_bytes)
        _write_u32(mv._data, offset_pos, append_pos - offset_pos)

    return write


def _patch_siblings(data: bytearray, base: int, header_size: int,
                    modified_field: FieldInfo,
                    all_fields: list[FieldInfo],
                    splice_pos: int, delta: int) -> None:
    """Patch self-relative offsets of sibling variable-size fields.

    Only patches fields whose offset slots are BEFORE the splice point
    and whose targets are at or past the splice point.  Skips fields
    that were elided (trailing optional elision).
    """
    if delta == 0:
        return
    for f in all_fields:
        if f is modified_field:
            continue
        if not f.frac_type.is_variable_size:
            continue
        # Skip elided fields (their offset slot doesn't exist)
        if _is_field_elided(data, base, header_size, f):
            continue
        sibling_offset_pos = base + header_size + f.fixed_offset
        patch_offset(data, sibling_offset_pos, splice_pos, delta)


# ── MutView class factory ──────────────────────────────────────────────────

def make_mut_view_class(class_name: str, struct_type: StructType) -> type:
    """Create a mutable view class for the given struct type.

    The returned class supports reading fields (like View) and writing
    fields via attribute assignment, modifying the underlying bytearray
    in place.

    Parameters
    ----------
    class_name : str
        Name of the parent @schema class (used for repr/error messages).
    struct_type : StructType
        The compiled struct layout.

    Returns
    -------
    type
        A MutView class.
    """
    header_size = 2 if struct_type.extensible else 0
    fields = struct_type.fields

    # ── Build reader table (reuse from _view.py) ──────────────────────
    readers: dict[str, Any] = {}
    for field in fields:
        reader = _make_reader_for_type(
            header_size + field.fixed_offset,
            field.frac_type,
        )
        if struct_type.extensible and field.frac_type.is_optional:
            reader = _make_elision_checked_reader(
                reader, field.fixed_offset, field.frac_type,
            )
        readers[field.name] = reader

    # ── Build writer tables (canonical and fast) ──────────────────────
    canonical_writers: dict[str, Any] = {}
    fast_writers: dict[str, Any] = {}

    for field in fields:
        ft = field.frac_type
        abs_offset = header_size + field.fixed_offset

        if not ft.is_variable_size:
            # Fixed-size scalar — same writer for both modes
            if isinstance(ft, BoolType):
                w = _make_bool_writer(abs_offset)
            elif isinstance(ft, (IntType, FloatType)):
                w = _make_scalar_writer(abs_offset, ft.fmt, ft.fixed_size)
            else:
                # Fallback for unknown fixed-size types
                def _make_fallback_writer(f=field, hs=header_size):
                    def write(mv: Any, value: Any) -> None:
                        new_bytes = _pack_standalone(f.frac_type, value)
                        pos = mv._base + hs + f.fixed_offset
                        mv._data[pos:pos + len(new_bytes)] = new_bytes
                    return write
                w = _make_fallback_writer()
            canonical_writers[field.name] = w
            fast_writers[field.name] = w
        else:
            # Variable-size field
            canonical_writers[field.name] = _make_variable_writer_canonical(
                field, header_size, fields, struct_type,
            )
            fast_writers[field.name] = _make_variable_writer_fast(
                field, header_size, struct_type, fields,
            )

    field_names = frozenset(readers.keys())

    class MutViewBase:
        """Mutable view over packed fracpack data.

        Reads fields lazily (like View) and supports field assignment
        that modifies the underlying bytearray.
        """
        __slots__ = ("_data", "_base", "_fast", "_writers")

        def __init__(self, data: bytearray, base: int = 0,
                     fast: bool = False) -> None:
            if not isinstance(data, bytearray):
                raise TypeError(
                    "MutView requires a bytearray, got "
                    f"{type(data).__name__}"
                )
            object.__setattr__(self, "_data", data)
            object.__setattr__(self, "_base", base)
            object.__setattr__(self, "_fast", fast)
            object.__setattr__(
                self, "_writers",
                fast_writers if fast else canonical_writers,
            )

        def __getattr__(self, name: str) -> Any:
            reader = readers.get(name)
            if reader is None:
                raise AttributeError(
                    f"'{class_name}.MutView' has no field '{name}'"
                )
            data = object.__getattribute__(self, "_data")
            base = object.__getattribute__(self, "_base")
            return reader(memoryview(data), base)

        def __setattr__(self, name: str, value: Any) -> None:
            writers = object.__getattribute__(self, "_writers")
            writer = writers.get(name)
            if writer is None:
                if name in field_names:
                    raise AttributeError(
                        f"Cannot write field '{name}' "
                        f"(no writer registered)"
                    )
                raise AttributeError(
                    f"'{class_name}.MutView' has no field '{name}'"
                )
            writer(self, value)

        def to_bytes(self) -> bytes:
            """Return a copy of the current buffer contents as bytes."""
            data = object.__getattribute__(self, "_data")
            return bytes(data)

        def __bytes__(self) -> bytes:
            return self.to_bytes()

        def compact(self) -> None:
            """Repack the buffer from scratch (removes dead bytes).

            Primarily useful after fast-mode mutations.  In canonical mode
            this is a no-op in terms of correctness but can be used to
            verify round-trip consistency.
            """
            data = object.__getattribute__(self, "_data")
            base = object.__getattribute__(self, "_base")
            # Full unpack
            c = UnpackCursor(bytes(data), base)
            raw = unpack_value(struct_type, c)
            # Full repack
            w = PackBuffer()
            pack_value(struct_type, raw, w)
            new_data = w.finish()
            # Replace buffer contents
            data[:] = new_data
            object.__setattr__(self, "_base", 0)

        def __repr__(self) -> str:
            data = object.__getattribute__(self, "_data")
            base = object.__getattribute__(self, "_base")
            fast = object.__getattribute__(self, "_fast")
            mode = "fast" if fast else "canonical"
            parts = []
            mv = memoryview(data)
            for name, reader in readers.items():
                try:
                    val = reader(mv, base)
                    parts.append(f"{name}={val!r}")
                except Exception:
                    parts.append(f"{name}=<error>")
            return (
                f"{class_name}.MutView({', '.join(parts)}, mode={mode})"
            )

    MutViewBase.__name__ = f"{class_name}MutView"
    MutViewBase.__qualname__ = f"{class_name}.MutView"

    return MutViewBase
