"""Tests for the in-place mutation system (psio._mutation).

Covers:
  - Scalar field writes (bool, int, float)
  - String field writes (grow, shrink, same size)
  - Optional set to None and back to a value
  - Fast mode: append and compact
  - Multiple field mutations
  - Read-after-write consistency
  - Canonical round-trip (mutated buffer unpacks correctly)
"""

from __future__ import annotations

import pytest

from psio import schema, types as t


# ── Test schemas ────────────────────────────────────────────────────────────

@schema
class Simple:
    x: t.u32
    y: t.u32


@schema
class Person:
    name: t.string
    age: t.u32
    active: t.bool_


@schema
class WithOptional:
    name: t.string
    bio: t.optional[t.string]
    score: t.optional[t.u32]


@schema
class MultiString:
    first: t.string
    second: t.string
    third: t.string


@schema
class MixedFields:
    label: t.string
    count: t.u32
    flag: t.bool_
    data: t.bytes_


# ── Scalar writes ──────────────────────────────────────────────────────────

class TestScalarWrites:
    def test_set_u32(self):
        obj = Simple(x=10, y=20)
        data = obj.pack()
        mv = Simple.mut_view(data)
        mv.x = 99
        result = Simple.unpack(mv.to_bytes())
        assert result.x == 99
        assert result.y == 20

    def test_set_both_u32(self):
        obj = Simple(x=10, y=20)
        mv = Simple.mut_view(obj.pack())
        mv.x = 100
        mv.y = 200
        result = Simple.unpack(mv.to_bytes())
        assert result.x == 100
        assert result.y == 200

    def test_set_bool(self):
        obj = Person(name="Alice", age=30, active=True)
        mv = Person.mut_view(obj.pack())
        mv.active = False
        result = Person.unpack(mv.to_bytes())
        assert result.active is False
        assert result.name == "Alice"
        assert result.age == 30

    def test_set_age(self):
        obj = Person(name="Alice", age=30, active=True)
        mv = Person.mut_view(obj.pack())
        mv.age = 99
        result = Person.unpack(mv.to_bytes())
        assert result.age == 99
        assert result.name == "Alice"


# ── String writes ──────────────────────────────────────────────────────────

class TestStringWrites:
    def test_set_string_same_size(self):
        obj = Person(name="Alice", age=30, active=True)
        mv = Person.mut_view(obj.pack())
        mv.name = "Bobby"  # same length as "Alice"
        result = Person.unpack(mv.to_bytes())
        assert result.name == "Bobby"
        assert result.age == 30

    def test_set_string_grow(self):
        obj = Person(name="Al", age=30, active=True)
        mv = Person.mut_view(obj.pack())
        mv.name = "Alexander"
        result = Person.unpack(mv.to_bytes())
        assert result.name == "Alexander"
        assert result.age == 30
        assert result.active is True

    def test_set_string_shrink(self):
        obj = Person(name="Alexander", age=30, active=True)
        mv = Person.mut_view(obj.pack())
        mv.name = "Al"
        result = Person.unpack(mv.to_bytes())
        assert result.name == "Al"
        assert result.age == 30
        assert result.active is True

    def test_set_string_to_empty(self):
        obj = Person(name="Alice", age=30, active=True)
        mv = Person.mut_view(obj.pack())
        mv.name = ""
        result = Person.unpack(mv.to_bytes())
        assert result.name == ""

    def test_set_string_from_empty(self):
        obj = Person(name="", age=30, active=True)
        mv = Person.mut_view(obj.pack())
        mv.name = "Hello"
        result = Person.unpack(mv.to_bytes())
        assert result.name == "Hello"
        assert result.age == 30


# ── Multiple string fields ────────────────────────────────────────────────

