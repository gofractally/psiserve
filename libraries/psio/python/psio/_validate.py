"""Zero-copy fracpack validation.

Validates packed binary data against a type schema without allocating
Python objects.  Returns a :class:`ValidationResult` indicating whether
the data is valid, extended (forward-compatible unknown fields), or
invalid (malformed).

Security-critical checks:
  1. Buffer bounds — every read checks remaining bytes
  2. Offset validity — offsets must be >= 4 for valid pointers
  3. Forward-only offsets — heap data must not overlap or go backward
  4. Size alignment — vec data_size divisible by element fixed_size
  5. UTF-8 validation
  6. Variant index bounds
  7. No extra data — all bytes consumed
  8. Extension detection — extra fixed bytes in extensible structs
  9. Recursive validation of nested types
 10. Bool values — must be 0 or 1
"""

from __future__ import annotations

import struct as _st
from dataclasses import dataclass

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


@dataclass(slots=True)
class ValidationResult:
    """Outcome of validating packed fracpack data."""

    status: str  # "valid", "extended", or "invalid"
    message: str | None = None  # error/info message for non-"valid"


class ValidationError(Exception):
    """Raised internally when validation fails."""


class _Validator:
    """Position-tracking cursor that validates fracpack data without
    allocating Python objects for the decoded values."""

    __slots__ = ("buf", "end", "extended")

    def __init__(self, buf: memoryview) -> None:
        self.buf = buf
        self.end = len(buf)
        self.extended = False

    # ── Bounds checking ─────────────────────────────────────────────────

    def _check(self, pos: int, size: int) -> None:
        if pos < 0 or pos + size > self.end:
            raise ValidationError(
                f"Read past end at {pos}+{size} (buffer size {self.end})"
            )

    # ── Primitive reads (with bounds checks) ────────────────────────────

    def _read_u8(self, pos: int) -> int:
        self._check(pos, 1)
        return self.buf[pos]

    def _read_u16(self, pos: int) -> int:
        self._check(pos, 2)
        return _st.unpack_from("<H", self.buf, pos)[0]

    def _read_u32(self, pos: int) -> int:
        self._check(pos, 4)
        return _st.unpack_from("<I", self.buf, pos)[0]

    # ── Top-level entry point ───────────────────────────────────────────

    def validate_top(self, ft: FracType) -> ValidationResult:
        """Validate *ft* as a top-level (standalone) packed value.

        Ensures all bytes are consumed and returns the result.
        """
        try:
            end_pos = self._validate_at(ft, 0)
            if end_pos != self.end:
                raise ValidationError(
                    f"Extra data: {self.end - end_pos} bytes after valid data"
                )
            if self.extended:
                return ValidationResult("extended", "Data has unknown fields")
            return ValidationResult("valid")
        except ValidationError as exc:
            return ValidationResult("invalid", str(exc))

    # ── Core dispatch: validate value at *pos*, return pos after data ───

    def _validate_at(self, ft: FracType, pos: int) -> int:  # noqa: C901 (dispatch)
        if isinstance(ft, BoolType):
            self._check(pos, 1)
            v = self.buf[pos]
            if v > 1:
                raise ValidationError(f"Invalid bool value: {v}")
            return pos + 1

        if isinstance(ft, IntType):
            size = ft.fixed_size
            self._check(pos, size)
            return pos + size

        if isinstance(ft, FloatType):
            size = ft.fixed_size
            self._check(pos, size)
            return pos + size

        if isinstance(ft, StringType):
            self._check(pos, 4)
            length = self._read_u32(pos)
            self._check(pos + 4, length)
            self._validate_utf8(pos + 4, length)
            return pos + 4 + length

        if isinstance(ft, BytesType):
            self._check(pos, 4)
            length = self._read_u32(pos)
            self._check(pos + 4, length)
            return pos + 4 + length

        if isinstance(ft, OptionalType):
            return self._validate_optional(ft, pos)

        if isinstance(ft, VecType):
            return self._validate_vec(ft, pos)

        if isinstance(ft, ArrayType):
            return self._validate_array(ft, pos)

        if isinstance(ft, VariantType):
            return self._validate_variant(ft, pos)

        if isinstance(ft, TupleType):
            return self._validate_tuple(ft, pos)

        if isinstance(ft, StructType):
            return self._validate_struct(ft, pos)

        raise ValidationError(f"Unknown type: {ft!r}")

    # ── Optional ────────────────────────────────────────────────────────

    def _validate_optional(self, ft: OptionalType, pos: int) -> int:
        self._check(pos, 4)
        offset = self._read_u32(pos)
        if offset == 1:
            return pos + 4  # None
        if offset == 0:
            return pos + 4  # empty container
        # offset must be >= 4 for a valid forward pointer
        if offset < 4:
            raise ValidationError(
                f"Reserved offset value {offset} at {pos}"
            )
        target = pos + offset
        if target > self.end:
            raise ValidationError(
                f"Offset {offset} at {pos} points past buffer end"
            )
        return self._validate_at(ft.inner, target)

    # ── Vec ─────────────────────────────────────────────────────────────

    def _validate_vec(self, ft: VecType, pos: int) -> int:
        self._check(pos, 4)
        data_size = self._read_u32(pos)
        elem_fixed = ft.element.fixed_size
        if elem_fixed == 0:
            raise ValidationError("Vec element has zero fixed size")
        if data_size % elem_fixed != 0:
            raise ValidationError(
                f"Vec data_size {data_size} not divisible by "
                f"element fixed_size {elem_fixed}"
            )
        count = data_size // elem_fixed
        fixed_start = pos + 4
        self._check(fixed_start, data_size)

        if not ft.element.is_variable_size:
            # Fixed-size elements: validate each inline
            for i in range(count):
                self._validate_at(ft.element, fixed_start + i * elem_fixed)
            return fixed_start + data_size

        # Variable-size elements: follow offsets
        end_pos = fixed_start + data_size
        for i in range(count):
            fp = fixed_start + i * elem_fixed
            end_pos = self._validate_embedded(ft.element, fp, end_pos)
        return end_pos

    # ── Array ───────────────────────────────────────────────────────────

    def _validate_array(self, ft: ArrayType, pos: int) -> int:
        elem_fixed = ft.element.fixed_size
        total_fixed = elem_fixed * ft.length
        self._check(pos, total_fixed)

        if not ft.element.is_variable_size:
            for i in range(ft.length):
                self._validate_at(ft.element, pos + i * elem_fixed)
            return pos + total_fixed

        end_pos = pos + total_fixed
        for i in range(ft.length):
            end_pos = self._validate_embedded(
                ft.element, pos + i * elem_fixed, end_pos
            )
        return end_pos

    # ── Variant ─────────────────────────────────────────────────────────

    def _validate_variant(self, ft: VariantType, pos: int) -> int:
        self._check(pos, 5)  # 1 byte tag + 4 bytes size
        tag = self._read_u8(pos)
        data_size = self._read_u32(pos + 1)
        cases_list = list(ft.cases.values())
        if tag >= len(cases_list):
            raise ValidationError(
                f"Variant index {tag} out of range ({len(cases_list)} cases)"
            )
        self._check(pos + 5, data_size)
        case_ft = cases_list[tag]
        if case_ft is not None:
            self._validate_at(case_ft, pos + 5)
        return pos + 5 + data_size

    # ── Tuple ───────────────────────────────────────────────────────────

    def _validate_tuple(self, ft: TupleType, pos: int) -> int:
        self._check(pos, 2)
        fixed_size = self._read_u16(pos)
        fixed_start = pos + 2
        heap_pos = fixed_start + fixed_size
        self._check(fixed_start, fixed_size)

        end_pos = heap_pos
        field_pos = fixed_start
        for elem_ft in ft.elements:
            if field_pos >= heap_pos:
                if not elem_ft.is_optional:
                    raise ValidationError(
                        "Non-optional tuple element missing"
                    )
                continue
            end_pos = self._validate_embedded(elem_ft, field_pos, end_pos)
            field_pos += elem_ft.fixed_size
        return end_pos

    # ── Struct ──────────────────────────────────────────────────────────

    def _validate_struct(self, ft: StructType, pos: int) -> int:
        if ft.extensible:
            return self._validate_extensible_struct(ft, pos)
        return self._validate_fixed_struct(ft, pos)

    def _validate_extensible_struct(self, ft: StructType, pos: int) -> int:
        self._check(pos, 2)
        fixed_size = self._read_u16(pos)
        fixed_start = pos + 2
        heap_pos = fixed_start + fixed_size
        self._check(fixed_start, fixed_size)

        # Extension detection: more fixed bytes than our schema defines
        expected_fixed = ft.members_fixed_size
        if fixed_size > expected_fixed:
            self.extended = True

        end_pos = heap_pos
        field_pos = fixed_start
        for fi in ft.fields:
            if field_pos + fi.frac_type.fixed_size > heap_pos:
                # Field doesn't fit — acceptable only for optional fields
                if not fi.frac_type.is_optional:
                    break
                continue
            end_pos = self._validate_embedded(
                fi.frac_type, field_pos, end_pos
            )
            field_pos += fi.frac_type.fixed_size
        return end_pos

    def _validate_fixed_struct(self, ft: StructType, pos: int) -> int:
        total_fixed = ft.members_fixed_size
        self._check(pos, total_fixed)

        end_pos = pos + total_fixed
        field_pos = pos
        for fi in ft.fields:
            end_pos = self._validate_embedded(
                fi.frac_type, field_pos, end_pos
            )
            field_pos += fi.frac_type.fixed_size
        return end_pos

    # ── Embedded field validation ───────────────────────────────────────

    def _validate_embedded(
        self, ft: FracType, fixed_pos: int, end_pos: int
    ) -> int:
        """Validate an embedded field at *fixed_pos* with heap data
        starting at *end_pos*.  Returns new end_pos after any heap
        data consumed."""
        if not ft.is_variable_size:
            self._validate_at(ft, fixed_pos)
            return end_pos

        # Variable-size: read the offset at fixed_pos
        self._check(fixed_pos, 4)
        offset = self._read_u32(fixed_pos)

        if offset == 0:
            return end_pos  # empty container

        if isinstance(ft, OptionalType):
            if offset == 1:
                return end_pos  # None
            if offset < 4:
                raise ValidationError(
                    f"Reserved offset value {offset} at {fixed_pos}"
                )
            target = fixed_pos + offset
            if target < end_pos:
                raise ValidationError(
                    f"Offset {offset} at {fixed_pos} points before heap "
                    f"(target {target} < heap {end_pos})"
                )
            # For embedded optional, the offset already points to the
            # inner value directly (no double-offset resolution).
            return self._validate_at(ft.inner, target)

        if offset < 4:
            raise ValidationError(
                f"Reserved offset value {offset} at {fixed_pos}"
            )
        target = fixed_pos + offset
        if target < end_pos:
            raise ValidationError(
                f"Offset {offset} at {fixed_pos} points before heap "
                f"(target {target} < heap {end_pos})"
            )
        return self._validate_at(ft, target)

    # ── UTF-8 validation ────────────────────────────────────────────────

    def _validate_utf8(self, pos: int, length: int) -> None:
        """Validate that *length* bytes starting at *pos* are valid UTF-8."""
        end = pos + length
        i = pos
        while i < end:
            b = self.buf[i]
            if b < 0x80:
                i += 1
                continue

            if (b & 0xE0) == 0xC0:
                need = 1
                if b < 0xC2:
                    # Overlong 2-byte sequence
                    raise ValidationError(
                        f"Overlong UTF-8 sequence at {i}"
                    )
            elif (b & 0xF0) == 0xE0:
                need = 2
            elif (b & 0xF8) == 0xF0:
                need = 3
                if b > 0xF4:
                    raise ValidationError(
                        f"Invalid UTF-8 lead byte 0x{b:02x} at {i}"
                    )
            else:
                raise ValidationError(
                    f"Invalid UTF-8 lead byte 0x{b:02x} at {i}"
                )

            if i + 1 + need > end:
                raise ValidationError(
                    f"Truncated UTF-8 sequence at {i}"
                )
            for j in range(1, need + 1):
                cont = self.buf[i + j]
                if (cont & 0xC0) != 0x80:
                    raise ValidationError(
                        f"Invalid UTF-8 continuation byte 0x{cont:02x} at {i + j}"
                    )
            i += 1 + need


# ── Public API ──────────────────────────────────────────────────────────────

def validate(ft: FracType, data: bytes | memoryview) -> ValidationResult:
    """Validate packed fracpack *data* against type *ft* without unpacking.

    Returns:
      - ``ValidationResult("valid")`` — data matches schema exactly
      - ``ValidationResult("extended", ...)`` — valid but has unknown fields
      - ``ValidationResult("invalid", ...)`` — data is malformed
    """
    buf = memoryview(data) if isinstance(data, bytes) else data
    v = _Validator(buf)
    return v.validate_top(ft)
