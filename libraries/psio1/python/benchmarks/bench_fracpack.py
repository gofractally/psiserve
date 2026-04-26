"""Comprehensive fracpack benchmarks for the psio Python library.

Covers pack, unpack, view, validate, JSON, mutation, competitor comparison,
and array scaling -- matching the design in doc/benchmark-design.md.

Run with:
    cd libraries/psio1/python
    .venv/bin/python -m pytest benchmarks/bench_fracpack.py -v --benchmark-sort=mean
"""

from __future__ import annotations

import json

import pytest

from psio import schema, types as t


# ============================================================================
# Schema definitions (matching benchmark_schemas.json)
# ============================================================================

@schema
class Point:
    class Meta:
        fixed = True
    x: t.f64
    y: t.f64


@schema
class RGBA:
    class Meta:
        fixed = True
    r: t.u8
    g: t.u8
    b: t.u8
    a: t.u8


@schema
class Token:
    kind: t.u16
    offset: t.u32
    length: t.u32
    text: t.string


@schema
class UserProfile:
    id: t.u64
    name: t.string
    email: t.string
    bio: t.optional[t.string]
    age: t.u32
    score: t.f64
    tags: t.vec[t.string]
    verified: t.bool_


@schema
class LineItem:
    product: t.string
    qty: t.u32
    unit_price: t.f64


@schema
class Order:
    id: t.u64
    customer: UserProfile
    items: t.vec[LineItem]
    total: t.f64
    note: t.optional[t.string]
    status: t.variant(pending=t.u32, shipped=t.string,
                       delivered=t.u64, cancelled=t.string)


@schema
class SensorReading:
    timestamp: t.u64
    device_id: t.string
    temp: t.f64
    humidity: t.f64
    pressure: t.f64
    accel_x: t.f64
    accel_y: t.f64
    accel_z: t.f64
    gyro_x: t.f64
    gyro_y: t.f64
    gyro_z: t.f64
    mag_x: t.f64
    mag_y: t.f64
    mag_z: t.f64
    battery: t.f32
    signal_dbm: t.i16
    error_code: t.optional[t.u32]
    firmware: t.string


# ============================================================================
# Fixture instances (reused across benchmarks)
# ============================================================================

POINT = Point(x=1.5, y=2.5)
RGBA_VAL = RGBA(r=255, g=128, b=64, a=200)

TOKEN = Token(kind=42, offset=100, length=15, text="hello_world")

USER = UserProfile(
    id=12345,
    name="Alice Beta",
    email="alice@example.com",
    bio="A short biography for benchmarking purposes.",
    age=30,
    score=98.6,
    tags=["engineer", "python", "fracpack"],
    verified=True,
)

LINEITEM_A = LineItem(product="Widget Alpha", qty=5, unit_price=19.99)
LINEITEM_B = LineItem(product="Gadget Beta", qty=2, unit_price=49.95)

ORDER = Order(
    id=99001,
    customer=USER,
    items=[LINEITEM_A, LINEITEM_B],
    total=199.85,
    note="Rush delivery please",
    status={"type": "shipped", "value": "UPS-12345"},
)

SENSOR = SensorReading(
    timestamp=1700000000000,
    device_id="sensor-42-alpha",
    temp=22.5,
    humidity=45.3,
    pressure=1013.25,
    accel_x=0.01,
    accel_y=-0.02,
    accel_z=9.81,
    gyro_x=0.001,
    gyro_y=0.002,
    gyro_z=-0.003,
    mag_x=25.0,
    mag_y=-12.5,
    mag_z=42.0,
    battery=3.7,
    signal_dbm=-65,
    error_code=None,
    firmware="v2.1.0",
)

# Pre-packed data (computed once)
POINT_DATA = POINT.pack()
RGBA_DATA = RGBA_VAL.pack()
TOKEN_DATA = TOKEN.pack()
USER_DATA = USER.pack()
ORDER_DATA = ORDER.pack()
SENSOR_DATA = SENSOR.pack()


