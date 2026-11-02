"""Real SQL DDL/first-query paired validation of a drop-in C ABI bridge."""
import os
os.environ.setdefault('OPENBLAS_NUM_THREADS','8')
os.environ.setdefault('OMP_NUM_THREADS','8')
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import socket
import threading
import time
import numpy as np
import pymysql

ROOT=None
BINARY=None
PORT=2902


def sha(path):
    h=hashlib.sha256()
    with open(path,'rb') as f:
        for b in iter(lambda:f.read(8<<20),b''):h.update(b)
    return h.hexdigest()


def run(label,bridge,datasets,nq):
    assert bridge.name=='libseekdb_cuvs_bridge.so', 'Use the bridge SONAME as filename so LD_LIBRARY_PATH selects the intended library'
    with socket.socket() as probe:
        probe.settimeout(1)
        assert probe.connect_ex(('127.0.0.1',PORT))!=0, 'SQL port is occupied; choose another --port'
    db=Path('/var/tmp')/('amd-v5-perf-'+label)
    assert not db.exists(),'Refuse to reuse database directory: '+str(db)
    db.mkdir()
    ev=ROOT/'evidence';ev.mkdir(exist_ok=True)
    env=dict(os.environ,LD_LIBRARY_PATH=str(bridge.parent)+':/opt/hipvs/lib:/opt/rocm/lib')
    log=(ev/('sql-'+label+'-console.log')).open('w')
    args=[str(BINARY),'--nodaemon','--base-dir',str(db),'--data-dir',str(db/'store'),
          '--port',str(PORT),'--parameter','memory_budget=8G','--parameter','cpu_count=16',
          '--parameter','datafile_size=4G','--parameter','datafile_maxsize=12G',
          '--parameter','log_disk_size=2G','--parameter','local_ip=127.0.0.1',
          '--parameter','__min_full_resource_pool_memory=2147483648','--parameter','syslog_level=INFO']
    proc=None;conn=None
    report={'completed':False,'label':label,'bridge_sha256':sha(bridge),'seekdb_sha256':sha(BINARY),
            'benchmark_sha256':sha(Path(__file__)),
            'scope':'real cold SQL DDL through first legal query; unchanged V5 main executable, candidate bridge rebuilt from source',
            'datasets':[]}
    target=ev/('sql-'+label+'.json')
    done=threading.Event()
    def monitor():
        vram=list(Path('/sys/class/drm').glob('card*/device/mem_info_vram_used'))
        while not done.wait(.05):
            for path in vram:
                try:report['peak_vram_device_bytes']=max(report.get('peak_vram_device_bytes',0),int(path.read_text()))
                except OSError:pass
            if proc is not None:
                try:
                    for line in Path(f'/proc/{proc.pid}/status').read_text().splitlines():
                        if line.startswith('VmHWM:'):
                            report['peak_seekdb_rss_bytes']=max(report.get('peak_seekdb_rss_bytes',0),int(line.split()[1])*1024)
                except OSError:pass
    monitor_thread=threading.Thread(target=monitor);monitor_thread.start()
    def save():target.write_text(json.dumps(report,indent=2))
    def sql(statement):
        with conn.cursor() as c:
            c.execute(statement);return c.fetchall()
    def query(table,q,k):
        v='['+','.join(format(float(x),'.9g') for x in q)+']'
        return [int(x[0]) for x in sql(f"select id from {table} order by l2_distance(v,'{v}') approximate limit {k}")]
    saved=[]
    try:
        proc=subprocess.Popen(args,env=env,stdout=log,stderr=subprocess.STDOUT)
        deadline=time.monotonic()+180
        while True:
            assert proc.poll() is None,'seekdb exited'
            try:
                conn=pymysql.connect(host='127.0.0.1',port=PORT,user='root',autocommit=True,local_infile=True,read_timeout=600)
                break
            except pymysql.err.OperationalError:
                if time.monotonic()>deadline:raise
                time.sleep(1)
        sql('set ob_query_timeout=600000000');sql('create database perf');sql('use perf')
        mapped={Path(line.split()[-1]).resolve() for line in Path(f'/proc/{proc.pid}/maps').read_text().splitlines() if line.split()[-1].endswith('/libseekdb_cuvs_bridge.so')}
        assert mapped=={bridge.resolve()}, 'Unexpected bridge mapping: '+str(mapped)
        report['loaded_bridge_verified']=True
        for kind in datasets:
            dest=ROOT/'data'/kind;base=np.load(dest/'base.npy',mmap_mode='r');q=np.load(dest/'queries.npy',mmap_mode='r')[:nq];truth=np.load(dest/'truth.npy')[:nq]
            ids=1000003+np.arange(len(base),dtype=np.int64)*7
            data_file=dest/'vectors.tsv'
            if not data_file.exists():
                with data_file.open('w') as out:
                    for pk,row in zip(ids,base):out.write(str(int(pk))+'\t['+','.join(format(float(x),'.9g') for x in row)+']\n')
            table='items_'+kind
            sql(f'create table {table}(id bigint primary key,v vector({base.shape[1]}))')
            sql(f"load data local infile '{data_file}' into table {table} fields terminated by '\\t' lines terminated by '\\n' (id,v)")
            assert sql(f'select count(*) from {table}')[0][0]==len(base)
            begin=time.perf_counter()
            sql(f'create vector index idx on {table}(v) with(distance=l2,type=hnsw,lib=cuvs,m=16,ef_construction=200,ef_search=64)')
            ddl=time.perf_counter()-begin
            first=query(table,q[0],10)
            build=time.perf_counter()-begin
            row={'name':kind,'rows':len(base),'dim':base.shape[1],'nq':len(q),'ddl_seconds':ddl,'ddl_plus_first_query_seconds':build,'topk':{}}
            for k in (10,100):
                begin=time.perf_counter();got=[query(table,vector,k) for vector in q];elapsed=time.perf_counter()-begin
                assert all(len(x)==k and len(set(x))==k for x in got)
                valid=set(map(int,ids));assert all(all(y in valid for y in x) for x in got)
                recall=np.mean([len(set(x)&set(map(int,ids[t[:k]])))/k for x,t in zip(got,truth)])
                row['topk'][str(k)]={'recall':float(recall),'qps':len(q)/elapsed,'seconds':elapsed}
                (ev/(kind+'-'+label+'-sql-k'+str(k)+'-ids.json')).write_text(json.dumps(got))
            saved.append((table,q[0].copy(),set(map(int,ids[truth[0]])),set(query(table,q[0],100))))
            report['datasets'].append(row);save();print(json.dumps(row),flush=True)
        sql('create table cpu_items(id int primary key,v vector(2))')
        sql('insert into cpu_items values(1,"[1,0]"),(2,"[0,1]"),(3,"[2,0]")')
        sql('create vector index idx on cpu_items(v) with(distance=l2,type=hnsw,lib=vsag)')
        assert query('cpu_items',np.array([1,0],dtype=np.float32),1)==[1]
        report['cpu_vsag_passed']=True
        conn.close();conn=None;proc.terminate();proc.wait(timeout=30)
        proc=subprocess.Popen(args,env=env,stdout=log,stderr=subprocess.STDOUT)
        deadline=time.monotonic()+180
        while True:
            assert proc.poll() is None
            try:
                conn=pymysql.connect(host='127.0.0.1',port=PORT,user='root',autocommit=True,read_timeout=600);break
            except pymysql.err.OperationalError:
                if time.monotonic()>deadline:raise
                time.sleep(1)
        sql('set ob_query_timeout=600000000');sql('use perf')
        report['reload_queries']=[]
        for table,q,truth,previous in saved:
            current=set(query(table,q,100))
            assert len(current)==100
            before=len(previous&truth)/100;after=len(current&truth)/100
            assert after >= before-.01
            report['reload_queries'].append({'table':table,'before_recall100':before,'after_recall100':after,'identical_set':current==previous})
        report['reload_validated']=True;report['completed']=True;save()
    finally:
        done.set();monitor_thread.join()
        save()
        if conn:conn.close()
        if proc and proc.poll() is None:
            proc.terminate()
            try:proc.wait(timeout=30)
            except subprocess.TimeoutExpired:proc.kill();proc.wait()
        log.close()


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--work-dir',type=Path,required=True);p.add_argument('--binary',type=Path,required=True);p.add_argument('--port',type=int,default=2902);p.add_argument('--label',required=True);p.add_argument('--bridge',type=Path,required=True);p.add_argument('--datasets',nargs='+',default=['sift250k','gaussian256']);p.add_argument('--nq',type=int,default=256)
    a=p.parse_args()
    if a.nq < 1:p.error('--nq must be positive')
    ROOT=a.work_dir.resolve();BINARY=a.binary.resolve();PORT=a.port
    run(a.label,a.bridge.resolve(),a.datasets,a.nq)
