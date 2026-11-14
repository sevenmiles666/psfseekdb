"""Linux cgroup checks for the local optimizer changes (run on the guest host)."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
from types import SimpleNamespace

import check_core
import check_optimizations as optimized
import run_prompt_verification


def command(*args):
    return subprocess.check_output(args,text=True)


def main():
    p=argparse.ArgumentParser();p.add_argument('--image',required=True);p.add_argument('--binary',type=Path,required=True)
    p.add_argument('--oracle',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    args=p.parse_args();samples=[]
    original=check_core.Server
    class ResourceServer(original):
        def __init__(self,*a,**kw):
            self.limit=a[2] if len(a)>2 else kw.get('memory')
            self.sampled=False
            super().__init__(*a,**kw)
        def stop(self,crash=False):
            if self.limit and not self.sampled and self.process.poll() is None:
                inspect=json.loads(command('docker','inspect',self.container))[0]
                pid=inspect['State']['Pid']
                group=next(s.split(':',2)[2] for s in Path(f'/proc/{pid}/cgroup').read_text().splitlines() if s.startswith('0:'))
                cg=Path('/sys/fs/cgroup')/group.lstrip('/')
                peak=int((cg/'memory.peak').read_text());limit=int((cg/'memory.max').read_text())
                swap=int((cg/'memory.swap.max').read_text());events=(cg/'memory.events').read_text()
                assert 0<peak<=limit==33554432 and swap==0
                assert 'oom 0\n' in events and 'oom_kill 0\n' in events and not inspect['State']['OOMKilled']
                samples.append({'peak_bytes':peak,'limit_bytes':limit,'swap_limit_bytes':swap,
                                'memory_events':events,'guest_io_stat':(cg/'io.stat').read_text()})
                self.sampled=True
            return super().stop(crash=crash)
    check_core.Server=ResourceServer
    config=SimpleNamespace(image=args.image,binary=None)
    report={'status':'running','core':{},'samples':samples,'io_scope':'guest cgroup virtual block device; no host SSD measurement'}
    with tempfile.TemporaryDirectory(prefix='six-linux-',dir='/data/tmp') as folder:
        root=Path(folder)
        for name,operation in [('protocol',check_core.check_protocol),('sql',check_core.check_sql),('transactions',check_core.check_transactions),
                               ('crash',check_core.check_crash),('oracle',check_core.check_exact_oracle),('topk',check_core.check_topk_rounding),
                               ('wal',check_core.check_wal_vector_formats),('index',check_core.check_index)]:
            operation(config,root/name);report['core'][name]='PASS';print(name+' PASS',flush=True)
        check_core.check_disk_budget(config,root/'large');print('52 MiB under 32 MiB PASS',flush=True)
        vectors=optimized.data(20000,20261119);q=vectors[51]
        expected=optimized.truth(args.oracle,vectors,[(q,lambda i:True)])[0]
        with ResourceServer(config,root/'compressed',33554432) as db:
            optimized.create(db,'t');optimized.fill(db,'t',vectors)
            optimized.query(db,f'SELECT id,L2_DISTANCE(v,{optimized.vector_text(q)}) AS d FROM t ORDER BY d LIMIT 10;',expected,vectors,q)
            # This row can invalidate an otherwise compressible oversized cache.
            vectors[-1][7]=struct.unpack('<f',struct.pack('<f',.1234567))[0]
            db.ok(f'UPDATE t SET v={optimized.vector_text(vectors[-1])} WHERE id=19999;',1)
            expected=optimized.truth(args.oracle,vectors,[(q,lambda i:True)])[0]
            for _ in range(3):
                optimized.query(db,f'SELECT id,L2_DISTANCE(v,{optimized.vector_text(q)}) AS d FROM t ORDER BY d LIMIT 10;',expected,vectors,q)
        print('oversized compressed cache and FP32 rejection PASS',flush=True)
        native=SimpleNamespace(image=None,binary=args.binary,oracle=args.oracle)
        report['quantization']=run_prompt_verification.quantization(native,root/'quantization')
        print('60 quantization boundary queries PASS',flush=True)
        report['cache_switching']=optimized.boundaries(args.binary,args.oracle,root)
        print('grouped index switches and signed zero PASS',flush=True)
    report['status']='PASS';args.output.write_text(json.dumps(report,indent=2)+'\n')


if __name__=='__main__':main()