class TestMultipleStringFields:
    def test_set_first_string_grow(self):
        """Growing the first string should correctly patch second and third."""
        obj = MultiString(first="A", second="BB", third="CCC")
        mv = MultiString.mut_view(obj.pack())
        mv.first = "AAAAAAAAAA"  # grow significantly
        result = MultiString.unpack(mv.to_bytes())
        assert result.first == "AAAAAAAAAA"
        assert result.second == "BB"
        assert result.third == "CCC"

    def test_set_first_string_shrink(self):
        obj = MultiString(first="AAAAAAAAAA", second="BB", third="CCC")
        mv = MultiString.mut_view(obj.pack())
        mv.first = "A"
        result = MultiString.unpack(mv.to_bytes())
        assert result.first == "A"
        assert result.second == "BB"
        assert result.third == "CCC"

    def test_set_middle_string(self):
        obj = MultiString(first="AAA", second="BBB", third="CCC")
        mv = MultiString.mut_view(obj.pack())
        mv.second = "BBBBBBBBBB"
        result = MultiString.unpack(mv.to_bytes())
        assert result.first == "AAA"
        assert result.second == "BBBBBBBBBB"
        assert result.third == "CCC"

    def test_set_last_string(self):
        obj = MultiString(first="AAA", second="BBB", third="CCC")
        mv = MultiString.mut_view(obj.pack())
        mv.third = "CCCCCCCCCC"
        result = MultiString.unpack(mv.to_bytes())
        assert result.first == "AAA"
        assert result.second == "BBB"
        assert result.third == "CCCCCCCCCC"

    def test_set_all_strings(self):
        obj = MultiString(first="A", second="B", third="C")
        mv = MultiString.mut_view(obj.pack())
        mv.first = "FIRST"
        mv.second = "SECOND"
        mv.third = "THIRD"
        result = MultiString.unpack(mv.to_bytes())
        assert result.first == "FIRST"
        assert result.second == "SECOND"
        assert result.third == "THIRD"


# ── Optional fields ────────────────────────────────────────────────────────

class TestOptionalWrites:
    def test_set_optional_string_to_none(self):
        obj = WithOptional(name="Alice", bio="Developer", score=100)
        mv = WithOptional.mut_view(obj.pack())
        mv.bio = None
        result = WithOptional.unpack(mv.to_bytes())
        assert result.name == "Alice"
        assert result.bio is None
        assert result.score == 100

    def test_set_optional_from_none_to_value(self):
        obj = WithOptional(name="Alice", bio=None, score=None)
        mv = WithOptional.mut_view(obj.pack())
        mv.bio = "New bio"
        result = WithOptional.unpack(mv.to_bytes())
        assert result.name == "Alice"
        assert result.bio == "New bio"

    def test_set_optional_u32_to_none(self):
        obj = WithOptional(name="Alice", bio="Dev", score=100)
        mv = WithOptional.mut_view(obj.pack())
        mv.score = None
        result = WithOptional.unpack(mv.to_bytes())
        assert result.score is None
        assert result.name == "Alice"
        assert result.bio == "Dev"

    def test_set_optional_u32_from_none(self):
        obj = WithOptional(name="Alice", bio="Dev", score=None)
        mv = WithOptional.mut_view(obj.pack())
        mv.score = 42
        result = WithOptional.unpack(mv.to_bytes())
        assert result.score == 42

    def test_toggle_optional(self):
        """Set to None, then back to a value."""
        obj = WithOptional(name="Alice", bio="Original", score=50)
        mv = WithOptional.mut_view(obj.pack())
        mv.bio = None
        assert mv.bio is None
        mv.bio = "Updated"
        result = WithOptional.unpack(mv.to_bytes())
        assert result.bio == "Updated"
        assert result.name == "Alice"
        assert result.score == 50


# ── Fast mode ──────────────────────────────────────────────────────────────

