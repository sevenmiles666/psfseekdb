"""Paired C ABI experiments; this is not the official SQL scoring harness."""
import os
os.environ.setdefault('OPENBLAS_NUM_THREADS', '8')
os.environ.setdefault('OMP_NUM_THREADS', '8')
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import resource
import threading
import time
import numpy as np
from gpu_monitor import gpu_memory_source

ROOT = None  # Set from the explicit --work-dir argument.


def digest(path):
    h = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(8 << 20), b''):
            h.update(block)
    return h.hexdigest()


def prepare(kind):
    dest = ROOT / 'data' / kind
    dest.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(20261102)
    if kind == 'sift250k':
        def fvec(path, rows):
            data = np.memmap(path, dtype=np.int32, mode='r').reshape(-1, 129)
            assert np.all(data[:rows, 0] == 128)
            return np.ascontiguousarray(data[:rows, 1:].view(np.float32))
        base = fvec(ROOT/'data/sift_base.fvecs', 250000)
        queries = fvec(ROOT/'data/sift_query.fvecs', 4096)
    elif kind == 'sift10k':
        # Use the public SIFT prefix; course .f32 files do not establish provenance.
        def fvec_prefix(path, rows):
            data=np.memmap(path,dtype=np.int32,mode='r').reshape(-1,129)
            assert len(data)>=rows and np.all(data[:rows,0]==128)
            return np.ascontiguousarray(data[:rows,1:].view(np.float32))
        base=fvec_prefix(ROOT/'data/sift_base.fvecs',10000)
        queries=fvec_prefix(ROOT/'data/sift_query.fvecs',100)
    elif kind == 'gaussian256':
        base = rng.normal(size=(250000,256)).astype(np.float32)
        queries = rng.normal(size=(4096,256)).astype(np.float32)
    elif kind == 'clustered256':
        centers = rng.normal(size=(1024,256)).astype(np.float32)
        base = centers[rng.integers(1024,size=250000)] + rng.normal(0, .5, (250000,256)).astype(np.float32)
        queries = centers[rng.integers(1024,size=4096)] + rng.normal(0, .5, (4096,256)).astype(np.float32)
    else:
        raise ValueError(kind)
    np.save(dest/'base.npy', base)
    np.save(dest/'queries.npy', queries)
    # Independent FP64 exact L2, chunked to avoid a nq*n distance matrix.
    truth = np.empty((len(queries),100),dtype=np.uint32)
    xb = base.astype(np.float64)
    norms = np.einsum('ij,ij->i',xb,xb)
    begin = time.perf_counter()
    for start in range(0,len(queries),64):
        q = queries[start:start+64].astype(np.float64)
        distances = np.einsum('ij,ij->i',q,q)[:,None] + norms[None,:] - 2*q@xb.T
        top = np.argpartition(distances,99,axis=1)[:,:100]
        # Deterministic distance then ID ordering for ties in public SIFT.
        for i, candidates in enumerate(top):
            truth[start+i] = candidates[np.lexsort((candidates,distances[i,candidates]))]
    np.save(dest/'truth.npy',truth)
    meta = {'name':kind,'rows':len(base),'dim':base.shape[1], 'nq':len(queries),
            'ground_truth':'independent FP64 exact L2 top100',
            'truth_seconds':time.perf_counter()-begin,
            'sha256':{p.name:digest(p) for p in dest.glob('*.npy')}}
    (dest/'dataset.json').write_text(json.dumps(meta,indent=2))
    print(json.dumps(meta),flush=True)


