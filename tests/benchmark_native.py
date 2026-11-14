"""Paired JSONL benchmark using Linux processes and private temporary directories."""

import argparse
import hashlib
import json
import os
import tempfile
from pathlib import Path
from statistics import median
from time import perf_counter
from types import SimpleNamespace

from benchmark_local import CREATE, dataset, input_statements, ROWS, QUERIES
from check_core import Server


def cpu_seconds(pid):
    fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


def elapsed(db, run):
    cpu = cpu_seconds(db.process.pid)
    started = perf_counter()
    result = run()
    return {"wall": perf_counter() - started, "cpu": cpu_seconds(db.process.pid) - cpu}, result


def run(binary, inserts, queries):
    args = SimpleNamespace(binary=binary.resolve(strict=True), image=None)
    with tempfile.TemporaryDirectory(prefix="mvdb-native-bench-") as temp:
        with Server(args, Path(temp)) as db:
            assert not db.legacy
            db.ok(CREATE)

            def insert_batch():
                for insert in inserts:
                    db.ok(insert, 100)

            timings = {}
            timings["write"], _ = elapsed(db, insert_batch)
            for query in queries[:8]:
                rows, count, error = db.sql(query)
                assert error is None and len(rows) == count == 10
            signature = hashlib.sha256()
            for name, group in (("unfiltered", queries[::2]), ("filtered", queries[1::2])):
                timings[name], result = elapsed(db, lambda: [db.sql(sql) for sql in group])
                assert all(error is None and len(rows) == count == 10 for rows, count, error in result)
                signature.update(json.dumps(result, separators=(",", ":")).encode())
            return timings, signature.hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--candidate", type=Path)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--seed", type=int, default=20261109)
    parser.add_argument("--vector-distribution", choices=("grid", "continuous"), default="grid")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error('--rounds must be positive')
    inserts, queries = input_statements(*dataset(args.seed, args.vector_distribution))
    binaries = [("reference", args.reference)]
    if args.candidate:
        binaries.append(("candidate", args.candidate))
    rounds = []
    signature = None
    for i in range(args.rounds):
        entry = {}
        order = binaries if i % 2 == 0 else list(reversed(binaries))
        for label, binary in order:
            timings, actual = run(binary, inserts, queries)
            if signature is None:
                signature = actual
            assert signature == actual, (label, i, signature, actual)
            entry[label] = timings
            print(f"round {i+1} {label} {timings}", flush=True)
        rounds.append(entry)
    summary = {}
    for label, _ in binaries:
        summary[label] = {name: {measure: median(record[label][name][measure] for record in rounds)
                                 for measure in ("wall", "cpu")}
                          for name in ("write", "unfiltered", "filtered")}
        summary[label]["query_total_wall"] = median(
            record[label]["unfiltered"]["wall"] + record[label]["filtered"]["wall"] for record in rounds)
    report = {"rows": ROWS, "queries": QUERIES, "seed": args.seed, "distribution": args.vector_distribution,
              "same_results_sha256": signature,
              "rounds": rounds, "summary": summary}
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2), flush=True)


if __name__ == "__main__":
    main()