class TestFastMode:
    def test_fast_scalar_overwrite(self):
        obj = Simple(x=10, y=20)
        mv = Simple.mut_view(obj.pack(), fast=True)
        mv.x = 99
        result = Simple.unpack(mv.to_bytes())
        assert result.x == 99
        assert result.y == 20

    def test_fast_string_shrink_overwrites(self):
        """Shrinking a string should overwrite in place in fast mode."""
        obj = Person(name="Alexander", age=30, active=True)
        mv = Person.mut_view(obj.pack(), fast=True)
        mv.name = "Al"
        # In fast mode, the result may have dead bytes but reads correctly
        assert mv.name == "Al"
        assert mv.age == 30

    def test_fast_string_grow_appends(self):
        """Growing a string should append in fast mode."""
        obj = Person(name="Al", age=30, active=True)
        mv = Person.mut_view(obj.pack(), fast=True)
        mv.name = "Alexander"
        assert mv.name == "Alexander"
        assert mv.age == 30

    def test_fast_set_none(self):
        obj = WithOptional(name="Alice", bio="Dev", score=100)
        mv = WithOptional.mut_view(obj.pack(), fast=True)
        mv.bio = None
        assert mv.bio is None
        assert mv.name == "Alice"

    def test_fast_compact(self):
        """After fast mutations, compact should produce canonical output."""
        obj = Person(name="Alice", age=30, active=True)
        mv = Person.mut_view(obj.pack(), fast=True)
        mv.name = "Bob"
        mv.age = 25
        mv.compact()
        result = Person.unpack(mv.to_bytes())
        assert result.name == "Bob"
        assert result.age == 25
        assert result.active is True

    def test_fast_compact_matches_fresh_pack(self):
        """After compact, the buffer should match a fresh pack."""
        obj = Person(name="Alice", age=30, active=True)
        mv = Person.mut_view(obj.pack(), fast=True)
        mv.name = "Bob"
        mv.age = 25
        mv.compact()
        expected = Person(name="Bob", age=25, active=True).pack()
        assert mv.to_bytes() == expected


# ── Multiple mutations ─────────────────────────────────────────────────────

class TestMultipleMutations:
    def test_scalar_then_string(self):
        obj = Person(name="Alice", age=30, active=True)
        mv = Person.mut_view(obj.pack())
        mv.age = 99
        mv.name = "Bob"
        result = Person.unpack(mv.to_bytes())
        assert result.name == "Bob"
        assert result.age == 99
        assert result.active is True

    def test_string_then_scalar(self):
        obj = Person(name="Alice", age=30, active=True)
        mv = Person.mut_view(obj.pack())
        mv.name = "Bob"
        mv.age = 99
        result = Person.unpack(mv.to_bytes())
        assert result.name == "Bob"
        assert result.age == 99

    def test_repeated_string_writes(self):
        obj = Person(name="Alice", age=30, active=True)
        mv = Person.mut_view(obj.pack())
        mv.name = "A very long name indeed"
        mv.name = "Short"
        mv.name = "Final"
        result = Person.unpack(mv.to_bytes())
        assert result.name == "Final"
        assert result.age == 30

    def test_mixed_field_writes(self):
        obj = MixedFields(label="test", count=10, flag=False, data=b"\x01\x02")
        mv = MixedFields.mut_view(obj.pack())
        mv.label = "new label"
        mv.count = 42
        mv.flag = True
        mv.data = b"\xff\xfe\xfd"
        result = MixedFields.unpack(mv.to_bytes())
        assert result.label == "new label"
        assert result.count == 42
        assert result.flag is True
        assert result.data == b"\xff\xfe\xfd"


# ── Read-after-write consistency ───────────────────────────────────────────

class TestReadAfterWrite:
    def test_read_scalar_after_write(self):
        obj = Simple(x=10, y=20)
        mv = Simple.mut_view(obj.pack())
        mv.x = 99
        assert mv.x == 99
        assert mv.y == 20

    def test_read_string_after_write(self):
        obj = Person(name="Alice", age=30, active=True)
        mv = Person.mut_view(obj.pack())
        mv.name = "Bob"
        assert mv.name == "Bob"

    def test_read_string_after_grow(self):
        obj = Person(name="Al", age=30, active=True)
        mv = Person.mut_view(obj.pack())
        mv.name = "A very long name"
        assert mv.name == "A very long name"
        assert mv.age == 30

    def test_read_optional_after_set_none(self):
        obj = WithOptional(name="Alice", bio="Dev", score=100)
        mv = WithOptional.mut_view(obj.pack())
        mv.bio = None
        assert mv.bio is None
        assert mv.score == 100

    def test_read_optional_after_set_value(self):
        obj = WithOptional(name="Alice", bio=None, score=None)
        mv = WithOptional.mut_view(obj.pack())
        mv.bio = "Hello"
        assert mv.bio == "Hello"


