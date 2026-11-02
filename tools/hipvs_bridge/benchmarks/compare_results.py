"""Compare paired development records; never claims official Hidden acceptance."""
import argparse
import json
import math
from pathlib import Path
from statistics import median


def compare(root, baseline, candidate, require_sql=True, additional_sql_pairs=()):
    rows=[]
    for kind in ('sift10k','sift250k','gaussian256','clustered256'):
        a=json.loads((root/f'{kind}-{baseline}.json').read_text())
        b=json.loads((root/f'{kind}-{candidate}.json').read_text())
        assert a['completed'] and b['completed'], kind
        assert a['dataset']==b['dataset'], 'Paired dataset mismatch: '+kind
        record={'dataset':kind,'recall':{},'qps':{}}
        quality=True;ratios=[]
        for k in ('10','100'):
            floor=.75 if kind=='sift10k' else (.95 if kind=='sift250k' else (.90 if k=='10' else .85))
            base_r=max(r['topk'][k]['recall'] for r in a['rounds'])
            selected_r=min(r['topk'][k]['recall'] for r in b['rounds'])
            threshold=max(floor,base_r-.005)
            passed=selected_r>=threshold
            quality=quality and passed
            record['recall'][k]={'baseline_best':base_r,'candidate_worst':selected_r,'threshold':threshold,'passed':passed}
            aq=median(r['topk'][k]['median_qps'] for r in a['rounds'])
            bq=median(r['topk'][k]['median_qps'] for r in b['rounds'])
            assert math.isfinite(aq) and aq>0 and math.isfinite(bq) and bq>0
            record['qps'][k]={'baseline':aq,'candidate':bq,'ratio':bq/aq}
            ratios.append(bq/aq)
        record['query_geometric_ratio']=math.sqrt(math.prod(ratios))
        record['query_gate_passed']=record['query_geometric_ratio']>=.90
        a_build=median(r['build_plus_first_query_seconds'] for r in a['rounds'])
        b_build=median(r['build_plus_first_query_seconds'] for r in b['rounds'])
        assert math.isfinite(a_build) and a_build>0 and math.isfinite(b_build) and b_build>0
        record['bridge_build_plus_first_query']={'baseline_seconds':a_build,'candidate_seconds':b_build,'speedup':a_build/b_build}
        record['vram_ratio']=b['peak_vram_device_bytes']/a['peak_vram_device_bytes']
        record['candidate_peak_vram_bytes']=b['peak_vram_device_bytes']
        record['candidate_peak_rss_bytes']=b['peak_rss_bytes']
        record['resource_gate_passed']=record['vram_ratio']<=1.25 and b['peak_vram_device_bytes']<=45_000_000_000 and b['peak_rss_bytes']<=96_000_000_000
        record['quality_gate_passed']=quality
        record['passed']=quality and record['query_gate_passed'] and record['resource_gate_passed']
        rows.append(record)
    summary={'scope':'paired public/generated development checks; not official Hidden results or score',
             'baseline_label':baseline,'candidate_label':candidate,'datasets':rows,'passed':all(r['passed'] for r in rows)}
    if not require_sql:return summary
    sql_runs={}
    pairs=[(baseline,candidate),*additional_sql_pairs]
    for pair in pairs:
        for label in pair:
            sql_path=root/f'sql-{label}.json'
            if not sql_path.exists():raise FileNotFoundError('SQL pair is incomplete: '+str(sql_path))
            data=json.loads(sql_path.read_text())
            summary['sql_'+label]=data
            sql_runs[label]={row['name']:row for row in data['datasets']}
            summary['passed']=summary['passed'] and data['completed'] and data['loaded_bridge_verified'] and data['cpu_vsag_passed'] and data['reload_validated']
    summary['sql_resources']=[]
    for a,b in pairs:
        base=summary['sql_'+a];selected=summary['sql_'+b]
        ratio=selected['peak_vram_device_bytes']/base['peak_vram_device_bytes']
        passed=ratio<=1.25 and selected['peak_vram_device_bytes']<=45_000_000_000 and selected['peak_seekdb_rss_bytes']<=96_000_000_000
        summary['sql_resources'].append({'baseline':a,'candidate':b,'vram_ratio':ratio,'candidate_peak_vram_bytes':selected['peak_vram_device_bytes'],'candidate_peak_rss_bytes':selected['peak_seekdb_rss_bytes'],'passed':passed})
        summary['passed']=summary['passed'] and passed
    summary['sql_datasets']=[]
    kinds=set(sql_runs[baseline])
    assert all(set(run)==kinds for run in sql_runs.values()), 'SQL pair has different datasets'
    for kind in sorted(kinds):
        ar=[sql_runs[a][kind] for a,b in pairs];br=[sql_runs[b][kind] for a,b in pairs]
        assert all((r['rows'],r['dim'],r['nq'])==(ar[0]['rows'],ar[0]['dim'],ar[0]['nq']) for r in ar+br)
        at=[r['ddl_plus_first_query_seconds'] for r in ar];bt=[r['ddl_plus_first_query_seconds'] for r in br]
        record={'dataset':kind,'baseline_cold_seconds':at,'candidate_cold_seconds':bt,
                'baseline_median_seconds':median(at),'candidate_median_seconds':median(bt),'speedup':median(at)/median(bt),'recall':{}}
        for k in ('10','100'):
            floor=.95 if kind=='sift250k' else (.90 if k=='10' else .85)
            paired=[]
            for a,b in zip(ar,br):
                threshold=max(floor,a['topk'][k]['recall']-.005)
                value=b['topk'][k]['recall']
                paired.append({'baseline':a['topk'][k]['recall'],'candidate':value,'threshold':threshold,'passed':value>=threshold})
            record['recall'][k]={'candidate_worst':min(r['topk'][k]['recall'] for r in br),'pairs':paired,'passed':all(r['passed'] for r in paired),'scope':'256 sequential single-query diagnostics, not formal nq=4096 batch quality'}
            summary['passed']=summary['passed'] and record['recall'][k]['passed']
        summary['sql_datasets'].append(record)
    return summary


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--evidence',type=Path,required=True);p.add_argument('--baseline',default='final-baseline');p.add_argument('--candidate',default='final-selected');p.add_argument('--output',type=Path,required=True);p.add_argument('--bridge-only',action='store_true');p.add_argument('--sql-pair',nargs=2,action='append',default=[])
    args=p.parse_args();result=compare(args.evidence,args.baseline,args.candidate,not args.bridge_only,args.sql_pair)
    args.output.write_text(json.dumps(result,indent=2))
    print(json.dumps(result,indent=2))
    if not result['passed']:raise SystemExit(1)
