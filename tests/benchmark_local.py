"""Paired development benchmark with an independent oracle and cgroup v2 counters."""

import argparse
from contextlib import contextmanager
import hashlib
import json
import math
from pathlib import Path
import random
from statistics import median
import struct
import subprocess
from time import perf_counter
from types import SimpleNamespace
import uuid

from check_core import Server


ROWS = 10000
DIMENSION = 128
QUERIES = 1000
MEMORY = 64 * 1024 * 1024
CREATE = "CREATE TABLE vectors (id BIGINT PRIMARY KEY, bucket BIGINT, v VECTOR(128));"


def docker(*arguments, input=None):
    return subprocess.run(["docker", *arguments], input=input, check=True, stdout=subprocess.PIPE,
                          timeout=45).stdout


@contextmanager
def fresh_volume():
    name = "mvdb-bench-" + uuid.uuid4().hex[:16]
    docker("volume", "create", name)
    try:
        yield name
    finally:
        docker("volume", "rm", name)


def dataset(seed):
    rng = random.Random(seed)
    buckets = [rng.randrange(16) for _ in range(ROWS)]
    vectors = [[rng.randrange(-32, 33) / 8 for _ in range(DIMENSION)] for _ in range(ROWS)]
    queries = [(rng.randrange(16) if i % 2 else -1,
                [rng.randrange(-32, 33) / 8 for _ in range(DIMENSION)]) for i in range(QUERIES)]
    return buckets, vectors, queries


def vector_literal(vector):
    return json.dumps(vector, separators=(",", ":"))


def input_statements(buckets, vectors, queries):
    inserts = []
    for start in range(0, ROWS, 100):
        inserts.append("INSERT INTO vectors VALUES " + ",".join(
            f"({key},{buckets[key]},{vector_literal(vectors[key])})" for key in range(start, start + 100)))
    statements = []
    for bucket, query in queries:
        where = f" WHERE bucket={bucket}" if bucket >= 0 else ""
        statements.append(f"SELECT id,L2_DISTANCE(v,{vector_literal(query)}) AS d FROM vectors{where} "
                          "ORDER BY d LIMIT 10;")
    return inserts, statements


def exact_truth(image, helper, buckets, vectors, queries):
    packed = bytearray(struct.pack("<III", ROWS, DIMENSION, QUERIES))
    packed.extend(struct.pack(f"<{ROWS}i", *buckets))
    for vector in vectors:
        packed.extend(struct.pack(f"<{DIMENSION}f", *vector))
    for bucket, query in queries:
        packed.extend(struct.pack(f"<i{DIMENSION}f", bucket, *query))
    output = docker("run", "--rm", "-i", "--network=none", "--cpus=2",
                    "--mount", f"type=bind,source={helper},target=/bench-probe,readonly",
                    "--entrypoint", "/bench-probe", image, "oracle", input=bytes(packed))
    rows = [[int(item) for item in line.split()] for line in output.decode().splitlines()]
    assert len(rows) == QUERIES and all(len(row) == 10 for row in rows)
    return rows, hashlib.sha256(packed).hexdigest()


def resources(db):
    result = docker("exec", db.container, "/bench-probe", "probe").decode().splitlines()
    device, peak, limit, swap, journal = result[:5]
    devices = {}
    for line in result[5:]:
        fields = line.split()
        devices[fields[0]] = {key: int(value) for key, value in
                              (field.split("=", 1) for field in fields[1:])}
    counters = devices.get(device)
    physical_bytes = None
    if counters is not None and "rbytes" in counters and "wbytes" in counters:
        physical_bytes = counters["rbytes"] + counters["wbytes"]
    assert int(limit) == MEMORY and swap == "0" and 0 < int(peak) <= MEMORY
    assert int(journal) > 0
    return {"data_device": device, "io_stat": devices, "io_bytes": physical_bytes,
            "memory_peak_bytes": int(peak), "memory_limit_bytes": int(limit),
            "swap_limit_bytes": int(swap), "journal_bytes": int(journal)}


def run_write(image, helper, inserts, buckets, vectors):
    args = SimpleNamespace(image=image, binary=None)
    with fresh_volume() as volume:
        with Server(args, None, MEMORY, volume=volume, probe=helper) as db:
            db.ok(CREATE)
            begin = perf_counter()
            for insert in inserts:
                db.ok(insert, 100)
            seconds = perf_counter() - begin
            measured = resources(db)
        with Server(args, None, MEMORY, volume=volume) as db:
            rows, count, error = db.sql("SELECT id,bucket,v FROM vectors ORDER BY id;")
            assert count == len(rows) == ROWS and error is None
            assert all(row == [i, buckets[i], vectors[i]] for i, row in enumerate(rows))
    return {"seconds": seconds, "durability_and_content_passed": True, **measured}


def verify_queries(responses, truth, buckets, vectors, queries, mode):
    recalls = {"filtered": [], "unfiltered": []}
    for response, expected, (bucket, query) in zip(responses, truth, queries):
        rows, count, error = response
        assert count == len(expected) == len(rows) and error is None
        ids = [row[0] for row in rows]
        assert len(set(ids)) == len(ids) and all(isinstance(key, int) and 0 <= key < ROWS for key in ids)
        assert bucket < 0 or all(buckets[key] == bucket for key in ids)
        assert rows == sorted(rows, key=lambda row: (row[1], row[0]))
        for key, distance in rows:
            real = math.sqrt(sum((a - b) ** 2 for a, b in zip(vectors[key], query)))
            assert math.isclose(distance, real, rel_tol=1e-12, abs_tol=1e-12)
        if mode == "exact":
            assert ids == expected
        group = "filtered" if bucket >= 0 else "unfiltered"
        recalls[group].append(len(set(ids).intersection(expected)) / len(expected))
    report = {key: {"count": len(values), "mean_recall_at_10": sum(values) / len(values),
                    "minimum_recall_at_10": min(values)} for key, values in recalls.items()}
    assert all(group["count"] == 500 and group["mean_recall_at_10"] >= 0.95 for group in report.values())
    return report