# ── Canonical round-trip ───────────────────────────────────────────────────

class TestCanonicalRoundTrip:
    def test_mutated_matches_fresh_pack_scalar(self):
        obj = Simple(x=10, y=20)
        mv = Simple.mut_view(obj.pack())
        mv.x = 99
        expected = Simple(x=99, y=20).pack()
        assert mv.to_bytes() == expected

    def test_mutated_matches_fresh_pack_string_same_size(self):
        obj = Person(name="Alice", age=30, active=True)
        mv = Person.mut_view(obj.pack())
        mv.name = "Bobby"
        expected = Person(name="Bobby", age=30, active=True).pack()
        assert mv.to_bytes() == expected

    def test_mutated_matches_fresh_pack_string_grow(self):
        obj = Person(name="Al", age=30, active=True)
        mv = Person.mut_view(obj.pack())
        mv.name = "Alexander"
        expected = Person(name="Alexander", age=30, active=True).pack()
        assert mv.to_bytes() == expected

    def test_mutated_matches_fresh_pack_string_shrink(self):
        obj = Person(name="Alexander", age=30, active=True)
        mv = Person.mut_view(obj.pack())
        mv.name = "Al"
        expected = Person(name="Al", age=30, active=True).pack()
        assert mv.to_bytes() == expected


# ── Edge cases ─────────────────────────────────────────────────────────────

class TestEdgeCases:
    def test_bytes_conversion(self):
        obj = Simple(x=10, y=20)
        mv = Simple.mut_view(obj.pack())
        assert bytes(mv) == obj.pack()

    def test_repr(self):
        obj = Simple(x=10, y=20)
        mv = Simple.mut_view(obj.pack())
        r = repr(mv)
        assert "Simple.MutView" in r
        assert "x=10" in r
        assert "canonical" in r

    def test_requires_bytearray(self):
        obj = Simple(x=10, y=20)
        data = obj.pack()
        # Direct construction with bytes should fail
        from psio._mutation import make_mut_view_class
        MutCls = make_mut_view_class("Simple", Simple.__schema__)
        with pytest.raises(TypeError, match="bytearray"):
            MutCls(data, 0)

    def test_bad_field_read(self):
        obj = Simple(x=10, y=20)
        mv = Simple.mut_view(obj.pack())
        with pytest.raises(AttributeError, match="no field"):
            _ = mv.z

    def test_bad_field_write(self):
        obj = Simple(x=10, y=20)
        mv = Simple.mut_view(obj.pack())
        with pytest.raises(AttributeError, match="no field"):
            mv.z = 42

    def test_unicode_string_mutation(self):
        obj = Person(name="Alice", age=30, active=True)
        mv = Person.mut_view(obj.pack())
        mv.name = "こんにちは"
        result = Person.unpack(mv.to_bytes())
        assert result.name == "こんにちは"
        assert result.age == 30

    def test_mut_view_classmethod(self):
        """The @schema decorator should add a mut_view classmethod."""
        obj = Person(name="Alice", age=30, active=True)
        data = obj.pack()
        mv = Person.mut_view(data)
        assert mv.name == "Alice"
        mv.name = "Bob"
        assert mv.name == "Bob"

    def test_fast_mode_via_classmethod(self):
        obj = Person(name="Alice", age=30, active=True)
        mv = Person.mut_view(obj.pack(), fast=True)
        mv.name = "Bob"
        assert mv.name == "Bob"
        r = repr(mv)
        assert "fast" in r