# ============================================================================
# 1. Pack benchmarks
# ============================================================================

@pytest.mark.benchmark(group="pack")
class TestPackBenchmarks:

    def test_pack_point(self, benchmark):
        p = Point(x=1.5, y=2.5)
        benchmark(p.pack)

    def test_pack_rgba(self, benchmark):
        c = RGBA(r=255, g=128, b=64, a=200)
        benchmark(c.pack)

    def test_pack_token(self, benchmark):
        tok = Token(kind=42, offset=100, length=15, text="hello_world")
        benchmark(tok.pack)

    def test_pack_user_profile(self, benchmark):
        u = UserProfile(
            id=12345, name="Alice Beta", email="alice@example.com",
            bio="A short biography for benchmarking purposes.",
            age=30, score=98.6, tags=["engineer", "python", "fracpack"],
            verified=True,
        )
        benchmark(u.pack)

    def test_pack_order(self, benchmark):
        benchmark(ORDER.pack)

    def test_pack_sensor_reading(self, benchmark):
        benchmark(SENSOR.pack)


# ============================================================================
# 2. Unpack benchmarks
# ============================================================================

@pytest.mark.benchmark(group="unpack")
class TestUnpackBenchmarks:

    def test_unpack_point(self, benchmark):
        benchmark(Point.unpack, POINT_DATA)

    def test_unpack_rgba(self, benchmark):
        benchmark(RGBA.unpack, RGBA_DATA)

    def test_unpack_token(self, benchmark):
        benchmark(Token.unpack, TOKEN_DATA)

    def test_unpack_user_profile(self, benchmark):
        benchmark(UserProfile.unpack, USER_DATA)

    def test_unpack_order(self, benchmark):
        benchmark(Order.unpack, ORDER_DATA)

    def test_unpack_sensor_reading(self, benchmark):
        benchmark(SensorReading.unpack, SENSOR_DATA)


# ============================================================================
# 3. View benchmarks (the fracpack advantage)
# ============================================================================

@pytest.mark.benchmark(group="view")
class TestViewBenchmarks:

    def test_view_point_one_field(self, benchmark):
        data = POINT_DATA
        def access_one():
            v = Point.view(data)
            return v.x
        benchmark(access_one)

    def test_view_user_one_field(self, benchmark):
        """View a single field -- should be much faster than full unpack."""
        data = USER_DATA
        def access_one():
            v = UserProfile.view(data)
            return v.name
        benchmark(access_one)

    def test_view_user_all_fields(self, benchmark):
        """View all fields -- compare overhead to full unpack."""
        data = USER_DATA
        def access_all():
            v = UserProfile.view(data)
            _ = (v.id, v.name, v.email, v.bio, v.age,
                 v.score, v.tags, v.verified)
        benchmark(access_all)

    def test_view_sensor_first_field(self, benchmark):
        """Access first field of wide struct."""
        data = SENSOR_DATA
        def access_first():
            v = SensorReading.view(data)
            return v.timestamp
        benchmark(access_first)

    def test_view_sensor_last_string(self, benchmark):
        """Access last string field (worst-case offset chasing)."""
        data = SENSOR_DATA
        def access_last():
            v = SensorReading.view(data)
            return v.firmware
        benchmark(access_last)

    def test_view_order_nested_field(self, benchmark):
        """Access a field inside a nested struct."""
        data = ORDER_DATA
        def access_nested():
            v = Order.view(data)
            return v.customer
        benchmark(access_nested)

    def test_view_order_customer_name(self, benchmark):
        """Access Order → customer → name via nested view navigation."""
        data = ORDER_DATA
        def access_nested_name():
            v = Order.view(data)
            return v.customer.name
        benchmark(access_nested_name)

    def test_view_point_all_fields(self, benchmark):
        """Access both x and y fields of a Point view."""
        data = POINT_DATA
        def access_all():
            v = Point.view(data)
            _ = v.x
            _ = v.y
        benchmark(access_all)

    def test_unpack_then_access_one(self, benchmark):
        """Baseline: full unpack just to read one field."""
        data = USER_DATA
        def unpack_access():
            obj = UserProfile.unpack(data)
            return obj.name
        benchmark(unpack_access)


