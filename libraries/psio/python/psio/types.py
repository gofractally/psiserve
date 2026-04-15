"""Fracpack type descriptors and annotation aliases.

Usage::

    from psio import types as t

    @schema
    class Point:
        x: t.f32
        y: t.f32

Each public name (``u32``, ``string``, ``vec``, etc.) is a
``typing.Annotated`` alias that carries both:

* the Python type (for type checkers and dataclass generation), and
* a :class:`FracType` instance (for runtime schema compilation).

Container subscripts (``vec[u32]``, ``optional[string]``) construct
new ``Annotated`` aliases at class-definition time.
"""

from __future__ import annotations

import struct as _struct
from typing import Annotated, Any, get_args, get_origin


# ── FracType base ────────────────────────────────────────────────────────────

class FracType:
    """Describes a fracpack-serialisable type at runtime."""

    __slots__ = ()

    fixed_size: int
    """Bytes this type occupies in a parent's fixed region."""

    is_variable_size: bool
    """True when data lives on the heap (offset in fixed region)."""

    is_optional: bool

    def is_empty(self, value: Any) -> bool:
        """True when *value* represents an empty container (offset = 0)."""
        return False


# ── Scalars ──────────────────────────────────────────────────────────────────

class BoolType(FracType):
    __slots__ = ()
    fixed_size = 1
    is_variable_size = False
    is_optional = False
    fmt = "<?"

    def __repr__(self) -> str:
        return "BoolType()"


class IntType(FracType):
    __slots__ = ("bits", "signed", "fmt")

    is_variable_size = False
    is_optional = False

    _FMT = {
        (8, False): "<B", (8, True): "<b",
        (16, False): "<H", (16, True): "<h",
        (32, False): "<I", (32, True): "<i",
        (64, False): "<Q", (64, True): "<q",
    }

    def __init__(self, bits: int, signed: bool) -> None:
        self.bits = bits
        self.signed = signed
        self.fmt = self._FMT[(bits, signed)]

    @property
    def fixed_size(self) -> int:
        return self.bits // 8

    def __repr__(self) -> str:
        prefix = "i" if self.signed else "u"
        return f"{prefix}{self.bits}"


class FloatType(FracType):
    __slots__ = ("bits", "fmt")

    is_variable_size = False
    is_optional = False

    def __init__(self, bits: int) -> None:
        self.bits = bits
        self.fmt = "<f" if bits == 32 else "<d"

    @property
    def fixed_size(self) -> int:
        return self.bits // 8

    def __repr__(self) -> str:
        return f"f{self.bits}"


# ── String ───────────────────────────────────────────────────────────────────

class StringType(FracType):
    __slots__ = ()
    fixed_size = 4  # offset
    is_variable_size = True
    is_optional = False

    def is_empty(self, value: Any) -> bool:
        return value == ""

    def __repr__(self) -> str:
        return "string"


# ── Bytes ────────────────────────────────────────────────────────────────────

class BytesType(FracType):
    __slots__ = ()
    fixed_size = 4
    is_variable_size = True
    is_optional = False

    def is_empty(self, value: Any) -> bool:
        return len(value) == 0

    def __repr__(self) -> str:
        return "bytes_"


# ── Vec ──────────────────────────────────────────────────────────────────────

class VecType(FracType):
    __slots__ = ("element",)
    fixed_size = 4
    is_variable_size = True
    is_optional = False

    def __init__(self, element: FracType) -> None:
        self.element = element

    def is_empty(self, value: Any) -> bool:
        return len(value) == 0

    def __repr__(self) -> str:
        return f"vec[{self.element!r}]"


# ── Optional ─────────────────────────────────────────────────────────────────

class OptionalType(FracType):
    __slots__ = ("inner",)
    fixed_size = 4
    is_variable_size = True
    is_optional = True

    def __init__(self, inner: FracType) -> None:
        self.inner = inner

    def __repr__(self) -> str:
        return f"optional[{self.inner!r}]"


# ── Array (fixed-length) ────────────────────────────────────────────────────

class ArrayType(FracType):
    __slots__ = ("element", "length")
    is_optional = False

    def __init__(self, element: FracType, length: int) -> None:
        self.element = element
        self.length = length

    @property
    def fixed_size(self) -> int:
        if self.element.is_variable_size:
            return 4  # offset to heap
        return self.element.fixed_size * self.length

    @property
    def is_variable_size(self) -> bool:
        return self.element.is_variable_size

    def __repr__(self) -> str:
        return f"array[{self.element!r}, {self.length}]"


# ── Variant ──────────────────────────────────────────────────────────────────

class VariantType(FracType):
    __slots__ = ("cases",)
    fixed_size = 4  # offset
    is_variable_size = True
    is_optional = False

    def __init__(self, cases: dict[str, FracType | None]) -> None:
        if len(cases) > 127:
            raise ValueError("Variant may have at most 127 cases")
        self.cases = cases

    def __repr__(self) -> str:
        return f"variant({self.cases!r})"


# ── Tuple ────────────────────────────────────────────────────────────────────

class TupleType(FracType):
    __slots__ = ("elements",)
    fixed_size = 4  # always extensible → always variable → offset
    is_variable_size = True
    is_optional = False

    def __init__(self, elements: list[FracType]) -> None:
        self.elements = elements

    def __repr__(self) -> str:
        return f"frac_tuple[{', '.join(repr(e) for e in self.elements)}]"


