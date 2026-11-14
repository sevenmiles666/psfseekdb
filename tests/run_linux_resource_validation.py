"""Run inside the existing local UTM guest; gather cgroup resource evidence."""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
from time import perf_counter
from types import SimpleNamespace


def command(*args):
    return subprocess.run(list(args), check=True, capture_output=True, text=True).stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--skip-core', action='store_true')
    args = parser.parse_args()
    work = args.work.resolve()
    sys.path.insert(0, str(work/'tests'))
    import check_core
    import benchmark_local
    events = []
    samples = []
    helper = work/'build/benchmark_support'
    image = 'minivectordb:ltc-prompt-20261114'
    report = {
        'role':'tool', 'recorded_at_guest':datetime.now(timezone.utc).isoformat(),
        'platform':platform.platform(), 'architecture':platform.machine(),
        'host_note':'UTM x86_64 QEMU emulation on Apple Silicon; local resource verification, not official performance.',
        'source_commit':'dccb61eb98b9f6a98cee30d59aca5684d9fedc15',
        'source_sha256':{str(p.relative_to(work)):hashlib.sha256(p.read_bytes()).hexdigest()
                         for p in sorted((work/'src').iterdir()) if p.is_file()},
        'compiler':(work/'build/compiler.txt').read_text().strip(),
        'compiler_difference':'Cached GCC 13.3.0 used for local Linux checks; repository Dockerfile specifies 13.4.0 and was not modified.',
        'binary_sha256':hashlib.sha256((work/'build/minivectordb').read_bytes()).hexdigest(),
        'probe_sha256':hashlib.sha256(helper.read_bytes()).hexdigest(),
        'image_inspect':json.loads(command('docker','image','inspect',image))[0],
        'core_regression':'running', 'large_storage':{}, 'workload_resources':[],
    }
    output=work/'linux-validation.json'

    def cgroup_sample(db):
        inspect=json.loads(command('docker','inspect',db.container))[0]
        pid=inspect['State']['Pid']
        group=next(s.split(':',2)[2] for s in Path(f'/proc/{pid}/cgroup').read_text().splitlines() if s.startswith('0:'))
        cgroup=Path('/sys/fs/cgroup')/group.lstrip('/')
        mount=next(m for m in inspect['Mounts'] if m['Destination']=='/data')
        storage=Path(mount['Source'])/'store.wal'
        dev=storage.stat().st_dev
        device=f'{os.major(dev)}:{os.minor(dev)}'
        devices={}
        for line in (cgroup/'io.stat').read_text().splitlines():
            fields=line.split()
            devices[fields[0]]={k:int(v) for k,v in (entry.split('=',1) for entry in fields[1:])}
        parent_controllers=(cgroup.parent/'cgroup.subtree_control').read_text().split()
        assert 'io' in parent_controllers, ('I/O controller not enabled',parent_controllers)
        counters=devices.get(device)
        # An entirely cached read can leave io.stat empty. With an enabled I/O
        # controller this records zero charged block I/O, not a missing metric.
        if counters is None:
            counters={'rbytes':0,'wbytes':0,'rios':0,'wios':0,'dbytes':0,'dios':0}
        assert 'rbytes' in counters and 'wbytes' in counters
        peak=int((cgroup/'memory.peak').read_text());limit=int((cgroup/'memory.max').read_text())
        swap=int((cgroup/'memory.swap.max').read_text())
        assert limit==inspect['HostConfig']['Memory'] and swap==0 and 0<peak<=limit
        assert not inspect['State']['OOMKilled']
        return {'data_device':device,'memory_peak_bytes':peak,'memory_limit_bytes':limit,'swap_limit_bytes':swap,
                'persisted_file_bytes':storage.stat().st_size,'data_file_bytes':storage.stat().st_size,
                'read_bytes':counters['rbytes'],'write_bytes':counters['wbytes'],
                'io_bytes':counters['rbytes']+counters['wbytes'],'io_stat':devices,
                'cgroup_path':str(cgroup),'memory_events':(cgroup/'memory.events').read_text(),
                'io_controller_enabled':True,'data_device_present_in_io_stat':device in devices,
                'docker_state':inspect['State'], 'measurement_note':'Read by guest host; no probe process charged to measured cgroup.'}

    class ResourceServer(check_core.Server):
        def __init__(self, config, data, memory=None, *, mode='exact', volume=None, probe=None):
            self.resource_memory=memory
            self.resource_recorded=False
            start=perf_counter()
            super().__init__(config,data,memory,mode=mode,volume=volume,probe=helper if memory else probe)
            if memory:
                events.append({'event':'ready','search_mode':mode,'ready_seconds':perf_counter()-start,
                               'memory_limit_requested':memory})

        def request(self,message,*,ensure_ascii=False):
            response=super().request(message,ensure_ascii=ensure_ascii)
            if self.resource_memory and message.get('sql','').startswith('SELECT'):
                events.append({'request':message,'response':response})
            return response

        def stop(self,crash=False):
            if not self.resource_recorded and self.resource_memory and self.process.poll() is None:
                sample=cgroup_sample(self)
                sample['search_mode']=self.mode
                samples.append(sample)
                (work/'linux-resource-samples.json').write_text(json.dumps(samples,indent=2)+'\n')
                self.resource_recorded=True
            return super().stop(crash=crash)

    check_core.Server=ResourceServer
    benchmark_local.resources=cgroup_sample
    config=SimpleNamespace(image=image,binary=None)
    with tempfile.TemporaryDirectory(prefix='ltc-prompt-linux-',dir='/data/tmp') as folder:
        base=Path(folder)
        for name,operation in [('protocol',check_core.check_protocol),('sql',check_core.check_sql),
                               ('transactions',check_core.check_transactions),('crash',check_core.check_crash),
                               ('oracle',check_core.check_exact_oracle),('topk',check_core.check_topk_rounding),
                               ('wal',check_core.check_wal_vector_formats),('index',check_core.check_index)]:
            if args.skip_core:
                continue
            operation(config,base/name)
            print(name+': PASS',flush=True)
        if args.skip_core:
            old=(work/'linux-core-and-probe-failure.log').read_text()
            assert all(name+': PASS' in old for name in ('protocol','sql','transactions','crash','oracle','topk','wal','index'))
            report['core_regression_source']='linux-core-and-probe-failure.log before resource-instrumentation failure'
        report['core_regression']='PASS'
        check_core.check_disk_budget(config,base/'large')
        assert len(samples)==3 and all(s['memory_limit_bytes']==33554432 for s in samples)
        assert all(s['persisted_file_bytes']>33554432 for s in samples)
        report['large_storage']={'status':'PASS','samples':list(samples),'output':json.dumps(events,separators=(',',':')),
                                 'scope':'32 MiB hard limit, zero swap; 1600 wide rows; restart, index rebuild, exact/ann and complete sort.'}
        output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
        print('32 MiB resource verification: PASS',flush=True)

    # One measured correctness/resource workload per distribution. These are resource
    # checks; this does not claim the three-round paired performance comparison.
    benchmark_local.ROWS=2400
    benchmark_local.QUERIES=100
    for distribution in ('grid','continuous'):
        buckets,vectors,queries=benchmark_local.dataset(20261114,distribution)
        inserts,statements=benchmark_local.input_statements(buckets,vectors,queries)
        truth,input_hash=benchmark_local.exact_truth(image,helper,buckets,vectors,queries)
        write=benchmark_local.run_write(image,helper,inserts,buckets,vectors)
        # The library quality helper requires 500 queries per group. Validate this
        # smaller resource workload independently, preserving the full responses.
        with benchmark_local.fresh_volume() as volume:
            with ResourceServer(config,None,benchmark_local.MEMORY,volume=volume,probe=helper) as db:
                db.ok(benchmark_local.CREATE)
                for insert in inserts:db.ok(insert,100)
                db.ok('CREATE VECTOR INDEX vi ON vectors(v);')
                for statement in statements[:8]:db.sql(statement)
                started=perf_counter()
                responses=[db.sql(statement) for statement in statements]
                seconds=perf_counter()-started
                counters=benchmark_local.resources(db)
        import math
        for (rows,count,error),expected,(bucket,query) in zip(responses,truth,queries):
            assert error is None and count==len(rows)==10
            assert [r[0] for r in rows]==expected
            assert bucket<0 or all(buckets[r[0]]==bucket for r in rows)
            for key,distance in rows:
                exact=math.sqrt(sum((a-b)**2 for a,b in zip(vectors[key],query)))
                assert math.isclose(distance,exact,rel_tol=1e-12,abs_tol=1e-12)
        assert write['io_bytes'] is not None and counters['io_bytes'] is not None
        report['workload_resources'].append({'distribution':distribution,'role':'tool','status':'PASS',
            'rows':2400,'dimension':128,'queries':100,'dataset_sha256':input_hash,
            'write':write,'query':{'seconds':seconds,**counters},'minimum_recall_at_10':1.0,
            'witnesses':[{'sql':statements[i],'rows':responses[i][0],'oracle_ids':truth[i]} for i in (0,1)],
            'results_sha256':hashlib.sha256(json.dumps(responses,separators=(',',':')).encode()).hexdigest()})
        output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')
        print(distribution+' cgroup I/O and result check: PASS',flush=True)
    report['status']='PASS'
    report['io_scope']='Guest Linux cgroup block-device counters; data volume and Docker image share the guest virtual disk. Counters do not measure host SSD physical I/O; no host cache reset.'
    output.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n')


if __name__=='__main__':main()
