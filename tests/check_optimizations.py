"""Behavioral checks and paired workloads for the six local optimizations."""
import argparse
from array import array
import hashlib
import json
import math
from pathlib import Path
import random
import shutil
from statistics import median
import struct
import subprocess
import tempfile
import time
from types import SimpleNamespace

from run_prompt_verification import TraceServer as Server


def configuration(binary):
    return SimpleNamespace(binary=binary.resolve(), image=None)


def vector_text(v):
    return '[' + ','.join('-0' if x == 0 and math.copysign(1,x)<0 else str(x) for x in v) + ']'


def create(db, name):
    db.ok(f'CREATE TABLE {name} (id BIGINT PRIMARY KEY,a BIGINT,b BIGINT,note VARCHAR(8192),v VECTOR(128));')


def fill(db, name, vectors, width=0):
    for start in range(0,len(vectors),100):
        values=[]
        for i in range(start,min(start+100,len(vectors))):
            note=str(i)+('x'*width)
            values.append(f"({i},{i%2},{i%251},'{note}',{vector_text(vectors[i])})")
        db.ok(f'INSERT INTO {name} VALUES '+','.join(values)+';',len(values))


def data(rows, seed):
    rng=random.Random(seed)
    vectors=[]
    for i in range(rows):
        v=[rng.randrange(-64,64)/8 for _ in range(128)]
        if i%17==0:v[0]=-0.0
        vectors.append(v)
    return vectors


def truth(oracle,vectors,queries):
    # Each query carries the complete predicate evaluated independently in Python.
    out=[]
    for vector,predicate in queries:
        buckets=array('i',(1 if predicate(i) else 0 for i in range(len(vectors))))
        body=struct.pack('<3I',len(vectors),128,1)+buckets.tobytes()
        body+=array('f',(x for v in vectors for x in v)).tobytes()
        body+=struct.pack('<i',1)+array('f',vector).tobytes()
        result=subprocess.run([str(oracle)],input=body,capture_output=True,check=True)
        out.append([int(x) for x in result.stdout.split()])
    return out


def query(db,statement,expected,vectors,query_vector):
    rows,count,error=db.sql(statement)
    assert error is None and count==len(expected),(statement,error,count)
    assert [r[0] for r in rows]==expected,(statement,rows,expected)
    for key,distance in rows:
        actual=math.sqrt(sum((x-y)**2 for x,y in zip(vectors[key],query_vector)))
        assert math.isclose(distance,actual,rel_tol=1e-12,abs_tol=1e-12)
    return rows


def workload(binary,kind,vectors,queries,expected,directory):
    with Server(configuration(binary),directory) as db:
        names=['t','u'] if kind=='multiple-tables' else ['t']
        for name in names: create(db,name);fill(db,name,vectors,4096 if kind=='wide-projection' else 0)
        if kind=='ann':db.ok('CREATE VECTOR INDEX vi ON t(v);');db.mode='ann'
        output=[]
        # Include construction, path changes and first query in the timed sequence.
        started=time.perf_counter()
        for turn in range(80):
            i=turn%len(queries);q,predicate=queries[i]
            name=names[turn%len(names)]
            where=''
            if kind=='filter':
                # Warm the broad column, then alternate equivalent AND orders and
                # force switches back to it after narrow-column grouping.
                if turn==0:where=' WHERE a=0'
                elif turn%2:where=f' WHERE a=0 AND b={i}'
                else:where=f' WHERE b={i} AND a=0'
            if kind=='short-lived':
                db.ok(f"UPDATE t SET note='{turn}' WHERE id=0;",1)
            if kind=='filter' and turn==0:
                # This query has a different predicate and is checked separately below.
                broad=db.sql(f'SELECT id FROM t{where} ORDER BY L2_DISTANCE(v,{vector_text(q)}) LIMIT 10;')
                assert broad[2] is None and all(r[0]%2==0 for r in broad[0])
                continue
            sql=f'SELECT id,L2_DISTANCE(v,{vector_text(q)}) AS d FROM {name}{where} ORDER BY d LIMIT 10;'
            output.append((i,db.sql(sql)))
        seconds=time.perf_counter()-started
        for i,(rows,count,error) in output:
            assert error is None and count==len(expected[i])
            assert [r[0] for r in rows]==expected[i],(kind,i,rows,expected[i])
            for key,distance in rows:
                actual=math.sqrt(sum((x-y)**2 for x,y in zip(vectors[key],queries[i][0])))
                assert math.isclose(distance,actual,rel_tol=1e-12,abs_tol=1e-12)
        if kind=='wide-projection':
            key=expected[0][0]
            rows,_,error=db.sql(f'SELECT id,note,v FROM t ORDER BY L2_DISTANCE(v,{vector_text(queries[0][0])}) LIMIT 1;')
            assert error is None and rows[0][0]==key and rows[0][1]==str(key)+'x'*4096
            assert struct.pack('<128f',*rows[0][2])==struct.pack('<128f',*vectors[key])
        return {'seconds':seconds,'queries_checked':len(output),
                'results_sha256':hashlib.sha256(json.dumps(output,separators=(',',':')).encode()).hexdigest()}


