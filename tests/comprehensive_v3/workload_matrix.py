"""Explicit SQL text encodings and FP32 truth for local performance comparisons.

These are diagnostic workloads, not a copy of the platform's hidden generator.
The short-decimal case keeps the original five-decimal SQL text. The long case
deliberately emits Python's representation of the resulting float32 values.
"""
import hashlib
import json
import random
import struct

ROWS, DIMENSION, QUERIES = 10000, 128, 1000
CREATE = "CREATE TABLE vectors (id BIGINT PRIMARY KEY, bucket BIGINT, v VECTOR(128));"
CASES = ("grid", "fp32-long", "decimal-short", "scientific")


def fp32(value):
    return struct.unpack("<f", struct.pack("<f", float(value)))[0]


def dataset(seed, case):
    if case not in CASES:
        raise ValueError(case)
    rng = random.Random(seed)
    buckets = [rng.randrange(16) for _ in range(ROWS)]

    def vector():
        if case == "grid":
            values = [rng.randrange(-32, 33) / 8 for _ in range(DIMENSION)]
            tokens = [repr(v) for v in values]
        elif case == "decimal-short":
            tokens = [f"{rng.uniform(-4, 4):.5f}" for _ in range(DIMENSION)]
            values = [fp32(v) for v in tokens]
        else:
            values = [fp32(rng.uniform(-4, 4)) for _ in range(DIMENSION)]
            tokens = [repr(v) if case == "fp32-long" else f"{v:.9e}" for v in values]
        assert all(fp32(t) == v for t, v in zip(tokens, values))
        return values, "'[" + ",".join(tokens) + "]'"

    pairs = [vector() for _ in range(ROWS)]
    vectors, literals = zip(*pairs)
    queries, statements = [], []
    for i in range(QUERIES):
        bucket = rng.randrange(16) if i % 2 else -1
        values, literal = vector()
        queries.append((bucket, values))
        where = f" WHERE bucket={bucket}" if bucket >= 0 else ""
        statements.append(f"SELECT id,L2_DISTANCE(v,{literal}) AS d FROM vectors{where} "
                          "ORDER BY d LIMIT 10;")
    inserts = ["INSERT INTO vectors VALUES " + ",".join(
        f"({key},{buckets[key]},{literals[key]})" for key in range(start, start + 100))
        for start in range(0, ROWS, 100)]
    return buckets, list(vectors), queries, inserts, statements


def fingerprint(inserts, statements):
    return hashlib.sha256(json.dumps([inserts, statements], separators=(",", ":")).encode()).hexdigest()