# ============================================================================
# 4. Validate benchmarks
# ============================================================================

@pytest.mark.benchmark(group="validate")
class TestValidateBenchmarks:

    def test_validate_point(self, benchmark):
        benchmark(Point.validate, POINT_DATA)

    def test_validate_user_profile(self, benchmark):
        benchmark(UserProfile.validate, USER_DATA)

    def test_validate_order(self, benchmark):
        benchmark(Order.validate, ORDER_DATA)

    def test_validate_sensor_reading(self, benchmark):
        benchmark(SensorReading.validate, SENSOR_DATA)


# ============================================================================
# 5. JSON benchmarks
# ============================================================================

@pytest.mark.benchmark(group="json")
class TestJSONBenchmarks:

    def test_to_json_point(self, benchmark):
        benchmark(POINT.to_json)

    def test_to_json_user(self, benchmark):
        benchmark(USER.to_json)

    def test_to_json_order(self, benchmark):
        benchmark(ORDER.to_json)

    def test_from_json_point(self, benchmark):
        j = POINT.to_json()
        benchmark(Point.from_json, j)

    def test_from_json_user(self, benchmark):
        j = USER.to_json()
        benchmark(UserProfile.from_json, j)

    def test_from_json_order(self, benchmark):
        j = ORDER.to_json()
        benchmark(Order.from_json, j)

    def test_to_json_sensor(self, benchmark):
        benchmark(SENSOR.to_json)

    def test_from_json_sensor(self, benchmark):
        j = SENSOR.to_json()
        benchmark(SensorReading.from_json, j)


# ============================================================================
# 6. Load-Modify-Store (mutation) benchmarks
# ============================================================================

@pytest.mark.benchmark(group="mutate")
class TestMutateBenchmarks:

    def test_mutate_scalar_first(self, benchmark):
        """Modify first fixed field (id) via MutView -- best case."""
        data = USER_DATA
        def mutate():
            mv = UserProfile.mut_view(bytearray(data))
            mv.id = 99999
            return bytes(mv._data)
        benchmark(mutate)

    def test_mutate_scalar_last_wide(self, benchmark):
        """Modify last fixed scalar in wide struct (signal_dbm)."""
        data = SENSOR_DATA
        def mutate():
            mv = SensorReading.mut_view(bytearray(data))
            mv.signal_dbm = -30
            return bytes(mv._data)
        benchmark(mutate)

    def test_mutate_string_same_length(self, benchmark):
        """Modify a string to same length (in-place overwrite in fast mode)."""
        data = USER_DATA
        def mutate():
            mv = UserProfile.mut_view(bytearray(data))
            mv.name = "Bob Charli"  # same length as "Alice Beta"
            return bytes(mv._data)
        benchmark(mutate)

    def test_mutate_string_grow(self, benchmark):
        """Short name to long name -- buffer growth + offset patching."""
        data = USER_DATA
        def mutate():
            mv = UserProfile.mut_view(bytearray(data))
            mv.name = "Alexandria Von Longnamerson III"
            return bytes(mv._data)
        benchmark(mutate)

    def test_mutate_string_shrink(self, benchmark):
        """Long name to short name -- buffer shrink + offset patching."""
        data = USER_DATA
        def mutate():
            mv = UserProfile.mut_view(bytearray(data))
            mv.name = "Bo"
            return bytes(mv._data)
        benchmark(mutate)

    def test_mutate_optional_set(self, benchmark):
        """Set bio from populated to a different value."""
        data = USER_DATA
        def mutate():
            mv = UserProfile.mut_view(bytearray(data))
            mv.bio = "Updated biography with new content."
            return bytes(mv._data)
        benchmark(mutate)

    def test_mutate_optional_clear(self, benchmark):
        """Set bio to None (optional toggle)."""
        data = USER_DATA
        def mutate():
            mv = UserProfile.mut_view(bytearray(data))
            mv.bio = None
            return bytes(mv._data)
        benchmark(mutate)

    def test_mutate_fast_mode_scalar(self, benchmark):
        """Fast-mode scalar mutation (compare to canonical)."""
        data = USER_DATA
        def mutate():
            mv = UserProfile.mut_view(bytearray(data), fast=True)
            mv.id = 99999
            return bytes(mv._data)
        benchmark(mutate)

    def test_mutate_fast_mode_string(self, benchmark):
        """Fast-mode string mutation (append-only, no splice)."""
        data = USER_DATA
        def mutate():
            mv = UserProfile.mut_view(bytearray(data), fast=True)
            mv.name = "Alexandria Von Longnamerson III"
            return bytes(mv._data)
        benchmark(mutate)

    def test_mutate_vs_repack(self, benchmark):
        """Baseline: unpack, modify, repack (naive approach)."""
        data = USER_DATA
        def naive():
            obj = UserProfile.unpack(data)
            obj2 = UserProfile(
                id=99999, name=obj.name, email=obj.email,
                bio=obj.bio, age=obj.age, score=obj.score,
                tags=obj.tags, verified=obj.verified,
            )
            return obj2.pack()
        benchmark(naive)


