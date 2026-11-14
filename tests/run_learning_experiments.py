"""Run small real SQL experiments and retain raw requests/responses.

Outputs are Agent/tool preparation, not student predictions or official scores.
This reuses only the process/protocol adapter from check_core.py; expected rows
are specified independently in each small scenario.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import platform
import struct
import tempfile

from check_core import Server


class Recorder:
    def __init__(self, args, directory):
        self.args = args
        self.directory = directory
        self.events = []
        self.db = None

    def start(self):
        self.db = Server(self.args, self.directory)
        assert not self.db.legacy, 'experiments require the official JSONL adapter'
        self.events.append({'event': 'ready', 'response': {'type': 'ready'}})

    def sql(self, sql, *, rows=None, affected=None, error=False):
        self.db.sequence += 1
        request = {'id': self.db.sequence, 'op': 'execute', 'sql': sql, 'search_mode': 'exact'}
        response = self.db.request(request)
        self.events.append({'request': request, 'response': response})
        assert response['ok'] is not error, (sql, response)
        if rows is not None:
            assert response.get('rows') == rows, (sql, response, rows)
        if affected is not None:
            assert response.get('affected_rows', 0) == affected, (sql, response)
        return response

    def stop(self, crash=False):
        if self.db is not None:
            self.db.stop(crash=crash)
            self.db = None
            self.events.append({'event': 'SIGKILL' if crash else 'EOF_and_wait'})

    def reopen(self, crash=False):
        self.stop(crash=crash)
        self.start()


def atomicity(r):
    for name, transaction, split, final, expected in [
        ('batch', False, False, None, [[1]]),
        ('split', False, True, None, [[1], [2]]),
        ('tx_commit', True, False, 'COMMIT', [[1], [2]]),
        ('tx_rollback', True, False, 'ROLLBACK', [[1]]),
    ]:
        r.sql(f'CREATE TABLE {name} (id BIGINT PRIMARY KEY);')
        r.sql(f'INSERT INTO {name} VALUES (1);', affected=1)
        if transaction:
            r.sql('BEGIN;')
            r.sql(f'INSERT INTO {name} VALUES (2);', affected=1)
            r.sql(f'INSERT INTO {name} VALUES (3),(1);', error=True)
            r.sql(f'SELECT id FROM {name} ORDER BY id;', rows=[[1], [2]])
        elif split:
            r.sql(f'INSERT INTO {name} VALUES (2);', affected=1)
            r.sql(f'INSERT INTO {name} VALUES (1);', error=True)
        else:
            r.sql(f'INSERT INTO {name} VALUES (2),(1);', error=True)
        if final:
            r.sql(final + ';')
        r.sql(f'SELECT id FROM {name} ORDER BY id;', rows=expected)
    r.reopen()
    for name, expected in [('batch', [[1]]), ('split', [[1], [2]]),
                           ('tx_commit', [[1], [2]]), ('tx_rollback', [[1]])]:
        r.sql(f'SELECT id FROM {name} ORDER BY id;', rows=expected)


def filtered_topk(r):
    r.sql('CREATE TABLE f (id BIGINT PRIMARY KEY, bucket BIGINT, v VECTOR(2));')
    r.sql('INSERT INTO f VALUES (1,0,[0,0]),(2,1,[3,4]),(3,1,[0,6]),(4,0,[0.1,0]);', affected=4)
    for stage in ['without_index', 'with_index', 'after_restart']:
        if stage == 'with_index':
            r.sql('CREATE VECTOR INDEX fi ON f(v);')
        elif stage == 'after_restart':
            r.reopen()
        r.sql('SELECT id,L2_DISTANCE(v,[0,0]) AS d FROM f WHERE bucket=1 ORDER BY d LIMIT 2;', rows=[[2, 5.0], [3, 6.0]])
        r.sql('SELECT id FROM f WHERE bucket=1 ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 10;', rows=[[2], [3]])
        r.sql('SELECT id FROM f WHERE bucket=99 ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 10;', rows=[])


def rounded_ties(r):
    r.sql('CREATE TABLE ties (id BIGINT PRIMARY KEY, v VECTOR(2));')
    r.sql('INSERT INTO ties VALUES (2,[1,0]),(1,[1,0.00000001490116119384765625]);', affected=2)
    for stage in ['without_index', 'with_index', 'after_restart']:
        if stage == 'with_index':
            r.sql('CREATE VECTOR INDEX ti ON ties(v);')
        elif stage == 'after_restart':
            r.reopen()
        prefix = 'SELECT id,L2_DISTANCE(v,[0,0]) AS d FROM ties ORDER BY d'
        r.sql(prefix + ' LIMIT 1;', rows=[[1, 1.0]])
        r.sql(prefix + ' DESC;', rows=[[1, 1.0], [2, 1.0]])
        r.sql(prefix + ',id DESC LIMIT 1;', rows=[[2, 1.0]])


def index_transactions(r):
    r.sql('CREATE TABLE x (id BIGINT PRIMARY KEY, v VECTOR(2));')
    r.sql('INSERT INTO x VALUES (1,[0,0]),(2,[2,0]);', affected=2)
    r.sql('CREATE VECTOR INDEX xi ON x(v);')
    query = 'SELECT id FROM x ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 2;'
    r.sql('BEGIN;')
    r.sql('UPDATE x SET v=[9,0] WHERE id=1;', affected=1)
    r.sql('DELETE FROM x WHERE id=2;', affected=1)
    r.sql('INSERT INTO x VALUES (3,[0.5,0]);', affected=1)
    r.sql('SELECT id FROM x ORDER BY id;', rows=[[1], [3]])
    r.sql(query, rows=[[3], [1]])
    r.sql('ROLLBACK;')
    r.sql(query, rows=[[1], [2]])
    r.reopen()
    r.sql(query, rows=[[1], [2]])


def crash_recovery(r):
    r.sql('CREATE TABLE c (id BIGINT PRIMARY KEY);')
    r.sql('INSERT INTO c VALUES (1);', affected=1)
    r.sql('BEGIN;')
    r.sql('INSERT INTO c VALUES (2);', affected=1)
    r.sql('SELECT id FROM c ORDER BY id;', rows=[[1], [2]])
    r.reopen(crash=True)
    r.sql('SELECT id FROM c ORDER BY id;', rows=[[1]])
    r.sql('BEGIN;')
    r.sql('INSERT INTO c VALUES (3);', affected=1)
    r.sql('COMMIT;')
    r.reopen(crash=True)
    r.sql('SELECT id FROM c ORDER BY id;', rows=[[1], [3]])
    r.reopen()
    r.sql('SELECT id FROM c ORDER BY id;', rows=[[1], [3]])


def float32_roundtrip(r):
    vectors = [[1.0, 0.5, -0.25], [0.1, 1.23456789, -0.33333333],
               [1.401298464324817e-45, 1e-30, -1e30]]
    expected_bits = [struct.pack('<3f', *v) for v in vectors]
    r.sql('CREATE TABLE v (id BIGINT PRIMARY KEY, value VECTOR(3));')
    for key, vector in enumerate(vectors, 1):
        r.sql(f'INSERT INTO v VALUES ({key},{json.dumps(vector)});', affected=1)
    r.sql('INSERT INTO v VALUES (4,[1,2]);', error=True)
    r.sql('INSERT INTO v VALUES (5,[1e100,0,0]);', error=True)
    for stage in ['before_restart', 'after_restart']:
        if stage == 'after_restart':
            r.reopen()
        rows = r.sql('SELECT id,value FROM v ORDER BY id;')['rows']
        assert [row[0] for row in rows] == [1, 2, 3]
        actual_bits = [struct.pack('<3f', *row[1]) for row in rows]
        assert actual_bits == expected_bits
        r.events.append({'event': 'independent_float32_bit_check', 'status': 'PASS',
                         'expected_hex': [v.hex() for v in expected_bits],
                         'actual_hex': [v.hex() for v in actual_bits]})


def unicode_length(r):
    r.sql('CREATE TABLE u (id BIGINT PRIMARY KEY, text VARCHAR(2));')
    r.sql("INSERT INTO u VALUES (1,'你🙂'),(2,'你a');", affected=2)
    r.sql("INSERT INTO u VALUES (3,'你🙂a');", error=True)
    r.sql('SELECT id,text FROM u ORDER BY text;', rows=[[2, '你a'], [1, '你🙂']])
    r.reopen()
    r.sql('SELECT id,text FROM u ORDER BY id;', rows=[[1, '你🙂'], [2, '你a']])


def autocommit_before_begin(r):
    r.sql('CREATE TABLE b (id BIGINT PRIMARY KEY);')
    r.sql('INSERT INTO b VALUES (1);', affected=1)
    r.sql('INSERT INTO b VALUES (2);', affected=1)
    r.sql('BEGIN;')
    r.sql('INSERT INTO b VALUES (3),(1);', error=True)
    r.sql('SELECT id FROM b ORDER BY id;', rows=[[1], [2]])
    r.sql('ROLLBACK;')
    r.sql('SELECT id FROM b ORDER BY id;', rows=[[1], [2]])
    r.reopen()
    r.sql('SELECT id FROM b ORDER BY id;', rows=[[1], [2]])


CASES = [
    ('statement-and-transaction-atomicity', atomicity),
    ('filtered-topk', filtered_topk), ('rounded-l2-ties', rounded_ties),
    ('index-transaction-visibility', index_transactions),
    ('confirmed-commit-and-crash', crash_recovery),
    ('float32-roundtrip-and-rejection', float32_roundtrip),
    ('unicode-character-length', unicode_length),
    ('autocommit-before-begin', autocommit_before_begin),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', choices=[name for name, _ in CASES])
    parser.add_argument('--source-commit', required=True)
    parser.add_argument('--build-note', default='')
    args = parser.parse_args()
    args.binary = args.binary.resolve()
    args.image = None
    results = []
    for name, operation in CASES:
        if args.case and args.case != name:
            continue
        with tempfile.TemporaryDirectory(prefix='mvdb-learning-') as directory:
            recorder = Recorder(args, Path(directory))
            try:
                recorder.start()
                operation(recorder)
            finally:
                recorder.stop()
            results.append({'id': name, 'role': 'tool', 'status': 'PASS',
                            'output': json.dumps(recorder.events, ensure_ascii=False, indent=2)})
        print(name + ': PASS', flush=True)
    repo = Path(__file__).resolve().parents[1]
    report = {
        'role': 'tool', 'recorded_at': datetime.now(timezone.utc).isoformat(),
        'source_commit': args.source_commit, 'platform': platform.platform(),
        'binary_sha256': hashlib.sha256(args.binary.read_bytes()).hexdigest(),
        'source_sha256': {p.relative_to(repo).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in sorted((repo / 'src').glob('*')) if p.is_file()},
        'build_note': args.build_note, 'experiments': results,
        'student_predictions_added': 0, 'official_grading_performed': False,
        'scope': 'Local semantic experiments prepared and run by Agent; no performance or student-learning score is inferred.',
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')


if __name__ == '__main__':
    main()
