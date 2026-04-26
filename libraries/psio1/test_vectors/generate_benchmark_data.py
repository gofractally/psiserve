#!/usr/bin/env python3
"""Generate deterministic benchmark data for all fracpack language benchmarks.

Produces benchmark_data.json with pre-built values at each tier so that
every language implementation encodes the same logical data and can
cross-check wire bytes.

Usage:
    python3 generate_benchmark_data.py
"""

import json
import math
import random
import sys
from pathlib import Path

SEED = 42
OUT = Path(__file__).parent / "benchmark_data.json"

WORDS = [
    "alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta",
    "iota", "kappa", "lambda", "mu", "nu", "xi", "omicron", "pi", "rho",
    "sigma", "tau", "upsilon", "phi", "chi", "psi", "omega",
]

SENTENCES = [
    "The quick brown fox jumps over the lazy dog.",
    "Pack my box with five dozen liquor jugs.",
    "How vexingly quick daft zebras jump.",
    "Sphinx of black quartz, judge my vow.",
    "Two driven jocks help fax my big quiz.",
]

DOMAINS = ["example.com", "test.org", "bench.dev", "perf.io", "data.net"]


def rng_word(r: random.Random) -> str:
    return r.choice(WORDS)


def rng_sentence(r: random.Random) -> str:
    return r.choice(SENTENCES)


def rng_name(r: random.Random) -> str:
    return f"{rng_word(r).capitalize()} {rng_word(r).capitalize()}"


def rng_email(r: random.Random, name: str) -> str:
    parts = name.lower().split()
    return f"{parts[0]}.{parts[1]}@{r.choice(DOMAINS)}"


def rng_tags(r: random.Random, n: int) -> list[str]:
    return [rng_word(r) for _ in range(n)]


# ── Tier 1: Micro ───────────────────────────────────────────────────────

def make_point(r: random.Random) -> dict:
    return {"x": round(r.uniform(-1000, 1000), 6), "y": round(r.uniform(-1000, 1000), 6)}


def make_rgba(r: random.Random) -> dict:
    return {"r": r.randint(0, 255), "g": r.randint(0, 255),
            "b": r.randint(0, 255), "a": r.randint(0, 255)}


# ── Tier 2: Small ───────────────────────────────────────────────────────

def make_token(r: random.Random) -> dict:
    return {
        "kind": r.randint(0, 127),
        "offset": r.randint(0, 100000),
        "length": r.randint(1, 200),
        "text": rng_word(r) * r.randint(1, 4),
    }


# ── Tier 3: Medium ──────────────────────────────────────────────────────

def make_user_profile(r: random.Random) -> dict:
    name = rng_name(r)
    return {
        "id": str(r.randint(1, 2**63)),
        "name": name,
        "email": rng_email(r, name),
        "bio": rng_sentence(r) if r.random() > 0.3 else None,
        "age": r.randint(18, 90),
        "score": round(r.uniform(0, 100), 2),
        "tags": rng_tags(r, r.randint(1, 5)),
        "verified": r.random() > 0.5,
    }


# ── Tier 4: Nested ──────────────────────────────────────────────────────

def make_line_item(r: random.Random) -> dict:
    return {
        "product": f"{rng_word(r)} {rng_word(r)}",
        "qty": r.randint(1, 100),
        "unit_price": round(r.uniform(0.99, 999.99), 2),
    }


def make_order(r: random.Random) -> dict:
    n_items = r.randint(1, 10)
    items = [make_line_item(r) for _ in range(n_items)]
    total = round(sum(i["qty"] * i["unit_price"] for i in items), 2)
    status_tag = r.choice(["pending", "shipped", "delivered", "cancelled"])
    if status_tag == "pending":
        status = {"pending": 0}
    elif status_tag == "shipped":
        status = {"shipped": f"TRACK-{r.randint(1000, 9999)}"}
    elif status_tag == "delivered":
        status = {"delivered": str(r.randint(1, 2**63))}
    else:
        status = {"cancelled": rng_sentence(r)}
    return {
        "id": str(r.randint(1, 2**63)),
        "customer": make_user_profile(r),
        "items": items,
        "total": total,
        "note": rng_sentence(r) if r.random() > 0.5 else None,
        "status": status,
    }