def boundaries(binary,oracle,root):
    vectors=data(2401,20261117)
    config=configuration(binary)
    # Late lossless-encoding failure after many encoded rows; all projections and
    # nonresident predicate columns remain exact through rollback and restart.
    vectors[-1][7]=struct.unpack('<f',struct.pack('<f',0.1234567))[0]
    qs=[(vectors[17],lambda i:i%2==0 and i%251==0)]
    expected=truth(oracle,vectors,qs)[0]
    directory=root/'boundary'
    with Server(config,directory) as db:
        create(db,'t');fill(db,'t',vectors,4096)
        for _ in range(12):
            query(db,f'SELECT id,L2_DISTANCE(v,{vector_text(qs[0][0])}) AS d FROM t WHERE a=0 AND b=0 ORDER BY d LIMIT 10;',expected,vectors,qs[0][0])
        db.ok('BEGIN;');db.ok("UPDATE t SET note='changed',a=1 WHERE id=0;",1)
        db.sql(f'SELECT note FROM t WHERE a=1 ORDER BY L2_DISTANCE(v,{vector_text(vectors[0])}) LIMIT 3;')
        db.ok('ROLLBACK;')
        row,_,_=db.sql('SELECT note FROM t WHERE id=0;');assert row==[['0'+'x'*4096]]
    with Server(config,directory) as db:
        query(db,f'SELECT id,L2_DISTANCE(v,{vector_text(qs[0][0])}) AS d FROM t WHERE b=0 AND a=0 ORDER BY d LIMIT 10;',expected,vectors,qs[0][0])
    # Exercise both directions of the in-place row permutation, including the
    # primary-order negative-zero bitmap in the lossless byte representation.
    vectors=data(2401,20261120);q=vectors[34]
    broad,narrow=truth(oracle,vectors,[(q,lambda i:i%2==0),(q,lambda i:i%2==0 and i%251==0)])
    with Server(config,root/'switching') as db:
        create(db,'t');fill(db,'t',vectors)
        for where,expected in [('a=0',broad)]*2+[('a=0 AND b=0',narrow)]*8+[('a=0',broad)]*2:
            query(db,f'SELECT id,L2_DISTANCE(v,{vector_text(q)}) AS d FROM t WHERE {where} ORDER BY d LIMIT 10;',expected,vectors,q)
        rows,_,error=db.sql(f'SELECT id,v FROM t WHERE a=0 ORDER BY L2_DISTANCE(v,{vector_text(q)}) LIMIT 1;')
        assert error is None and rows[0][0]==34
        assert struct.pack('<128f',*rows[0][1])==struct.pack('<128f',*vectors[34])
    return {'status':'PASS','rows':len(vectors),'mixed_fp32_last_row':True,'wide_columns':True,
            'rollback_restart':True,'grouped_index_switch_and_signed_zero':'PASS'}