# ============================================================================
# 7. Roundtrip benchmarks
# ============================================================================

@pytest.mark.benchmark(group="roundtrip")
class TestRoundtripBenchmarks:

    def test_roundtrip_point(self, benchmark):
        def rt():
            return Point.unpack(POINT.pack()).pack()
        benchmark(rt)

    def test_roundtrip_user(self, benchmark):
        def rt():
            return UserProfile.unpack(USER.pack()).pack()
        benchmark(rt)

    def test_roundtrip_order(self, benchmark):
        def rt():
            return Order.unpack(ORDER.pack()).pack()
        benchmark(rt)


# ============================================================================
# 8. Competitor comparison: stdlib json
# ============================================================================

USER_DICT = {
    "id": 12345,
    "name": "Alice Beta",
    "email": "alice@example.com",
    "bio": "A short biography for benchmarking purposes.",
    "age": 30,
    "score": 98.6,
    "tags": ["engineer", "python", "fracpack"],
    "verified": True,
}

ORDER_DICT = {
    "id": 99001,
    "customer": USER_DICT,
    "items": [
        {"product": "Widget Alpha", "qty": 5, "unit_price": 19.99},
        {"product": "Gadget Beta", "qty": 2, "unit_price": 49.95},
    ],
    "total": 199.85,
    "note": "Rush delivery please",
    "status": {"shipped": "UPS-12345"},
}


@pytest.mark.benchmark(group="competitor_json")
class TestCompetitorJSON:

    def test_json_dumps_user(self, benchmark):
        benchmark(json.dumps, USER_DICT)

    def test_json_loads_user(self, benchmark):
        s = json.dumps(USER_DICT)
        benchmark(json.loads, s)

    def test_json_dumps_order(self, benchmark):
        benchmark(json.dumps, ORDER_DICT)

    def test_json_loads_order(self, benchmark):
        s = json.dumps(ORDER_DICT)
        benchmark(json.loads, s)

    def test_fracpack_pack_user(self, benchmark):
        """Direct comparison: fracpack pack vs json.dumps for same data."""
        benchmark(USER.pack)

    def test_fracpack_unpack_user(self, benchmark):
        """Direct comparison: fracpack unpack vs json.loads for same data."""
        benchmark(UserProfile.unpack, USER_DATA)

    def test_fracpack_pack_order(self, benchmark):
        benchmark(ORDER.pack)

    def test_fracpack_unpack_order(self, benchmark):
        benchmark(Order.unpack, ORDER_DATA)


# ============================================================================
# 9. Competitor comparison: msgpack (optional)
# ============================================================================

