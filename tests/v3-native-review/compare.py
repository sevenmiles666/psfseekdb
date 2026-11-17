"""Matched local comparison. Local dccb61e reference is NOT the platform baseline."""
import argparse
from decimal import Decimal, ROUND_HALF_UP
import json
from pathlib import Path
from statistics import median
import sys

WORK = Path(__file__).resolve().parent
ROOT = WORK.parents[1]
sys.path.insert(0, str(ROOT / 'tests/comprehensive_v3'))
import benchmark_local as b
from workload_matrix import dataset, fingerprint

parser = argparse.ArgumentParser()
parser.add_argument('--cases', nargs='+', default=['grid', 'fp32-long', 'decimal-short'])
parser.add_argument('--seed', type=int, default=20261118)
parser.add_argument('--candidate', default='colleague-v3')
parser.add_argument('--include-current', action='store_true')
args = parser.parse_args()
images = {'reference': 'minivectordb-round2:release-dccb61e', 'candidate': 'minivectordb-round2:' + args.candidate}
if args.include_current:
    images['current_b97'] = 'minivectordb-round2:dccb-refined'
probe = ROOT / 'build/benchmark_support'
for case in args.cases:
    buckets, vectors, queries, inserts, sql = dataset(args.seed, case)
    truth, digest = b.exact_truth(images['candidate'], probe, buckets, vectors, queries)
    report = {'status': 'running', 'reference_commit': 'dccb61eb98b9f6a98cee30d59aca5684d9fedc15',
        'candidate_tag': args.candidate, 'official_baseline_available': False,
        'environment': 'WSL2 / Docker Desktop x86_64, pinned GCC13.4, cpuset4,6, client8, 2 CPU, 64MiB no swap. Fresh volume each phase; shared image/data block device, final resource probe affects peak. No global cache reset. Local E is not a platform score.',
        'distribution': case, 'seed': args.seed, 'dataset_sha256': digest,
        'sql_text_sha256': fingerprint(inserts, sql), 'images': {k:b.docker('image','inspect',v,'--format','{{.Id}}').decode().strip() for k,v in images.items()}, 'rounds': []}
    path = WORK / (args.candidate + '-' + case + '-' + str(args.seed) + '.json')
    assert not path.exists(), path
    for turn in range(3):
        result = {'number': turn + 1, 'write': {}, 'query': {}}
        keys = list(images)
        order = keys[turn % len(keys):] + keys[:turn % len(keys)]
        for label in order:
            result['write'][label] = b.run_write(images[label], probe, inserts, buckets, vectors)
            print(case, turn+1, label, 'write', result['write'][label]['seconds'], flush=True)
        for label in reversed(order):
            result['query'][label] = b.run_query(images[label], probe, 'exact', inserts, sql, truth, buckets, vectors, queries, True)
            assert all(x['mean_recall_at_10'] == 1.0 for x in result['query'][label]['recall'].values())
            print(case, turn+1, label, 'query', result['query'][label]['seconds'], flush=True)
        report['rounds'].append(result)
        path.write_text(json.dumps(report, indent=2)+'\n')
    metrics = ('seconds', 'io_bytes', 'memory_peak_bytes')
    base = {p:{k:median(r[p]['reference'][k] for r in report['rounds']) for k in metrics} for p in ('write','query')}
    report['fixed_local_baseline'] = base
    report['summary'] = {}
    for label in images:
        values = [(b.efficiency(base['write'],r['write'][label])+2*b.efficiency(base['query'],r['query'][label]))/3 for r in report['rounds']]
        report['summary'][label] = {p:{k:median(r[p][label][k] for r in report['rounds']) for k in metrics} for p in ('write','query')}
        report['summary'][label].update(E_rounds=values, E=float(Decimal(str(median(values))).quantize(Decimal('.000001'), rounding=ROUND_HALF_UP)))
    report['status'] = 'PASS'
    path.write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report['summary']), flush=True)