def recovery(reference,candidate,root):
    seed=root/'history';config=configuration(reference)
    with Server(config,seed) as db:
        db.ok('CREATE TABLE h (id BIGINT PRIMARY KEY,note VARCHAR(8192),v VECTOR(2));')
        for start in range(0,1200,100):
            db.ok('INSERT INTO h VALUES '+','.join(f"({i},'{'z'*2048}',[{i%7},-0])" for i in range(start,start+100))+';',100)
        db.ok('CREATE VECTOR INDEX vi ON h(v);')
        for turn in range(5):db.ok(f"UPDATE h SET note='{'y'*2048}{turn}';",1200)
    before=(seed/'store.wal').stat().st_size
    # A leftover incomplete replacement must never become authoritative.
    (seed/'store.wal.next').write_bytes(b'incomplete checkpoint')
    durations={};sizes={}
    for label,binary in [('reference',reference),('candidate',candidate)]:
        directory=root/('restart-'+label);shutil.copytree(seed,directory)
        times=[]
        for _ in range(3):
            start=time.perf_counter()
            with Server(configuration(binary),directory) as db:
                times.append(time.perf_counter()-start)
                db.query('SELECT id FROM h WHERE id=1199;',[[1199]])
                db.query('SELECT note FROM h WHERE id=0;',[['y'*2048+'4']])
                db.ok('BEGIN;');db.ok("UPDATE h SET note='discarded' WHERE id=0;",1);db.ok('ROLLBACK;')
        durations[label]=times;sizes[label]=(directory/'store.wal').stat().st_size
    assert sizes['candidate']<before/3,(before,sizes)
    # Kill while writing the replacement: either old or new WAL must contain all
    # acknowledged rows. No fault hooks are built into the database.
    interrupted=root/'interrupted';shutil.copytree(seed,interrupted)
    (interrupted/'store.wal.next').unlink()
    process=subprocess.Popen([str(candidate),'--data-dir',str(interrupted)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    caught=False;deadline=time.monotonic()+10
    while time.monotonic()<deadline and process.poll() is None:
        path=interrupted/'store.wal.next'
        try:size=path.stat().st_size
        except FileNotFoundError:size=0
        if size>8192:
            process.kill();caught=True;break
        time.sleep(0.001)
    if not caught:process.kill()
    process.communicate(timeout=10)
    with Server(configuration(candidate),interrupted) as db:
        db.query('SELECT id FROM h ORDER BY id;',[[i] for i in range(1200)])
        db.query('SELECT note FROM h WHERE id=0;',[['y'*2048+'4']])
        db.ok("UPDATE h SET note='after-checkpoint' WHERE id=0;",1)
        db.stop(crash=True)
    with Server(configuration(candidate),interrupted) as db:
        db.query('SELECT note FROM h WHERE id=0;',[['after-checkpoint']])
    assert caught,'checkpoint finished before interruption; use a larger fixture'
    online=root/'online';shutil.copytree(seed,online)
    sizes_after_commits=[]
    with Server(configuration(candidate),online) as db:
        sizes_after_commits.append((online/'store.wal').stat().st_size)
        for turn in range(7):
            db.ok(f"UPDATE h SET note='{'w'*2048}{turn}';",1200)
            sizes_after_commits.append((online/'store.wal').stat().st_size)
        assert any(b<a for a,b in zip(sizes_after_commits,sizes_after_commits[1:]))
        db.ok('BEGIN;')
        for _ in range(4):db.ok(f"UPDATE h SET note='{'q'*2048}';",1200)
        db.ok('COMMIT;')
        db.stop(crash=True)
    with Server(configuration(candidate),online) as db:
        db.query('SELECT id FROM h ORDER BY id;',[[i] for i in range(1200)])
        db.query('SELECT note FROM h WHERE id=1199;',[['q'*2048]])
    return {'status':'PASS','original_bytes':before,'final_bytes':sizes,'ready_seconds':durations,
            'steady_ready_median':{k:median(v[1:]) for k,v in durations.items()},'killed_during_replacement':caught,
            'online_commit_wal_sizes':sizes_after_commits,'explicit_commit_checkpoint_restart':'PASS'}


def main():
    p=argparse.ArgumentParser();p.add_argument('--reference',type=Path,required=True);p.add_argument('--candidate',type=Path,required=True)
    p.add_argument('--oracle',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--rounds',type=int,default=3)
    args=p.parse_args()
    report={'status':'running','reference_sha256':hashlib.sha256(args.reference.read_bytes()).hexdigest(),
            'candidate_sha256':hashlib.sha256(args.candidate.read_bytes()).hexdigest(),'cases':{}}
    with tempfile.TemporaryDirectory(prefix='mvdb-optimizations-') as folder:
        root=Path(folder)
        report['boundaries']=boundaries(args.candidate,args.oracle,root);print('boundaries PASS',flush=True)
        report['recovery']=recovery(args.reference,args.candidate,root);print('recovery PASS',flush=True)
        for kind,rows in [('compressed-admission',20000),('filter',10000),('wide-projection',3000),('multiple-tables',4000),('short-lived',4000),('ann',10000)]:
            vectors=data(rows,20261117)
            rng=random.Random(20261118)
            queries=[([rng.randrange(-64,64)/8 for _ in range(128)],
                      (lambda k,b=i:k%2==0 and k%251==b) if kind=='filter' else (lambda k:True)) for i in range(4)]
            expected=truth(args.oracle,vectors,queries)
            rounds=[]
            for turn in range(args.rounds):
                entry={}
                pairs=[('reference',args.reference),('candidate',args.candidate)]
                if turn%2:pairs.reverse()
                for label,binary in pairs:
                    entry[label]=workload(binary,kind,vectors,queries,expected,root/f'{kind}-{turn}-{label}')
                assert entry['reference']['results_sha256']==entry['candidate']['results_sha256']
                rounds.append(entry)
            summary={label:median(e[label]['seconds'] for e in rounds) for label in ('reference','candidate')}
            summary['speedup']=summary['reference']/summary['candidate']
            report['cases'][kind]={'rounds':rounds,'summary':summary,'rows':rows,'dimension':128}
            args.output.write_text(json.dumps(report,indent=2)+'\n')
            print(kind,summary,flush=True)
    report['status']='PASS';args.output.write_text(json.dumps(report,indent=2)+'\n')


if __name__=='__main__':main()