try:
    import msgpack

    @pytest.mark.benchmark(group="competitor_msgpack")
    class TestCompetitorMsgpack:

        def test_msgpack_pack_user(self, benchmark):
            benchmark(msgpack.packb, USER_DICT)

        def test_msgpack_unpack_user(self, benchmark):
            data = msgpack.packb(USER_DICT)
            benchmark(msgpack.unpackb, data)

        def test_msgpack_pack_order(self, benchmark):
            benchmark(msgpack.packb, ORDER_DICT)

        def test_msgpack_unpack_order(self, benchmark):
            data = msgpack.packb(ORDER_DICT)
            benchmark(msgpack.unpackb, data)

except ImportError:
    pass


# ============================================================================
# 10. Array scaling benchmarks
# ============================================================================

@pytest.mark.benchmark(group="array_scaling")
class TestArrayScaling:

    @pytest.mark.parametrize("n", [10, 100, 1000, 10000])
    def test_pack_points(self, benchmark, n):
        """Pack a list of n Points via raw codec."""
        from psio._codec import pack as raw_pack
        ft = t.extract_frac_type(t.vec[Point])
        points = [Point(x=float(i), y=float(i * 2)) for i in range(n)]
        benchmark(raw_pack, ft, points)

    @pytest.mark.parametrize("n", [10, 100, 1000, 10000])
    def test_unpack_points(self, benchmark, n):
        """Unpack a list of n Points via raw codec."""
        from psio._codec import pack as raw_pack, unpack as raw_unpack
        ft = t.extract_frac_type(t.vec[Point])
        points = [Point(x=float(i), y=float(i * 2)) for i in range(n)]
        data = raw_pack(ft, points)
        benchmark(raw_unpack, ft, data)

    @pytest.mark.parametrize("n", [10, 100, 1000])
    def test_pack_tokens(self, benchmark, n):
        """Pack a list of n Tokens via raw codec."""
        from psio._codec import pack as raw_pack
        ft = t.extract_frac_type(t.vec[Token])
        tokens = [Token(kind=i % 100, offset=i * 10, length=5,
                        text=f"tok_{i}") for i in range(n)]
        benchmark(raw_pack, ft, tokens)

    @pytest.mark.parametrize("n", [10, 100, 1000])
    def test_unpack_tokens(self, benchmark, n):
        """Unpack a list of n Tokens via raw codec."""
        from psio._codec import pack as raw_pack, unpack as raw_unpack
        ft = t.extract_frac_type(t.vec[Token])
        tokens = [Token(kind=i % 100, offset=i * 10, length=5,
                        text=f"tok_{i}") for i in range(n)]
        data = raw_pack(ft, tokens)
        benchmark(raw_unpack, ft, data)


# ============================================================================
# 11. View vs Native object access
# ============================================================================

