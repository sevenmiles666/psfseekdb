"""Same-host B-C-C-B comparison of durable bulk writes with large text values."""
import argparse
import json
from pathlib import Path
import statistics
import tempfile
import time
import test_engine

parser = argparse.ArgumentParser()
parser.add_argument('--baseline', type=Path, required=True)
parser.add_argument('--candidate', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--rows', type=int, default=2000)
parser.add_argument('--text-bytes', type=int, default=32768)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=False)
rounds = []
for index, variant in enumerate(('baseline', 'candidate', 'candidate', 'baseline'), 1):
    test_engine.BINARY = getattr(args, variant).resolve()
    with tempfile.TemporaryDirectory(prefix='mvdb-text-benchmark-') as data:
        p = test_engine.Peer(data)
        try:
            p.sql(f'CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR({args.text_bytes}));')
            payload = 'q' * (args.text_bytes - 16)
            before = time.monotonic()
            for first in range(0, args.rows, 100):
                end = min(first + 100, args.rows)
                values = ','.join(f"({i},'{i:016d}{payload}')" for i in range(first, end))
                assert p.sql('INSERT INTO t VALUES ' + values + ';')['affected_rows'] == end - first
            elapsed = time.monotonic() - before
            p.close()
            p = test_engine.Peer(data)
            assert p.sql('SELECT id FROM t ORDER BY id DESC LIMIT 3;')['rows'] == [[args.rows - i] for i in (1, 2, 3)]
            for key in (0, args.rows // 2, args.rows - 1):
                assert p.sql(f'SELECT s FROM t WHERE id={key};')['rows'] == [[f'{key:016d}' + payload]]
            rounds.append(dict(order=index, variant=variant, write_seconds=elapsed,
                               storage_bytes=(Path(data) / 'database.mvdb').stat().st_size))
            print(json.dumps(rounds[-1]), flush=True)
        finally:
            p.close()
old = statistics.mean(r['write_seconds'] for r in rounds if r['variant'] == 'baseline')
new = statistics.mean(r['write_seconds'] for r in rounds if r['variant'] == 'candidate')
report = dict(rows=args.rows, text_bytes=args.text_bytes, rounds=rounds, baseline_seconds=old,
              candidate_seconds=new, reduction_percent=100 * (old - new) / old,
              scope='Local same-host wall clock, includes commit acknowledgements; restart/content checks outside timing; not official scoring.')
(args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report), flush=True)
