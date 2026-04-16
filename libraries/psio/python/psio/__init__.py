"""psio — Python fracpack serialisation library.

Quick start::

    from psio import schema, types as t

    @schema
    class Person:
        name: t.string
        age: t.u32
        active: t.bool_

    alice = Person(name="Alice", age=30, active=True)
    data = alice.pack()
    alice2 = Person.unpack(data)
    view = Person.view(data)
    assert view.name == "Alice"
"""

from ._json import from_json, to_json
from ._schema import schema
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
    # Public type aliases
    bool_,
    u8,
    u16,
    u32,
    u64,
    i8,
    i16,
    i32,
    i64,
    f32,
    f64,
    string,
    bytes_,
    # Container constructors
    array,
    optional,
    variant,
    vec,
)

__all__ = [
    # Decorator
    "schema",
    # JSON
    "to_json",
    "from_json",
    # Type descriptors
    "FracType",
    "BoolType",
    "IntType",
    "FloatType",
    "StringType",
    "BytesType",
    "VecType",
    "OptionalType",
    "ArrayType",
    "VariantType",
    "TupleType",
    "StructType",
    "FieldInfo",
    # Type aliases
    "bool_",
    "u8",
    "u16",
    "u32",
    "u64",
    "i8",
    "i16",
    "i32",
    "i64",
    "f32",
    "f64",
    "string",
    "bytes_",
    # Containers
    "vec",
    "optional",
    "array",
    "variant",
]
