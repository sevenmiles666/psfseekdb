"""Independent SQL/state, tree, and process-crash checks for the real binary."""
import argparse
import copy
import json
import math
import os
from pathlib import Path
import random
import select
import shutil
import struct
import subprocess
import tempfile
import unittest

BINARY = Path(__file__).resolve().parents[1] / 'build/minivectordb'


class Peer:
    def __init__(self, data, fault=None, extra_env=None):
        env = dict(os.environ)
        env.update(extra_env or {})
        if fault:
            env['MVDB_CRASH_AT'] = fault
        self.process = subprocess.Popen(
            [str(BINARY), '--data-dir', str(data)], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        self.ident = 0
        assert self.read() == {'type': 'ready'}

    def read(self):
        if not select.select([self.process.stdout], [], [], 30)[0]:
            raise AssertionError('response timeout')
        line = self.process.stdout.readline()
        if not line:
            raise EOFError('database exited')
        return json.loads(line)

    def sql(self, text, ok=True, mode='exact', ident=None):
        self.ident += 1
        request_id = ident if ident is not None else self.ident
        self.process.stdin.write(json.dumps(dict(
            id=request_id, op='execute', sql=text, search_mode=mode),
            ensure_ascii=False).encode() + b'\n')
        self.process.stdin.flush()
        reply = self.read()
        assert reply['id'] == request_id and type(reply['id']) is type(request_id), reply
        assert reply['ok'] is ok, (text, reply)
        if not ok:
            assert isinstance(reply['error']['code'], str) and reply['error']['code']
        return reply

    def close(self):
        try:
            if self.process.poll() is None:
                self.process.stdin.write(b'{"id":"stop","op":"shutdown"}\n')
                self.process.stdin.flush()
                assert self.read() == {'id': 'stop', 'ok': True}
                self.process.stdin.close()
                assert self.process.wait(timeout=30) == 0
                assert self.process.stdout.read() == b''
        finally:
            self.kill()

    def kill(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=30)
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            stream.close()


class EngineTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='mvdb-tests-')
        self.data = Path(self.tmp.name) / 'data'
        self.peer = Peer(self.data)

    def tearDown(self):
        self.peer.close()
        self.tmp.cleanup()

    def restart(self, crash=False):
        self.peer.kill() if crash else self.peer.close()
        self.peer = Peer(self.data)

    def test_boundaries_and_protocol(self):
        p = self.peer
        p.sql('CREATE TABLE t (id BIGINT PRIMARY KEY, text VARCHAR(2), v VECTOR(2));')
        for key in (-9223372036854775808, 9007199254740993, 9223372036854775807):
            p.sql(f"INSERT INTO t VALUES ({key}, '你😀', '[0,0]');")
        rows = p.sql('SELECT id FROM t ORDER BY id;')['rows']
        self.assertEqual(rows, [[-9223372036854775808], [9007199254740993], [9223372036854775807]])
        for value in ('9223372036854775808', '-9223372036854775809', '1.2'):
            p.sql(f"INSERT INTO t VALUES ({value}, 'a', '[0,0]');", ok=False)
        p.sql("INSERT INTO t VALUES (1, '一二三', '[0,0]');", ok=False)
        self.assertEqual(p.sql("SELECT id FROM t WHERE text='一二三四';")['rows'], [])
        for vector in ('[NaN,0]', '[Infinity,0]', '[1e100,0]', '[0]', '[true,0]', '[0,null]'):
            p.sql(f"INSERT INTO t VALUES (1, 'a', '{vector}');", ok=False)
        p.sql("SELECT id FROM t WHERE id = 9007199254740993;", ident='string-id')
        p.sql("UPDATE t SET id = 1;", ok=False)
        p.sql("SELECT * FROM t; DELETE FROM t;", ok=False)
        self.restart()
        self.assertEqual(self.peer.sql('SELECT id FROM t ORDER BY id;')['rows'], rows)

    def test_quotes_unicode_and_errors(self):
        p = self.peer
        p.sql('CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(30));')
        p.sql("INSERT INTO t VALUES (1,'a; b''c'),(2,'中文'),(3,'😀'),(4,'a\\nb');")
        result = p.sql('SELECT s FROM t ORDER BY s;')['rows']
        self.assertEqual(result, [[s] for s in sorted(["a; b'c", '中文', '😀', 'a\nb'])])
        for statement in ('SELECT bad FROM t;', 'INSERT INTO t VALUES (9);',
                          'UPDATE t SET s=1 WHERE id=99;', 'SELECT id FROM t WHERE missing=1;',
                          'SELECT id FROM t ORDER BY missing;', 'ALTER TABLE t ADD z BIGINT;',
                          'SELECT * FROM t WHERE id', 'SELECT * FROM t WHERE id =',
                          'BEGIN; COMMIT;', "INSERT INTO t VALUES (7,'unterminated);",
                          'CREATE TABLE bad (id BIGINT, id BIGINT PRIMARY KEY);'):
            p.sql(statement, ok=False)
        self.assertEqual(len(p.sql('SELECT * FROM t;')['rows']), 4)

    def test_statement_and_transaction_atomicity(self):
        p = self.peer
        p.sql('CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(8));')
        p.sql("INSERT INTO t VALUES (1,'base');")
        p.sql('BEGIN;')
        p.sql("UPDATE t SET s='keep' WHERE id=1;")
        p.sql("INSERT INTO t VALUES (2,'first'),(1,'dupe');", ok=False)
        self.assertEqual(p.sql('SELECT * FROM t;')['rows'], [[1, 'keep']])
        p.sql('COMMIT;')
        self.restart(crash=True)
        p = self.peer
        self.assertEqual(p.sql('SELECT * FROM t;')['rows'], [[1, 'keep']])
        p.sql("INSERT INTO t VALUES (2,'first'),(3,'too-long-value');", ok=False)
        p.sql('BEGIN;')
        p.sql('CREATE TABLE hidden (id BIGINT PRIMARY KEY);')
        p.sql("DELETE FROM t;")
        p.sql('ROLLBACK;')
        p.sql('SELECT * FROM hidden;', ok=False)
        self.assertEqual(p.sql('SELECT * FROM t;')['rows'], [[1, 'keep']])
        p.sql('BEGIN;')
        p.sql("INSERT INTO t VALUES (2,'pending');")
        self.restart(crash=True)
        self.assertEqual(self.peer.sql('SELECT * FROM t;')['rows'], [[1, 'keep']])

    def test_quoted_text_roundtrips_at_scan_boundaries(self):
        p = self.peer
        p.sql('CREATE TABLE "quoted_table" (`id` BIGINT PRIMARY KEY, s VARCHAR(100000));')
        rng = random.Random(20483)
        alphabet = "ab'\\;\n\t\0你😀\"`"
        samples = ['a' * n + "'\\你😀" for n in range(24)]
        samples += [''.join(rng.choice(alphabet) for _ in range(rng.randrange(257))) for _ in range(120)]
        samples += ["'\\\n" * 8192]
        def quoted(text):
            return "'" + text.replace('\\', '\\\\').replace("'", "''").replace('\n', '\\n').replace('\t', '\\t').replace('\0', '\\0') + "'"
        values = ','.join(f'({i},{quoted(text)})' for i, text in enumerate(samples))
        p.sql('INSERT INTO quoted_table VALUES ' + values + ';')
        self.assertEqual(p.sql('SELECT s FROM quoted_table ORDER BY id;')['rows'], [[s] for s in samples])
        self.restart()
        self.assertEqual(self.peer.sql('SELECT s FROM quoted_table ORDER BY id;')['rows'], [[s] for s in samples])

    def test_large_tree_splits_deletes_and_reopen(self):
        p = self.peer
        p.sql('CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(30));')
        keys = list(range(-10000, 10000))
        random.Random(619).shuffle(keys)
        for start in range(0, len(keys), 100):
            part = keys[start:start + 100]
            p.sql('INSERT INTO t VALUES ' + ','.join(f"({k},'v{k}')" for k in part) + ';')
        self.restart()
        p = self.peer
        self.assertEqual(p.sql('SELECT id FROM t ORDER BY id LIMIT 3;')['rows'], [[-10000], [-9999], [-9998]])
        self.assertEqual(p.sql('SELECT id FROM t ORDER BY id DESC LIMIT 3;')['rows'], [[9999], [9998], [9997]])
        self.assertEqual(p.sql("UPDATE t SET s='updated' WHERE id>=-100 AND id<=100;")['affected_rows'], 201)
        p.sql('BEGIN;')
        self.assertEqual(p.sql('DELETE FROM t WHERE id<0;')['affected_rows'], 10000)
        p.sql('ROLLBACK;')
        self.assertEqual(len(p.sql('SELECT id FROM t;')['rows']), 20000)
        self.assertEqual(p.sql('DELETE FROM t WHERE id>=-5000 AND id<5000;')['affected_rows'], 10000)
        self.restart(crash=True)
        p = self.peer
        expected = [[k, 'updated' if -100 <= k <= 100 else f'v{k}']
                    for k in sorted(keys) if not -5000 <= k < 5000]
        self.assertEqual(p.sql('SELECT id,s FROM t ORDER BY id;')['rows'], expected)
        self.assertEqual(p.sql('DELETE FROM t;')['affected_rows'], 10000)
        p.sql("INSERT INTO t VALUES (1,'new');")
        self.restart()
        self.assertEqual(self.peer.sql('SELECT * FROM t;')['rows'], [[1, 'new']])

    def test_random_transactions_against_reference(self):
        p = self.peer
        p.sql('CREATE TABLE t (id BIGINT PRIMARY KEY, value BIGINT);')
        randomizer = random.Random(80621)
        committed, visible, active = {}, {}, False
        for step in range(750):
            op = randomizer.randrange(7)
            key, value = randomizer.randrange(-60, 61), randomizer.randrange(-200, 201)
            if op == 0:
                p.sql(f'INSERT INTO t VALUES ({key},{value});', ok=key not in visible)
                if key not in visible:
                    visible[key] = value
            elif op == 1:
                p.sql(f'UPDATE t SET value={value} WHERE id={key};')
                if key in visible:
                    visible[key] = value
            elif op == 2:
                p.sql(f'DELETE FROM t WHERE id={key};')
                visible.pop(key, None)
            elif op == 3 and not active:
                p.sql('BEGIN;')
                active = True
            elif op == 4 and active:
                p.sql('COMMIT;')
                active = False
                committed = dict(visible)
            elif op == 5 and active:
                p.sql('ROLLBACK;')
                active = False
                visible = dict(committed)
            elif op == 6:
                self.restart(crash=True)
                p = self.peer
                visible = dict(committed)
                active = False
            if not active:
                committed = dict(visible)
            if step % 9 == 0:
                self.assertEqual(p.sql('SELECT * FROM t ORDER BY id;')['rows'], [[k, v] for k, v in sorted(visible.items())])

    def test_vector_filter_topk_and_transaction(self):
        p = self.peer
        p.sql('CREATE TABLE t (id BIGINT PRIMARY KEY, c BIGINT, v VECTOR(4));')
        rng = random.Random(717)
        vectors = {}
        for k in range(100):
            values = [struct.unpack('f', struct.pack('f', rng.uniform(-2, 2)))[0] for _ in range(4)]
            vectors[k] = values
            p.sql(f"INSERT INTO t VALUES ({k},{k % 3},'{json.dumps(values)}');")
        p.sql('CREATE VECTOR INDEX vi ON t(v);')
        for mode in ('exact', 'ann'):
            for category in (None, 0, 2, 99):
                query = [struct.unpack('f', struct.pack('f', rng.uniform(-2, 2)))[0] for _ in range(4)]
                where = '' if category is None else f'WHERE c={category}'
                actual = p.sql(f"SELECT id,L2_DISTANCE(v,'{json.dumps(query)}') AS d FROM t {where} ORDER BY d LIMIT 10;", mode=mode)['rows']
                truth = sorted((math.dist(v, query), k) for k, v in vectors.items()
                               if category is None or k % 3 == category)[:10]
                self.assertEqual([r[0] for r in actual], [k for _, k in truth])
                for row, (distance, _) in zip(actual, truth):
                    self.assertAlmostEqual(row[1], distance, places=9)
        p.sql('BEGIN;')
        p.sql("UPDATE t SET v='[0,0,0,0]' WHERE id=17;")
        self.assertEqual(p.sql("SELECT id FROM t ORDER BY L2_DISTANCE(v,'[0,0,0,0]') LIMIT 1;")['rows'], [[17]])
        p.sql('ROLLBACK;')
        self.assertEqual(p.sql('SELECT v FROM t WHERE id=17;')['rows'], [[vectors[17]]])

    def test_vector_index_preserves_rounded_l2_ties(self):
        p = self.peer
        p.sql('CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(2));')
        p.sql("INSERT INTO t VALUES (1,'[1,0.00000001490116119384765625]'),(2,'[1,0]');")
        query = "SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM t ORDER BY d LIMIT 1;"
        expected = [[1, 1.0]]
        self.assertEqual(p.sql(query)['rows'], expected)
        p.sql('CREATE VECTOR INDEX vi ON t(v);')
        for mode in ('exact', 'ann'):
            self.assertEqual(p.sql(query, mode=mode)['rows'], expected)
            self.assertEqual(p.sql(query.replace('ORDER BY d', 'ORDER BY d,id'), mode=mode)['rows'], expected)
            self.assertEqual(p.sql(query.replace('LIMIT 1', 'LIMIT 2'), mode=mode)['rows'], [[1, 1.0], [2, 1.0]])
        self.restart()
        self.assertEqual(self.peer.sql(query)['rows'], expected)

    def test_topk_projects_only_final_rows(self):
        p = self.peer
        p.sql('CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(9000000));')
        payload = 'q' * 9000000
        p.sql("INSERT INTO t VALUES (1,'" + payload + "'),(2,'" + payload + "'),(3,'small3'),(4,'small4');")
        expected = [[4, 'small4'], [3, 'small3']]
        self.assertEqual(p.sql('SELECT id,s FROM t ORDER BY id DESC LIMIT 2;')['rows'], expected)
        self.assertEqual(p.sql('SELECT id,s FROM t ORDER BY s DESC LIMIT 2;')['rows'], expected)
        self.assertEqual(p.sql('SELECT id FROM t ORDER BY s DESC;')['rows'], [[4], [3], [1], [2]])
        p.sql('SELECT id,s FROM t ORDER BY id LIMIT 2;', ok=False)
        p.sql('SELECT id,s FROM t LIMIT 2;', ok=False)
        self.assertEqual(p.sql('SELECT id FROM t ORDER BY id;')['rows'], [[1], [2], [3], [4]])

    def test_sort_keys_can_exceed_cache_budget(self):
        p = self.peer
        p.sql('CREATE TABLE t (id BIGINT PRIMARY KEY, c BIGINT, s VARCHAR(1024));')
        count = 9000
        for first in range(0, count, 100):
            values = ','.join(f"({i},{i % 4},'{'x' * 1017}{i * 37 % count:07d}')" for i in range(first, first + 100))
            p.sql('INSERT INTO t VALUES ' + values + ';')
        expected = [[i] for i in sorted(range(count), key=lambda i: (-(i % 4), i * 37 % count))]
        self.assertEqual(p.sql('SELECT id FROM t ORDER BY c DESC,s;')['rows'], expected)
        self.assertEqual(p.sql('SELECT id FROM t ORDER BY c DESC,s LIMIT 8500;')['rows'], expected[:8500])

    def test_multicolumn_order_matches_reference(self):
        p = self.peer
        p.sql('CREATE TABLE t (id BIGINT PRIMARY KEY, c BIGINT, s VARCHAR(8));')
        rng = random.Random(7385)
        records = [(i, rng.randrange(-5, 6), rng.choice(['aa', 'b', '你', '😀', ''])) for i in range(-80, 81)]
        rng.shuffle(records)
        p.sql('INSERT INTO t VALUES ' + ','.join(f"({i},{c},'{s}')" for i, c, s in records) + ';')
        for specifications in ([('c', False)], [('c', True), ('s', False)], [('s', False), ('id', True)],
                               [('s', True), ('c', False), ('id', False)]):
            expected = sorted(records)
            for name, descending in reversed(specifications):
                column = {'id': 0, 'c': 1, 's': 2}[name]
                expected.sort(key=lambda row: row[column], reverse=descending)
            order = ','.join(name + (' DESC' if descending else ' ASC') for name, descending in specifications)
            for limit in (None, 0, 1, 17, 500):
                suffix = '' if limit is None else f' LIMIT {limit}'
                wanted = expected if limit is None else expected[:limit]
                self.assertEqual(p.sql(f'SELECT id,c,s FROM t ORDER BY {order}{suffix};')['rows'], [list(row) for row in wanted])

    def test_commit_crash_points_and_interrupted_recovery(self):
        self.peer.sql('CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(8));')
        self.peer.sql("INSERT INTO t VALUES (1,'base');")
        self.peer.close()
        base = Path(self.tmp.name) / 'base'
        shutil.copytree(self.data, base)
        for point in ('after_statement', 'before_data_sync', 'after_data_sync', 'after_root_write', 'after_root_sync'):
            with self.subTest(point=point):
                directory = Path(self.tmp.name) / point
                shutil.copytree(base, directory)
                process = Peer(directory, fault=point)
                with self.assertRaises(EOFError):
                    process.sql("INSERT INTO t VALUES (2,'new'),(3,'new');")
                self.assertEqual(process.process.wait(timeout=30), 86)
                process.kill()
                if point != 'after_statement':
                    # Recovery can itself be interrupted, without publishing partial state.
                    crashed = subprocess.run([str(BINARY), '--data-dir', str(directory)],
                                             input=b'', capture_output=True,
                                             env={**os.environ, 'MVDB_CRASH_AT': 'recovery_loaded'}, timeout=30)
                    self.assertEqual(crashed.returncode, 86)
                recovered = Peer(directory)
                rows = recovered.sql('SELECT * FROM t ORDER BY id;')['rows']
                self.assertIn(rows, ([[1, 'base']], [[1, 'base'], [2, 'new'], [3, 'new']]))
                if point in ('after_statement', 'before_data_sync', 'after_data_sync'):
                    self.assertEqual(rows, [[1, 'base']])
                if point == 'after_root_sync':
                    self.assertEqual(len(rows), 3)
                recovered.close()
        self.peer = Peer(self.data)

    def test_compaction_and_its_crash_boundaries(self):
        self.peer.close()
        self.peer = Peer(self.data, extra_env={'MVDB_COMPACT_THRESHOLD': '32768'})
        self.peer.sql('CREATE TABLE t (id BIGINT PRIMARY KEY, value BIGINT);')
        self.peer.sql('INSERT INTO t VALUES (1,0);')
        for value in range(1, 100):
            self.peer.sql(f'UPDATE t SET value={value} WHERE id=1;')
        self.assertLess((self.data / 'database.mvdb').stat().st_size, 100000)
        self.restart(crash=True)
        self.assertEqual(self.peer.sql('SELECT * FROM t;')['rows'], [[1, 99]])
        self.peer.close()
        for point in ('compaction_written', 'before_compaction_rename', 'after_compaction_rename'):
            target = Path(self.tmp.name) / point
            shutil.copytree(self.data, target)
            p = Peer(target, fault=point, extra_env={'MVDB_COMPACT_THRESHOLD': '8192'})
            crashed = False
            for value in range(200):
                try:
                    p.sql(f'UPDATE t SET value={value} WHERE id=1;')
                except EOFError:
                    crashed = True
                    break
            self.assertTrue(crashed)
            p.kill()
            p = Peer(target)
            self.assertEqual(p.sql('SELECT * FROM t;')['rows'], [[1, value]])
            p.close()
        self.peer = Peer(self.data)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', type=Path)
    args, remaining = parser.parse_known_args()
    if args.binary:
        BINARY = args.binary.resolve()
    unittest.main(argv=[__file__, *remaining], verbosity=2)