# ── Tier 5: Wide ────────────────────────────────────────────────────────

def make_sensor_reading(r: random.Random) -> dict:
    return {
        "timestamp": str(r.randint(1700000000000, 1800000000000)),
        "device_id": f"sensor-{r.randint(1, 9999):04d}",
        "temp": round(r.uniform(-40, 85), 4),
        "humidity": round(r.uniform(0, 100), 4),
        "pressure": round(r.uniform(950, 1050), 4),
        "accel_x": round(r.gauss(0, 2), 6),
        "accel_y": round(r.gauss(0, 2), 6),
        "accel_z": round(r.gauss(0, 2) + 9.8, 6),
        "gyro_x": round(r.gauss(0, 0.1), 6),
        "gyro_y": round(r.gauss(0, 0.1), 6),
        "gyro_z": round(r.gauss(0, 0.1), 6),
        "mag_x": round(r.gauss(25, 5), 4),
        "mag_y": round(r.gauss(0, 5), 4),
        "mag_z": round(r.gauss(-40, 5), 4),
        "battery": round(r.uniform(2.5, 4.2), 2),
        "signal_dbm": r.randint(-120, -30),
        "error_code": r.randint(1, 255) if r.random() > 0.9 else None,
        "firmware": f"v{r.randint(1,9)}.{r.randint(0,99)}.{r.randint(0,999)}",
    }


# ── Tier 7: Tree ────────────────────────────────────────────────────────

def make_tree(r: random.Random, depth: int) -> dict:
    node = {
        "value": r.randint(0, 10000),
        "label": rng_word(r),
        "children": [],
    }
    if depth > 0:
        node["children"] = [make_tree(r, depth - 1), make_tree(r, depth - 1)]
    return node


# ── Tier 8: System catalog ──────────────────────────────────────────────

def make_func_param(r: random.Random) -> dict:
    return {
        "name": rng_word(r),
        "type_name": r.choice(["u32", "string", "bool", "f64", "vec[u8]"]),
        "doc": f"Parameter {rng_word(r)}" if r.random() > 0.4 else None,
    }


def make_func_def(r: random.Random) -> dict:
    return {
        "name": f"{rng_word(r)}_{rng_word(r)}",
        "params": [make_func_param(r) for _ in range(r.randint(0, 5))],
        "returns": r.choice(["void", "u32", "string", "bool", "Result"]),
        "doc": rng_sentence(r),
    }


def make_type_field(r: random.Random, idx: int) -> dict:
    return {
        "name": rng_word(r),
        "type_name": r.choice(["u32", "i64", "string", "bool", "f64", "optional[string]"]),
        "offset": idx * 4,
        "doc": f"Field {rng_word(r)}" if r.random() > 0.5 else None,
    }


def make_type_def(r: random.Random) -> dict:
    name = f"{rng_word(r).capitalize()}{rng_word(r).capitalize()}"
    kind_tag = r.choice(["struct_", "struct_", "struct_", "enum_", "alias"])
    if kind_tag == "struct_":
        n_fields = r.randint(2, 8)
        kind = {"struct_": [make_type_field(r, i) for i in range(n_fields)]}
    elif kind_tag == "enum_":
        kind = {"enum_": [rng_word(r).capitalize() for _ in range(r.randint(2, 6))]}
    else:
        kind = {"alias": r.choice(["u64", "string", "vec[u8]"])}
    return {
        "name": name,
        "kind": kind,
        "doc": rng_sentence(r),
    }


