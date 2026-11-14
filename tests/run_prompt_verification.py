"""Execute extended Prompt verification; preserve results and independent oracles.

Uses native binaries without claiming Linux container resource enforcement.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import platform
import random
from statistics import median
import struct
import subprocess
import tempfile
import zlib
from time import perf_counter
from types import SimpleNamespace

import check_core
from check_core import Server
from benchmark_local import CREATE, dataset, input_statements, verify_queries, ROWS, DIMENSION, QUERIES


def digest(value):
    return hashlib.sha256(json.dumps(value, ensure_ascii=False, separators=(',', ':'), allow_nan=False).encode()).hexdigest()


class TraceServer(Server):
    events = None

    def request(self, message, *, ensure_ascii=False):
        self.process.stdin.write(json.dumps(message, ensure_ascii=ensure_ascii) + '\n')
        self.process.stdin.flush()
        raw = self.line()
        # Keep the sign in JSON's legal -0 spelling for independent FP32 checks.
        response = json.loads(raw, parse_int=lambda v: -0.0 if v == '-0' else int(v))
        assert type(response['id']) is type(message['id']) and response['id'] == message['id']
        assert type(response['ok']) is bool
        event = {'request': message, 'response': response, 'raw_response': raw}
        if len(message.get('sql', '')) > 2000:
            event['request'] = {k: v for k, v in message.items() if k != 'sql'}
            event['request'].update(sql_prefix=message['sql'][:80], sql_utf8_bytes=len(message['sql'].encode()),
                                    sql_sha256=hashlib.sha256(message['sql'].encode()).hexdigest())
        if self.events is not None:
            self.events.append(event)
        return response


def filtered(db):
    db.ok('CREATE TABLE f (id BIGINT PRIMARY KEY, bucket BIGINT, v VECTOR(2));')
    db.ok('INSERT INTO f VALUES (1,0,[0,0]),(2,1,[3,4]),(3,1,[0,6]),(4,0,[0.1,0]),(5,1,[0,8]);', 5)
    for mode in ('exact', 'ann'):
        db.mode = mode
        db.query('SELECT id FROM f WHERE bucket>=1 AND id<5 ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 10;', [[2], [3]])
        db.query('SELECT id FROM f WHERE bucket=1 AND id>=3 ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 1;', [[3]])
        db.query('SELECT id FROM f WHERE bucket>4 ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 10;', [])


def index_commit(args, directory):
    with TraceServer(args, directory) as db:
        db.ok('CREATE TABLE x (id BIGINT PRIMARY KEY, v VECTOR(2));')
        db.ok('INSERT INTO x VALUES (1,[0,0]),(2,[2,0]);', 2)
        db.ok('CREATE VECTOR INDEX xi ON x(v);')
        db.ok('BEGIN;')
        db.ok('UPDATE x SET v=[9,0] WHERE id=1;', 1)
        db.ok('DELETE FROM x WHERE id=2;', 1)
        db.ok('INSERT INTO x VALUES (3,[0.5,0]);', 1)
        for mode in ('exact', 'ann'):
            db.mode = mode
            db.query('SELECT id FROM x ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 2;', [[3], [1]])
        db.ok('COMMIT;')
        db.stop(crash=True)
    with TraceServer(args, directory) as db:
        db.query('SELECT id FROM x ORDER BY id;', [[1], [3]])
        for mode in ('exact', 'ann'):
            db.mode = mode
            db.query('SELECT id FROM x ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 2;', [[3], [1]])


def crash_vectors(args, directory):
    with TraceServer(args, directory) as db:
        db.ok('CREATE TABLE c (id BIGINT PRIMARY KEY, v VECTOR(2));')
        db.ok('INSERT INTO c VALUES (1,[1,0]);', 1)
        db.ok('CREATE VECTOR INDEX ci ON c(v);')
        db.ok('BEGIN;')
        db.ok('UPDATE c SET v=[9,0] WHERE id=1;', 1)
        db.ok('INSERT INTO c VALUES (2,[0,0]);', 1)
        db.stop(crash=True)
    with TraceServer(args, directory) as db:
        db.query('SELECT id,L2_DISTANCE(v,[0,0]) AS d FROM c ORDER BY d LIMIT 2;', [[1, 1]])
        db.ok('BEGIN;')
        db.ok('INSERT INTO c VALUES (3,[0,0]);', 1)
        db.ok('COMMIT;')
        db.stop(crash=True)
    with TraceServer(args, directory) as db:
        db.query('SELECT id,L2_DISTANCE(v,[0,0]) AS d FROM c ORDER BY d LIMIT 2;', [[3, 0], [1, 1]])


def float_sign(args, directory):
    values = [[-0.0, 0.0, 0.125], [0.1, 1.23456789, -1e30], [2**-149, -2**-149, 3e38]]
    bits = [struct.pack('<3f', *v) for v in values]
    with TraceServer(args, directory) as db:
        db.ok('CREATE TABLE z (id BIGINT PRIMARY KEY, v VECTOR(3));')
        for i, vector in enumerate(values):
            db.ok(f'INSERT INTO z VALUES ({i},{json.dumps(vector)});', 1)
        db.error('INSERT INTO z VALUES (9,[1e100,0,0]);', 'INVALID_VECTOR')
        actual, _, _ = db.sql('SELECT v FROM z ORDER BY id;')
        assert [struct.pack('<3f', *r[0]) for r in actual] == bits
    with TraceServer(args, directory) as db:
        actual, _, _ = db.sql('SELECT v FROM z ORDER BY id;')
        assert [struct.pack('<3f', *r[0]) for r in actual] == bits
    return {'expected_bits': [v.hex() for v in bits], 'actual_bits': [struct.pack('<3f', *r[0]).hex() for r in actual]}


def quantization(args, directory):
    rng = random.Random(20261114)
    checked = 0
    datasets = []
    f32 = lambda v: struct.unpack('<f', struct.pack('<f', v))[0]
    with TraceServer(args, directory) as db:
        for dimension in (31, 32, 33, 128, 129):
            size = 262144 // dimension + 7
            vectors = [[rng.randrange(-32, 33) / 8 for _ in range(dimension)] for _ in range(size)]
            vectors[-1] = vectors[0].copy()
            name = 'q' + str(dimension)
            db.ok(f'CREATE TABLE {name} (id BIGINT PRIMARY KEY, bucket BIGINT, v VECTOR({dimension}));')
            for start in range(0, size, 100):
                batch = vectors[start:start + 100]
                sql = ','.join(f'({start+i},{(start+i)%3},{json.dumps(v,separators=(",", ":"))})' for i, v in enumerate(batch))
                db.ok(f'INSERT INTO {name} VALUES {sql};', len(batch))
            db.ok(f'CREATE VECTOR INDEX i{dimension} ON {name}(v);')
            for phase in ('scaled', 'continuous_fallback'):
                if phase == 'continuous_fallback':
                    vectors[1][-1] = f32(0.1)
                    db.ok(f'UPDATE {name} SET v={json.dumps(vectors[1])} WHERE id=1;', 1)
                for bucket, condition in ((None, ''), (1, ' WHERE bucket>=1 AND bucket<=1')):
                    for query in (vectors[0], [f32(v+1e-4) for v in vectors[0]], [100.0]+vectors[0][1:]):
                        expected = sorted((math.sqrt(sum((a-b)**2 for a,b in zip(v,query))), i)
                                          for i,v in enumerate(vectors) if bucket is None or i%3 == bucket)[:7]
                        rows, count, error = db.sql(f'SELECT id,L2_DISTANCE(v,{json.dumps(query)}) AS d FROM {name}{condition} ORDER BY d LIMIT 7;')
                        assert error is None and count == 7
                        assert [r[0] for r in rows] == [i for _,i in expected]
                        assert all(math.isclose(r[1], e[0], rel_tol=1e-12, abs_tol=1e-12) for r,e in zip(rows,expected))
                        checked += 1
                datasets.append({'dimension': dimension, 'rows': size, 'phase': phase, 'vectors_sha256': digest(vectors)})
    return {'independent_queries': checked, 'datasets': datasets, 'seed': 20261114}


def wide_storage(args, directory):
    with TraceServer(args, directory) as db:
        db.ok('CREATE TABLE wide (id BIGINT PRIMARY KEY, note VARCHAR(32768), v VECTOR(2));')
        payload = 'z' * 32760
        for start in range(0, 1600, 16):
            sql = ','.join(f"({key},'{key*37%11:08d}{payload}',[{key},0])" for key in range(start, start+16))
            db.ok('INSERT INTO wide VALUES '+sql, 16)
        db.ok('CREATE VECTOR INDEX wi ON wide(v);')
    size = (directory/'store.wal').stat().st_size
    assert size > 32*1024*1024
    started = perf_counter()
    with TraceServer(args, directory) as db:
        ready = perf_counter()-started
        db.query('SELECT id FROM wide ORDER BY L2_DISTANCE(v,[1599,0]) LIMIT 5;', [[1599],[1598],[1597],[1596],[1595]])
        db.query('SELECT id FROM wide ORDER BY note LIMIT 3;', [[0],[11],[22]])
        expected = sorted(range(1600), key=lambda k: (-(k*37%11), k))
        db.query('SELECT id FROM wide ORDER BY note DESC;', [[k] for k in expected])
    return {'persisted_bytes': size, 'restart_to_ready_seconds': ready, 'memory_limit_enforced': False,
            'linux_container_limit_check': 'unavailable_no_local_container_runtime'}


def benchmark(args):
    reports = []
    for distribution in ('grid', 'continuous'):
        buckets, vectors, queries = dataset(20261114, distribution)
        inserts, statements = input_statements(buckets, vectors, queries)
        packed = bytearray(struct.pack('<III', ROWS, DIMENSION, QUERIES))
        packed.extend(struct.pack(f'<{ROWS}i', *buckets))
        for v in vectors: packed.extend(struct.pack(f'<{DIMENSION}f', *v))
        for bucket, q in queries: packed.extend(struct.pack(f'<i{DIMENSION}f', bucket, *q))
        output = subprocess.run([str(args.oracle)], input=packed, check=True, capture_output=True).stdout
        truth = [[int(v) for v in line.split()] for line in output.decode().splitlines()]
        rounds = []
        witnesses = []
        for round_number in range(3):
            order = [('old_c16935a', args.reference), ('ltc_dccb61e', args.binary)]
            if round_number%2: order.reverse()
            for label, binary in order:
                config = SimpleNamespace(binary=binary, image=None)
                with tempfile.TemporaryDirectory(prefix='ltc-prompt-bench-') as folder:
                    directory = Path(folder)
                    with Server(config, directory) as db:
                        db.ok(CREATE)
                        started = perf_counter()
                        for insert in inserts: db.ok(insert, 100)
                        write = perf_counter()-started
                    wal_bytes = (directory/'store.wal').stat().st_size
                    started = perf_counter()
                    with Server(config, directory) as db:
                        restart = perf_counter()-started
                        stored,count,error = db.sql('SELECT id,bucket,v FROM vectors ORDER BY id;')
                        assert error is None and count == ROWS
                        for i,row in enumerate(stored):
                            assert row[0] == i and row[1] == buckets[i]
                            assert struct.pack('<128f',*row[2]) == struct.pack('<128f',*vectors[i])
                        db.ok('CREATE VECTOR INDEX idx ON vectors(v);')
                        for statement in statements[:8]: db.sql(statement)
                        times = {}
                        responses = [None]*QUERIES
                        for group, indexes in (('unfiltered', range(0,QUERIES,2)), ('filtered', range(1,QUERIES,2))):
                            started = perf_counter()
                            for index in indexes: responses[index] = db.sql(statements[index])
                            times[group] = perf_counter()-started
                        quality = verify_queries(responses, truth, buckets, vectors, queries, 'exact')
                        if round_number == 0:
                            witnesses.extend({'variant': label, 'sql': statements[i], 'rows': responses[i][0], 'oracle_ids': truth[i]} for i in (0,1))
                rounds.append({'round':round_number+1,'variant':label,'write_seconds':write,'restart_seconds':restart,
                               'wal_bytes':wal_bytes, 'query_seconds':times, 'quality':quality,
                               'results_sha256':digest(responses)})
                print(f'{distribution} round {round_number+1} {label}: write={write:.4f}, query={sum(times.values()):.4f}',flush=True)
        assert len({r['results_sha256'] for r in rounds}) == 1
        summary = {}
        for label in ('old_c16935a', 'ltc_dccb61e'):
            subset=[r for r in rounds if r['variant']==label]
            summary[label]={'write_seconds':median(r['write_seconds'] for r in subset),
                            'query_seconds':median(sum(r['query_seconds'].values()) for r in subset),
                            'wal_bytes':median(r['wal_bytes'] for r in subset),
                            'restart_seconds':median(r['restart_seconds'] for r in subset)}
        reports.append({'distribution':distribution, 'rows':ROWS,'dimension':DIMENSION,'queries':QUERIES,
                        'dataset_sha256':hashlib.sha256(packed).hexdigest(),'rounds':rounds,'summary':summary,'witnesses':witnesses})
    return reports


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('binary','reference','oracle','output'): parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--benchmark-only', action='store_true')
    parser.add_argument('--resume', action='store_true', help='continue only unfinished extended cases from this exact binary')
    args=parser.parse_args()
    for key in ('binary','reference','oracle'): setattr(args,key,getattr(args,key).resolve(strict=True))
    args.image=None
    report={'role':'tool','recorded_at':datetime.now(timezone.utc).isoformat(),'platform':platform.platform(),
            'source_commit':'dccb61eb98b9f6a98cee30d59aca5684d9fedc15',
            'binary_sha256':hashlib.sha256(args.binary.read_bytes()).hexdigest(),
            'reference_binary_sha256':hashlib.sha256(args.reference.read_bytes()).hexdigest(),
            'scope':'macOS arm64 native experiments; no Linux cgroup limit or physical-I/O measurement', 'experiments':[]}
    if args.resume and args.output.exists():
        previous=json.loads(args.output.read_text())
        assert previous['binary_sha256']==report['binary_sha256']
        assert previous['reference_binary_sha256']==report['reference_binary_sha256']
        report=previous
    operations=[('multi-predicate-filtered-topk', lambda a,d: simple_filtered(a,d)),
                ('index-commit-and-ann-visibility',index_commit),('vector-index-crash-recovery',crash_vectors),
                ('signed-zero-float32-roundtrip',float_sign),('quantized-filter-boundaries',quantization),
                ('mixed-old-wal-compatibility',lambda a,d: mixed_wal(a,d)),('wide-storage-restart-sort',wide_storage)]
    if not args.benchmark_only:
        check_core.Server=TraceServer
        for name, operation in operations:
            if any(e['id']==name and e['status']=='PASS' for e in report['experiments']):
                continue
            events=[]
            TraceServer.events=events
            with tempfile.TemporaryDirectory(prefix='ltc-prompt-check-') as folder:
                metadata=operation(args,Path(folder))
            report['experiments'].append({'id':name,'role':'tool','status':'PASS','metadata':metadata,
                                          'output':json.dumps(events,ensure_ascii=False,separators=(',',':'))})
            args.output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
            print(name+': PASS',flush=True)
        TraceServer.events=None
    else:
        report['benchmark']=benchmark(args)
    report['status']='PASS'
    args.output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')


def simple_filtered(args,directory):
    with TraceServer(args,directory) as db:
        filtered(db)
        db.ok('CREATE VECTOR INDEX fi ON f(v);')
        db.query('SELECT id FROM f WHERE bucket>=1 AND id<5 ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 10;',[[2],[3]])
    with TraceServer(args,directory) as db:
        db.query('SELECT id FROM f WHERE bucket>=1 AND id<5 ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 10;',[[2],[3]])


def mixed_wal(args,directory):
    reference=SimpleNamespace(image=None,binary=args.reference)
    vectors=[[-0.0,65504.0,2**-14]+[0.125,-3.5]*7,
             [(-1)**i*(0.1+i*0.03125) for i in range(17)],
             [1e-39,3e38]+[0.2]*15,
             [-0.0,-15.875,0.125]+[0.75,-2.25]*7]
    def insert(db,key):
        db.ok(f"INSERT INTO formats VALUES ({key},{json.dumps(vectors[key])});",1)
    def verify(db):
        rows,count,error=db.sql('SELECT v FROM formats ORDER BY id;')
        assert error is None and count==4
        assert [struct.pack('<17f',*r[0]) for r in rows]==[struct.pack('<17f',*v) for v in vectors]
    with TraceServer(reference,directory) as db:
        db.ok('CREATE TABLE formats (id BIGINT PRIMARY KEY,v VECTOR(17));')
        insert(db,2)
        db.ok('CREATE VECTOR INDEX vi ON formats(v);')
    with TraceServer(args,directory) as db:
        for key in (0,1,3): insert(db,key)
        verify(db)
        db.ok('BEGIN;')
        db.ok('DELETE FROM formats WHERE id=0;',1)
        db.ok('ROLLBACK;')
        verify(db)
        db.ok('BEGIN;')
        db.ok('DELETE FROM formats WHERE id=1;',1)
        db.stop(crash=True)
    with TraceServer(args,directory,mode='ann') as db: verify(db)
    wal=(directory/'store.wal').read_bytes()
    offset=8
    kinds=set()
    frames=0
    while offset<len(wal):
        size,crc=struct.unpack_from('<II',wal,offset)
        payload=wal[offset+8:offset+8+size]
        assert len(payload)==size and zlib.crc32(payload)==crc
        kinds.add(payload[0])
        offset+=size+8
        frames+=1
    assert offset==len(wal) and {2,6,7,8}<=kinds
    check_core.check_crash(args,directory/'torn')
    return {'old_build':'c16935a engine from clean cdf66ba1d15dcea75a913d5672b6fc034e2de3d3',
            'new_build': 'dccb61eb98b9f6a98cee30d59aca5684d9fedc15', 'direction':'old WAL opened and extended by new build',
            'all_frame_crcs_valid':True,'frames':frames,'frame_kinds':sorted(kinds),'signed_zero_bit_check':True,
            'harness_correction':'Compare preserved signed-zero bits directly; do not apply the legacy JSON-parser zero normalization.'}


if __name__=='__main__': main()