def run_query(image, helper, mode, inserts, statements, truth, buckets, vectors, queries):
    args = SimpleNamespace(image=image, binary=None)
    with fresh_volume() as volume:
        with Server(args, None, MEMORY, mode=mode, volume=volume, probe=helper) as db:
            db.ok(CREATE)
            for insert in inserts:
                db.ok(insert, 100)
            prepare = perf_counter()
            if mode == "ann":
                db.ok("CREATE VECTOR INDEX idx ON vectors(v);")
            for statement in statements[:8]:
                rows, count, error = db.sql(statement)
                assert len(rows) == count == 10 and error is None
            preparation_seconds = perf_counter() - prepare
            begin = perf_counter()
            responses = [db.sql(statement) for statement in statements]
            seconds = perf_counter() - begin
            measured = resources(db)
    recall = verify_queries(responses, truth, buckets, vectors, queries, mode)
    return {"seconds": seconds, "index_and_warmup_seconds": preparation_seconds,
            "valid_unique_ids_filters_distances_passed": True, "recall": recall, **measured}


def efficiency(reference, result):
    fields = (("seconds", 0.001, 0.6), ("io_bytes", 1048576, 0.2),
              ("memory_peak_bytes", 1048576, 0.2))
    score = 0.0
    for key, epsilon, weight in fields:
        if reference[key] is None or result[key] is None:
            return None
        ratio = (reference[key] + epsilon) / (result[key] + epsilon)
        score += weight * ratio / (1 + ratio)
    return score


def summary(rounds):
    def medians(records):
        result = {}
        for key in ("seconds", "io_bytes", "memory_peak_bytes"):
            values = [record[key] for record in records]
            result[key] = median(values) if all(value is not None for value in values) else None
        return result

    reference_write = medians([row["write"]["baseline"] for row in rounds])
    reference_query = medians([row["query"]["baseline"] for row in rounds])
    result = {}
    for mode in ("baseline", "exact", "ann"):
        write = "baseline" if mode == "baseline" else "candidate"
        values = []
        for row in rounds:
            w = efficiency(reference_write, row["write"][write])
            q = efficiency(reference_query, row["query"][mode])
            values.append((w + 2 * q) / 3 if w is not None and q is not None else None)
        result[mode] = {"write_median": medians([row["write"][write] for row in rounds]),
                        "query_median": medians([row["query"][mode] for row in rounds]),
                        "local_efficiency_rounds": values,
                        "local_efficiency_median": round(median(values), 6)
                        if all(value is not None for value in values) else None}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-image", default="minivectordb-round2:baseline-590ff19")
    parser.add_argument("--candidate-image", default="minivectordb-round2:dev")
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--seed", type=int, default=20261109)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error("--rounds must be positive")
    helper = args.probe.resolve(strict=True)
    buckets, vectors, queries = dataset(args.seed)
    inserts, statements = input_statements(buckets, vectors, queries)
    truth, input_hash = exact_truth(args.candidate_image, helper, buckets, vectors, queries)
    print(f"Independent oracle ready: {ROWS} rows × {DIMENSION}, {QUERIES} queries", flush=True)
    report = {"scope": "Local paired development run using the specified reference image.",
              "measurement": "Two CPUs, 64 MiB, no swap, disconnected containers with fresh Docker volumes. "
              "io.stat is for the volume backing device shared with image storage. "
              "memory.peak includes one final probe process. No host cache reset.",
              "seed": args.seed, "dataset_sha256": input_hash, "rows": ROWS,
              "dimension": DIMENSION, "queries": QUERIES, "top_k": 10, "rounds": [],
              "images": {mode: docker("image", "inspect", image, "--format", "{{.Id}}").decode().strip()
                         for mode, image in (("baseline", args.baseline_image),
                                             ("candidate", args.candidate_image))}}
    for number in range(args.rounds):
        row = {"number": number + 1, "write": {}, "query": {}}
        write_order = [("baseline", args.baseline_image), ("candidate", args.candidate_image)]
        if number % 2:
            write_order.reverse()
        for label, image in write_order:
            row["write"][label] = run_write(image, helper, inserts, buckets, vectors)
            print(f"Round {number + 1} {label} write: {row['write'][label]['seconds']:.3f}s", flush=True)
        query_order = [("baseline", args.baseline_image, "exact"),
                       ("exact", args.candidate_image, "exact"),
                       ("ann", args.candidate_image, "ann")]
        rotate = number % len(query_order)
        for label, image, mode in query_order[rotate:] + query_order[:rotate]:
            row["query"][label] = run_query(image, helper, mode, inserts, statements, truth,
                                            buckets, vectors, queries)
            print(f"Round {number + 1} {label} query: {row['query'][label]['seconds']:.3f}s, "
                  f"recall={row['query'][label]['recall']}", flush=True)
        report["rounds"].append(row)
        report["summary"] = summary(report["rounds"])
        args.output.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps(report["summary"], indent=2), flush=True)


if __name__ == "__main__":
    main()