# ── Struct (created by @schema, not hand-constructed) ────────────────────────

class FieldInfo:
    """Metadata for one struct field, computed at @schema time."""
    __slots__ = ("name", "frac_type", "python_type", "fixed_offset", "index")

    def __init__(self, name: str, frac_type: FracType, python_type: type,
                 fixed_offset: int, index: int) -> None:
        self.name = name
        self.frac_type = frac_type
        self.python_type = python_type
        self.fixed_offset = fixed_offset
        self.index = index


class StructType(FracType):
    """Computed layout for a @schema-decorated class."""
    __slots__ = ("fields", "extensible", "members_fixed_size", "_field_map")
    is_optional = False

    def __init__(self, fields: list[FieldInfo], extensible: bool = True) -> None:
        self.fields = fields
        self.extensible = extensible
        self.members_fixed_size = sum(f.frac_type.fixed_size for f in fields)
        self._field_map = {f.name: f for f in fields}

    @property
    def fixed_size(self) -> int:
        if self.is_variable_size:
            return 4  # offset in parent
        return self.members_fixed_size

    @property
    def is_variable_size(self) -> bool:
        if self.extensible:
            return True
        return any(f.frac_type.is_variable_size for f in self.fields)


# ── Helpers: extract FracType from Annotated ─────────────────────────────────

def extract_frac_type(annotation: Any) -> FracType | None:
    """Return the FracType from an ``Annotated[T, FracType()]`` or None.

    Also recognises ``@schema``-decorated classes (which carry
    ``__frac_type__``) so nested structs work as field annotations.
    """
    if get_origin(annotation) is Annotated:
        for arg in get_args(annotation)[1:]:
            if isinstance(arg, FracType):
                return arg
    # Check for @schema-decorated classes
    ft = getattr(annotation, "__frac_type__", None)
    if isinstance(ft, FracType):
        return ft
    return None


def extract_python_type(annotation: Any) -> type:
    """Return the base Python type from ``Annotated[T, ...]``."""
    if get_origin(annotation) is Annotated:
        return get_args(annotation)[0]
    return annotation


# ── Singleton descriptors ────────────────────────────────────────────────────

_bool = BoolType()
_u8 = IntType(8, False)
_u16 = IntType(16, False)
_u32 = IntType(32, False)
_u64 = IntType(64, False)
_i8 = IntType(8, True)
_i16 = IntType(16, True)
_i32 = IntType(32, True)
_i64 = IntType(64, True)
_f32 = FloatType(32)
_f64 = FloatType(64)
_string = StringType()
_bytes = BytesType()


# ── Public Annotated aliases ─────────────────────────────────────────────────
# Type checkers see the Python type; @schema sees the FracType metadata.

bool_ = Annotated[bool, _bool]
u8 = Annotated[int, _u8]
u16 = Annotated[int, _u16]
u32 = Annotated[int, _u32]
u64 = Annotated[int, _u64]
i8 = Annotated[int, _i8]
i16 = Annotated[int, _i16]
i32 = Annotated[int, _i32]
i64 = Annotated[int, _i64]
f32 = Annotated[float, _f32]
f64 = Annotated[float, _f64]
string = Annotated[str, _string]
bytes_ = Annotated[bytes, _bytes]


# ── Container constructors (subscript syntax) ────────────────────────────────

class vec:
    """Variable-length list.  Usage: ``vec[t.u32]``, ``vec[t.string]``."""

    def __class_getitem__(cls, item: Any) -> Any:
        inner = extract_frac_type(item)
        if inner is None:
            raise TypeError(f"vec[] requires a psio type, got {item!r}")
        py = extract_python_type(item)
        return Annotated[list[py], VecType(inner)]  # type: ignore[valid-type]


class optional:
    """Nullable wrapper.  Usage: ``optional[t.u32]``, ``optional[t.string]``."""

    def __class_getitem__(cls, item: Any) -> Any:
        inner = extract_frac_type(item)
        if inner is None:
            raise TypeError(f"optional[] requires a psio type, got {item!r}")
        py = extract_python_type(item)
        return Annotated[py | None, OptionalType(inner)]  # type: ignore[valid-type]


class array:
    """Fixed-length array.  Usage: ``array[t.u8, 32]``."""

    def __class_getitem__(cls, params: Any) -> Any:
        if not isinstance(params, tuple) or len(params) != 2:
            raise TypeError("array[] requires (type, length), e.g. array[t.u8, 32]")
        item, length = params
        inner = extract_frac_type(item)
        if inner is None:
            raise TypeError(f"array[] element must be a psio type, got {item!r}")
        py = extract_python_type(item)
        return Annotated[list[py], ArrayType(inner, length)]  # type: ignore[valid-type]


def variant(**cases: Any) -> Any:
    """Tagged union.  Usage: ``Shape = variant(Circle=t.f64, Point=None)``."""
    frac_cases: dict[str, FracType | None] = {}
    for name, ann in cases.items():
        if ann is None:
            frac_cases[name] = None
        else:
            ft = extract_frac_type(ann)
            if ft is None:
                raise TypeError(f"variant case {name!r} must be a psio type or None")
            frac_cases[name] = ft
    return Annotated[dict, VariantType(frac_cases)]  # type: ignore[valid-type]