@pytest.mark.benchmark(group="view_vs_native")
class TestViewVsNative:
    """Compare: native dataclass field access vs view field access vs unpack+access."""

    # ── Point (fixed-size, should be nearly equal) ──

    def test_point_native(self, benchmark):
        """Access all fields on a plain dataclass instance."""
        p = Point(x=1.5, y=2.5)
        def access():
            _ = p.x
            _ = p.y
        benchmark(access)

    def test_point_view(self, benchmark):
        """Access all fields on a view over packed bytes."""
        data = Point(x=1.5, y=2.5).pack()
        def access():
            v = Point.view(data)
            _ = v.x
            _ = v.y
        benchmark(access)

    def test_point_unpack_then_access(self, benchmark):
        """Full unpack then access all fields (worst case)."""
        data = Point(x=1.5, y=2.5).pack()
        def access():
            p = Point.unpack(data)
            _ = p.x
            _ = p.y
        benchmark(access)

    # ── UserProfile (complex, strings + vec + optional) ──

    def test_user_native(self, benchmark):
        """Access all fields on a plain dataclass instance."""
        u = UserProfile(id=12345, name="Alice Beta", email="alice@test.com",
                        bio="A bio", age=30, score=95.5,
                        tags=["go", "perf"], verified=True)
        def access():
            _ = u.id
            _ = u.name
            _ = u.email
            _ = u.bio
            _ = u.age
            _ = u.score
            _ = u.tags
            _ = u.verified
        benchmark(access)

    def test_user_view(self, benchmark):
        """Access all fields via zero-copy view (raw strings, has_ for optional, len for vec)."""
        u = UserProfile(id=12345, name="Alice Beta", email="alice@test.com",
                        bio="A bio", age=30, score=95.5,
                        tags=["go", "perf"], verified=True)
        data = u.pack()
        def access():
            v = UserProfile.view(data)
            _ = v.id
            _ = v.name_raw       # memoryview slice, no decode
            _ = v.email_raw      # memoryview slice, no decode
            _ = v.has_bio        # offset check only, no materialization
            _ = v.age
            _ = v.score
            _ = v.tags_len       # element count, no list allocation
            _ = v.verified
        benchmark(access)

    def test_user_view_decoded(self, benchmark):
        """Access all fields via view with full decode (strings, optional, vec)."""
        u = UserProfile(id=12345, name="Alice Beta", email="alice@test.com",
                        bio="A bio", age=30, score=95.5,
                        tags=["go", "perf"], verified=True)
        data = u.pack()
        def access():
            v = UserProfile.view(data)
            _ = v.id
            _ = v.name
            _ = v.email
            _ = v.bio
            _ = v.age
            _ = v.score
            _ = v.tags
            _ = v.verified
        benchmark(access)

    def test_user_unpack_then_access(self, benchmark):
        """Full unpack then access all fields."""
        u = UserProfile(id=12345, name="Alice Beta", email="alice@test.com",
                        bio="A bio", age=30, score=95.5,
                        tags=["go", "perf"], verified=True)
        data = u.pack()
        def access():
            obj = UserProfile.unpack(data)
            _ = obj.id
            _ = obj.name
            _ = obj.email
            _ = obj.bio
            _ = obj.age
            _ = obj.score
            _ = obj.tags
            _ = obj.verified
        benchmark(access)

    # ── SensorReading (wide, 18 fields) ──

    def test_sensor_native(self, benchmark):
        """Access all 18 fields on a plain dataclass."""
        s = SensorReading(timestamp=1700000000000, device_id="sensor-0001",
                          temp=22.5, humidity=45.0, pressure=1013.25,
                          accel_x=0.1, accel_y=-0.2, accel_z=9.8,
                          gyro_x=0.01, gyro_y=-0.01, gyro_z=0.0,
                          mag_x=25.0, mag_y=0.5, mag_z=-40.0,
                          battery=3.7, signal_dbm=-85,
                          error_code=None, firmware="v2.1.0")
        def access():
            _ = s.timestamp; _ = s.device_id; _ = s.temp; _ = s.humidity
            _ = s.pressure; _ = s.accel_x; _ = s.accel_y; _ = s.accel_z
            _ = s.gyro_x; _ = s.gyro_y; _ = s.gyro_z
            _ = s.mag_x; _ = s.mag_y; _ = s.mag_z
            _ = s.battery; _ = s.signal_dbm; _ = s.error_code; _ = s.firmware
        benchmark(access)

    def test_sensor_view(self, benchmark):
        """Access all 18 fields via zero-copy view (raw strings, has_ for optional)."""
        s = SensorReading(timestamp=1700000000000, device_id="sensor-0001",
                          temp=22.5, humidity=45.0, pressure=1013.25,
                          accel_x=0.1, accel_y=-0.2, accel_z=9.8,
                          gyro_x=0.01, gyro_y=-0.01, gyro_z=0.0,
                          mag_x=25.0, mag_y=0.5, mag_z=-40.0,
                          battery=3.7, signal_dbm=-85,
                          error_code=None, firmware="v2.1.0")
        data = s.pack()
        def access():
            v = SensorReading.view(data)
            _ = v.timestamp; _ = v.device_id_raw; _ = v.temp; _ = v.humidity
            _ = v.pressure; _ = v.accel_x; _ = v.accel_y; _ = v.accel_z
            _ = v.gyro_x; _ = v.gyro_y; _ = v.gyro_z
            _ = v.mag_x; _ = v.mag_y; _ = v.mag_z
            _ = v.battery; _ = v.signal_dbm; _ = v.has_error_code; _ = v.firmware_raw
        benchmark(access)

    def test_sensor_view_decoded(self, benchmark):
        """Access all 18 fields via view with full decode."""
        s = SensorReading(timestamp=1700000000000, device_id="sensor-0001",
                          temp=22.5, humidity=45.0, pressure=1013.25,
                          accel_x=0.1, accel_y=-0.2, accel_z=9.8,
                          gyro_x=0.01, gyro_y=-0.01, gyro_z=0.0,
                          mag_x=25.0, mag_y=0.5, mag_z=-40.0,
                          battery=3.7, signal_dbm=-85,
                          error_code=None, firmware="v2.1.0")
        data = s.pack()
        def access():
            v = SensorReading.view(data)
            _ = v.timestamp; _ = v.device_id; _ = v.temp; _ = v.humidity
            _ = v.pressure; _ = v.accel_x; _ = v.accel_y; _ = v.accel_z
            _ = v.gyro_x; _ = v.gyro_y; _ = v.gyro_z
            _ = v.mag_x; _ = v.mag_y; _ = v.mag_z
            _ = v.battery; _ = v.signal_dbm; _ = v.error_code; _ = v.firmware
        benchmark(access)

    def test_sensor_unpack_then_access(self, benchmark):
        """Full unpack then access all 18 fields."""
        s = SensorReading(timestamp=1700000000000, device_id="sensor-0001",
                          temp=22.5, humidity=45.0, pressure=1013.25,
                          accel_x=0.1, accel_y=-0.2, accel_z=9.8,
                          gyro_x=0.01, gyro_y=-0.01, gyro_z=0.0,
                          mag_x=25.0, mag_y=0.5, mag_z=-40.0,
                          battery=3.7, signal_dbm=-85,
                          error_code=None, firmware="v2.1.0")
        data = s.pack()
        def access():
            obj = SensorReading.unpack(data)
            _ = obj.timestamp; _ = obj.device_id; _ = obj.temp; _ = obj.humidity
            _ = obj.pressure; _ = obj.accel_x; _ = obj.accel_y; _ = obj.accel_z
            _ = obj.gyro_x; _ = obj.gyro_y; _ = obj.gyro_z
            _ = obj.mag_x; _ = obj.mag_y; _ = obj.mag_z
            _ = obj.battery; _ = obj.signal_dbm; _ = obj.error_code; _ = obj.firmware
        benchmark(access)

    # ── Summary (informational) ──

    def test_summary(self, benchmark):
        """Just prints the expected ratios for reference."""
        # This is informational, not a real benchmark
        benchmark(lambda: None)


# ============================================================================
# 12. Encoded size report (not timed -- informational)
# ============================================================================

class TestEncodedSizes:
    """Non-benchmark tests that report encoded sizes for reference."""

    def test_report_sizes(self):
        sizes = {
            "Point": len(POINT_DATA),
            "RGBA": len(RGBA_DATA),
            "Token": len(TOKEN_DATA),
            "UserProfile": len(USER_DATA),
            "Order": len(ORDER_DATA),
            "SensorReading": len(SENSOR_DATA),
            "UserProfile.json": len(json.dumps(USER_DICT)),
            "Order.json": len(json.dumps(ORDER_DICT)),
        }
        for name, size in sizes.items():
            print(f"  {name}: {size} bytes")
        # Smoke check: fracpack should be smaller than JSON for structured data
        assert sizes["UserProfile"] < sizes["UserProfile.json"]
