"""Reproducible local measurement with fresh vectors and independent exact truth."""
import argparse
import json
import math
import random
import statistics
import struct
import tempfile
import time
from pathlib import Path
import test_engine

parser = argparse.ArgumentParser()
parser.add_argument('--binary', type=Path, required=True)
parser.add_argument('--rows', type=int, default=10000)
parser.add_argument('--dimensions', type=int, default=128)
parser.add_argument('--queries', type=int, default=1000)
parser.add_argument('--rounds', type=int, default=3)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=False)
test_engine.BINARY = args.binary.resolve()
rng = random.Random(20261111)
f32 = lambda x: struct.unpack('f', struct.pack('f', x))[0]
vectors = [[f32(rng.uniform(-1, 1)) for _ in range(args.dimensions)] for _ in range(args.rows)]
queries = [[f32(rng.uniform(-1, 1)) for _ in range(args.dimensions)] for _ in range(args.queries)]
truth = {}
for i, query in enumerate(queries):
    if i >= 20 and i % 50:
        continue
    category = i % 2 == 1
    candidates = [(math.dist(v, query), k) for k, v in enumerate(vectors) if not category or k % 8 == i % 8]
    truth[i] = sorted(candidates)[:10]
rounds = []
for iteration in range(args.rounds):
    with tempfile.TemporaryDirectory(prefix='mvdb-benchmark-') as directory:
        p = test_engine.Peer(directory)
        try:
            p.sql(f'CREATE TABLE docs (id BIGINT PRIMARY KEY, category BIGINT, embedding VECTOR({args.dimensions}));')
            before = time.monotonic()
            for first in range(0, args.rows, 100):
                values = ','.join(f"({i},{i % 8},'{json.dumps(vectors[i], separators=(',', ':'))}')"
                                  for i in range(first, min(first + 100, args.rows)))
                p.sql('INSERT INTO docs VALUES ' + values + ';')
            write_seconds = time.monotonic() - before
            before = time.monotonic()
            p.sql('CREATE VECTOR INDEX vi ON docs(embedding);')
            p.sql("SELECT id FROM docs ORDER BY L2_DISTANCE(embedding,'" + json.dumps(queries[0]) + "') LIMIT 10;")
            prepare_seconds = time.monotonic() - before
            durations = []
            for i, query in enumerate(queries):
                where = f'WHERE category={i % 8}' if i % 2 else ''
                sql = f"SELECT id,L2_DISTANCE(embedding,'{json.dumps(query)}') AS d FROM docs {where} ORDER BY d LIMIT 10;"
                before = time.monotonic()
                rows = p.sql(sql)['rows']
                durations.append(time.monotonic() - before)
                if i in truth:
                    assert [r[0] for r in rows] == [key for _, key in truth[i]]
                    for row, (distance, key) in zip(rows, truth[i]):
                        assert math.isclose(row[1], distance, abs_tol=1e-5, rel_tol=1e-5)
            report = dict(round=iteration + 1, write_seconds=write_seconds, prepare_seconds=prepare_seconds,
                          query_seconds=sum(durations), query_p50_ms=statistics.median(durations) * 1000,
                          query_p95_ms=sorted(durations)[int(0.95 * (len(durations) - 1))] * 1000,
                          exact_truth_checks=len(truth), storage_bytes=(Path(directory) / 'database.mvdb').stat().st_size)
            rounds.append(report)
            print(json.dumps(report), flush=True)
            (args.output / 'result.json').write_text(json.dumps(dict(
                rows=args.rows, dimensions=args.dimensions, queries=args.queries, rounds=rounds,
                scope='Local wall-clock measurement only; not official score or hardware.'), indent=2) + '\n')
        finally:
            p.close()