def make_module_def(r: random.Random, n_types: int, n_funcs: int) -> dict:
    return {
        "name": f"{rng_word(r)}_{rng_word(r)}",
        "types": [make_type_def(r) for _ in range(n_types)],
        "functions": [make_func_def(r) for _ in range(n_funcs)],
        "doc": rng_sentence(r),
    }


def make_system_catalog(r: random.Random, n_modules: int,
                        types_per: int, funcs_per: int) -> dict:
    return {
        "version": 1,
        "modules": [make_module_def(r, types_per, funcs_per) for _ in range(n_modules)],
        "metadata": [f"author:{rng_name(r)}", f"license:MIT", f"url:https://{r.choice(DOMAINS)}/docs"],
    }


# ── Array builders ──────────────────────────────────────────────────────

def make_point_cloud(r: random.Random, n: int) -> dict:
    return {"points": [make_point(r) for _ in range(n)]}


def make_token_stream(r: random.Random, n: int) -> dict:
    return {"tokens": [make_token(r) for _ in range(n)]}


def make_user_batch(r: random.Random, n: int) -> dict:
    return {"users": [make_user_profile(r) for _ in range(n)]}


# ── Main ────────────────────────────────────────────────────────────────

def main():
    r = random.Random(SEED)

    data = {
        "format_version": 1,
        "description": "Deterministic benchmark data (seed=42) for cross-language benchmarks",

        # Tier 1: single instances
        "point": make_point(r),
        "rgba": make_rgba(r),

        # Tier 2: single instance
        "token": make_token(r),

        # Tier 3: single instance
        "user_profile": make_user_profile(r),

        # Tier 4: single instance (with 5 line items)
        "order": make_order(r),

        # Tier 5: single instance
        "sensor_reading": make_sensor_reading(r),

        # Tier 6: arrays at various sizes
        "point_cloud_10": make_point_cloud(random.Random(SEED + 1), 10),
        "point_cloud_1k": make_point_cloud(random.Random(SEED + 2), 1_000),
        "point_cloud_100k": make_point_cloud(random.Random(SEED + 3), 100_000),
        "token_stream_1k": make_token_stream(random.Random(SEED + 4), 1_000),
        "token_stream_10k": make_token_stream(random.Random(SEED + 5), 10_000),
        "user_batch_10": make_user_batch(random.Random(SEED + 6), 10),
        "user_batch_1k": make_user_batch(random.Random(SEED + 7), 1_000),

        # Tier 7: tree
        "tree_depth_5": make_tree(random.Random(SEED + 8), 5),
        "tree_depth_10": make_tree(random.Random(SEED + 9), 10),

        # Tier 8: system catalog
        "system_catalog": make_system_catalog(random.Random(SEED + 10), 10, 20, 5),

        # Mutation test data (specific values for load-modify-store benchmarks)
        "mutation_targets": {
            "user_short_name": "Al",
            "user_long_name": "Alexander Bartholomew Christopherson III",
            "user_new_bio": "A completely new biography that is longer than before.",
            "order_new_product": "quantum entangled widget deluxe",
            "catalog_new_doc": "Updated documentation for the module after review.",
        },
    }

    with open(OUT, "w") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)

    # Print summary
    sizes = {
        "point": 1,
        "rgba": 1,
        "token": 1,
        "user_profile": 1,
        "order": "nested",
        "sensor_reading": 1,
        "point_cloud_10": 10,
        "point_cloud_1k": "1,000",
        "point_cloud_100k": "100,000",
        "token_stream_1k": "1,000",
        "token_stream_10k": "10,000",
        "user_batch_10": 10,
        "user_batch_1k": "1,000",
        "tree_depth_5": "31 nodes",
        "tree_depth_10": "1,023 nodes",
        "system_catalog": "10 modules x 20 types x 5 funcs",
    }
    print(f"Generated {OUT}")
    print(f"File size: {OUT.stat().st_size:,} bytes")
    for k, v in sizes.items():
        print(f"  {k}: {v}")


if __name__ == "__main__":
    main()