def run(lib_path, kind, label, rounds):
    dest=ROOT/'data'/kind
    base=np.load(dest/'base.npy',mmap_mode='r')
    queries=np.load(dest/'queries.npy',mmap_mode='r')
    truth=np.load(dest/'truth.npy')
    lib=C.CDLL(str(lib_path))
    fp=C.POINTER(C.c_float);up=C.POINTER(C.c_uint)
    lib.seekdb_cuvs_build.argtypes=[fp,C.c_long,C.c_long];lib.seekdb_cuvs_build.restype=C.c_void_p
    lib.seekdb_cuvs_search.argtypes=[C.c_void_p,fp,C.c_long,C.c_long,up,fp]
    lib.seekdb_cuvs_free.argtypes=[C.c_void_p]
    lib.seekdb_cuvs_serialize.argtypes=[C.c_void_p,C.c_char_p]
    lib.seekdb_cuvs_deserialize.argtypes=[C.c_char_p,C.c_long,C.c_long]
    lib.seekdb_cuvs_deserialize.restype=C.c_void_p
    def search(handle,q,k):
        ids=np.empty((len(q),k),dtype=np.uint32)
        distances=np.empty((len(q),k),dtype=np.float32)
        begin=time.perf_counter()
        rc=lib.seekdb_cuvs_search(handle,q.ctypes.data_as(fp),len(q),k,ids.ctypes.data_as(up),distances.ctypes.data_as(fp))
        elapsed=time.perf_counter()-begin
        assert rc==0, rc
        assert np.all(ids<len(base)) and np.all(np.isfinite(distances))
        assert all(len(set(row.tolist()))==k for row in ids)
        # Recompute returned L2 for a subset, outside all timers.
        exact=np.sum((base[ids[:16]].astype(np.float64)-q[:16,None,:].astype(np.float64))**2,axis=2)
        assert np.allclose(exact,distances[:16],rtol=1e-4,atol=.15)
        return ids,distances,elapsed
    ev=ROOT/'evidence';ev.mkdir(exist_ok=True)
    result={'label':label,'library_sha256':digest(lib_path),'benchmark_sha256':digest(Path(__file__)),'dataset':json.loads((dest/'dataset.json').read_text()),
            'scope':'C ABI only, not official SQL DDL scoring','rounds':[],'completed':False}
    result.update(gpu_memory_source()[1])
    target=ev/(kind+'-'+label+'.json')
    peak=[0];done=threading.Event()
    vram=[gpu_memory_source()[0]]
    def monitor():
        while not done.wait(.05):
            for p in vram:
                try:peak[0]=max(peak[0],int(p.read_text()))
                except OSError:pass
    monitor_thread=threading.Thread(target=monitor);monitor_thread.start()
    try:
        for rnd in range(rounds):
            row={};handle=None
            try:
                begin=time.perf_counter()
                handle=lib.seekdb_cuvs_build(base.ctypes.data_as(fp),len(base),base.shape[1])
                assert handle
                row['build_seconds']=time.perf_counter()-begin
                _,_,single=search(handle,queries[:1],10)
                row['build_plus_first_query_seconds']=row['build_seconds']+single
                row['topk']={}
                for k in (10,100):
                    search(handle,queries,k)
                    times=[]
                    for _ in range(3):
                        ids,distances,elapsed=search(handle,queries,k);times.append(elapsed)
                    recall=np.mean([len(set(got.tolist())&set(want[:k].tolist()))/k for got,want in zip(ids,truth)])
                    np.save(ev/(kind+'-'+label+'-round'+str(rnd)+'-k'+str(k)+'-ids.npy'),ids)
                    row['topk'][str(k)]={'recall':float(recall),'seconds':times,'median_qps':len(queries)/float(np.median(times))}
                # Round trip with same public C ABI and snapshot format.
                if rnd==0:
                    snap=ROOT/(kind+'-'+label+'.cagra')
                    assert lib.seekdb_cuvs_serialize(handle,str(snap).encode())==0
                    before,_,_=search(handle,queries,100)
                    lib.seekdb_cuvs_free(handle);handle=None
                    handle=lib.seekdb_cuvs_deserialize(str(snap).encode(),len(base),base.shape[1]);assert handle
                    after,_,_=search(handle,queries,100)
                    before_recall=float(np.mean([len(set(got.tolist())&set(want.tolist()))/100 for got,want in zip(before,truth)]))
                    after_recall=float(np.mean([len(set(got.tolist())&set(want.tolist()))/100 for got,want in zip(after,truth)]))
                    # ANN traversal and equal-distance SIFT ties can change the
                    # returned order/set even for the unmodified frozen V5.
                    # Check complete results and paired quality, not byte equality.
                    assert after_recall >= before_recall-.005
                    row['persistence']={'before_recall100':before_recall,'after_recall100':after_recall,
                                        'identical_sets':sum(set(a.tolist())==set(b.tolist()) for a,b in zip(before,after)),
                                        'checked_queries':len(queries)}
                result['rounds'].append(row)
                target.write_text(json.dumps(result,indent=2));print(json.dumps(row),flush=True)
            finally:
                if handle:lib.seekdb_cuvs_free(handle)
        result['completed']=True
    finally:
        done.set();monitor_thread.join()
        result['peak_vram_device_bytes']=peak[0]
        result['peak_rss_bytes']=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss*1024
        target.write_text(json.dumps(result,indent=2))


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--work-dir',type=Path,required=True);p.add_argument('--prepare',choices=['sift250k','sift10k','gaussian256','clustered256']);p.add_argument('--dataset');p.add_argument('--lib',type=Path);p.add_argument('--label');p.add_argument('--rounds',type=int,default=2)
    a=p.parse_args()
    ROOT=a.work_dir.resolve()
    if a.rounds < 1:p.error('--rounds must be positive')
    if not a.prepare and not all((a.dataset,a.lib,a.label)):p.error('measurement requires --dataset, --lib and --label')
    if a.prepare:prepare(a.prepare)
    else:run(a.lib,a.dataset,a.label,a.rounds)
