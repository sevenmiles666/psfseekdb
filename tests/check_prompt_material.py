"""Validate learning-input integrity, evidence references and runtime files.

This checks submission inputs; it does not simulate the platform's Prompt grader.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f'duplicate JSON key: {key}')
        result[key] = value
    return result


def reject_constant(value):
    raise ValueError(f'nonstandard JSON number: {value}')


def load_json(data):
    result = json.loads(data.decode('utf-8'), object_pairs_hook=unique_object,
                        parse_constant=reject_constant)

    def finite(value):
        if isinstance(value, float) and not math.isfinite(value):
            raise ValueError('non-finite numeric value')
        if isinstance(value, dict):
            for child in value.values():
                finite(child)
        if isinstance(value, list):
            for child in value:
                finite(child)

    finite(result)
    return result


def resolve_pointer(document, pointer):
    if not pointer.startswith('/'):
        raise ValueError('reference must be an RFC6901 JSON pointer')
    value = document
    for part in pointer[1:].split('/'):
        if re.search(r'~(?![01])', part):
            raise ValueError('invalid JSON pointer escape')
        key = part.replace('~1', '/').replace('~0', '~')
        if isinstance(value, list):
            if not re.fullmatch(r'0|[1-9][0-9]*', key):
                raise ValueError('invalid JSON pointer array index')
            value = value[int(key)]
        else:
            value = value[key]
    return value


def check_reference(material, ref, expected_role=None):
    value = resolve_pointer(material, ref['pointer'])
    quote = ref['quote']
    if not isinstance(value, str) or not isinstance(quote, str) or not 0 < len(quote) <= 240 or quote not in value:
        raise ValueError(f'evidence quote does not match {ref["pointer"]}')
    if expected_role is not None:
        parent = ref['pointer'].rsplit('/', 1)[0]
        role = None
        while parent:
            record = resolve_pointer(material, parent)
            if isinstance(record, dict) and 'role' in record:
                role = record['role']
                break
            parent = parent.rsplit('/', 1)[0]
        if role != expected_role:
            raise ValueError(f'evidence role must be {expected_role}: {ref["pointer"]}')


def check_learning_episodes(material):
    ids = set()
    used_student = set()
    used_result = set()
    references = 0
    for episode in material.get('learning_episodes', []):
        identity = episode['id']
        if not isinstance(identity, str) or not identity or identity in ids:
            raise ValueError('duplicate or empty learning episode id')
        ids.add(identity)
        if not episode.get('student_records') or not episode.get('result_records'):
            raise ValueError('learning episode needs actual student and result records')
        for field, role, used in [
            ('student_records', 'user', used_student),
            ('result_records', 'tool', used_result),
            ('assistant_records', 'assistant', None),
        ]:
            for ref in episode.get(field, []):
                check_reference(material, ref, role)
                if used is not None:
                    if ref['pointer'] in used:
                        raise ValueError(f'duplicate {field} source across learning episodes')
                    used.add(ref['pointer'])
                references += 1
    return len(ids), references


def check_repository_evidence(repo, material):
    evidence = material.get('repository_evidence')
    if evidence is None:
        return 0
    if evidence['role'] != 'tool' or evidence['snapshot_commit'] != material['submission_context']['repository_snapshot_commit']:
        raise ValueError('repository evidence attribution or snapshot differs')
    for entry in evidence['source_files']:
        body = (repo / entry['path']).read_bytes()
        git_sha = hashlib.sha1(b'blob ' + str(len(body)).encode() + b'\0' + body).hexdigest()
        if git_sha != entry['git_blob_sha1'] or hashlib.sha256(body).hexdigest() != entry['sha256']:
            raise ValueError(f'repository evidence source differs: {entry["path"]}')
    for excerpt in evidence['code_and_test_corroboration']:
        lines = (repo / excerpt['path']).read_text().splitlines()
        start = excerpt['start_line'] - 1
        expected = excerpt['excerpt'].splitlines()
        if start < 0 or lines[start:start + len(expected)] != expected:
            raise ValueError('repository code excerpt differs')
    for report in evidence['benchmark_audit']['reports']:
        raw = load_json((repo / report['path']).read_bytes())
        for phase, variants in report['medians_recomputed'].items():
            for variant, metrics in variants.items():
                summary_variant = 'exact' if variant == 'candidate' else variant
                if raw['summary'][summary_variant][phase + '_median'] != metrics:
                    raise ValueError('repository benchmark summary differs')
    return len(evidence['source_files'])


def check_development_traces(material):
    index = material.get('development_trace_index')
    if index is None:
        return 0, 0
    if index['role'] != 'assistant':
        raise ValueError('development index must retain assistant attribution')
    ids = set()
    references = 0
    for trace in index['traces']:
        if trace['id'] in ids:
            raise ValueError('duplicate development trace id')
        ids.add(trace['id'])
        if not trace['result_records']:
            raise ValueError('development trace needs actual results')
        for field, role in [('assistant_records', 'assistant'), ('result_records', 'tool')]:
            for ref in trace[field]:
                check_reference(material, ref, role)
                references += 1
        for ref in trace.get('retrospective_records', []):
            if not ref['pointer'].endswith('/assistant_retrospective'):
                raise ValueError('retrospective must reference an assistant retrospective field')
            check_reference(material, ref)
            references += 1
    return len(ids), references


def check(repo):
    manifest = load_json((repo / 'agent/material-manifest.json').read_bytes())
    data = (repo / manifest['canonical_path']).read_bytes()
    if len(data) > 128 * 1024:
        raise ValueError('material exceeds 128 KiB')
    material = load_json(data)
    if not isinstance(material, dict):
        raise ValueError('material top level must be an object')
    if (repo / manifest['compatibility_path']).read_bytes() != data:
        raise ValueError('material paths differ')
    digest = hashlib.sha256(data).hexdigest()
    if digest != manifest['material_sha256'] or len(data) != manifest['material_bytes']:
        raise ValueError('material bytes or digest differ from the manifest')
    summary = resolve_pointer(material, manifest['summary_pointer'])
    if not isinstance(summary, str) or not summary.strip():
        raise ValueError('material summary must be a nonempty string')
    for ref in material['evidence_navigation']:
        check_reference(material, ref)
    episodes, episode_references = check_learning_episodes(material)
    if manifest.get('format') == 'minivectordb-material-manifest-v2' and not episodes:
        raise ValueError('v2 material needs at least one navigable learning episode')
    history = {key: material[key] for key in manifest['historical_top_level_keys']}
    canonical = json.dumps(history, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode()
    if hashlib.sha256(canonical).hexdigest() != manifest['historical_payload_sha256']:
        raise ValueError('imported historical fields were changed')
    if 'imported_source' in manifest:
        source = manifest['imported_source']
        restored = {entry['key']: resolve_pointer(material, entry['preserved_at']) for entry in source['fields']}
        payload = json.dumps(restored, ensure_ascii=False, sort_keys=True, separators=(',', ':'), allow_nan=False).encode('utf-8')
        if hashlib.sha256(payload).hexdigest() != source['canonical_payload_sha256']:
            raise ValueError('source material was modified or incompletely preserved')
        if material['submission_context']['repository_snapshot_commit'] != source['commit']:
            raise ValueError('source commit differs from the manifest')
        for key in ['current_grade_source', 'participation_record']:
            check_reference(material, material['submission_context'][key], 'user')
    if 'recovered_session_payload_sha256' in manifest:
        payload = json.dumps(material['recovered_sessions'], ensure_ascii=False, sort_keys=True,
                             separators=(',', ':'), allow_nan=False).encode('utf-8')
        if hashlib.sha256(payload).hexdigest() != manifest['recovered_session_payload_sha256']:
            raise ValueError('recovered historical session was changed')
    for entry in manifest['runtime_files']:
        body = (repo / entry['path']).read_bytes()
        git_sha = hashlib.sha1(b'blob ' + str(len(body)).encode() + b'\0' + body).hexdigest()
        if git_sha != entry['git_blob_sha1'] or hashlib.sha256(body).hexdigest() != entry['sha256']:
            raise ValueError(f'runtime file differs from the manifest: {entry["path"]}')
    repository_sources = check_repository_evidence(repo, material)
    development_traces, development_references = check_development_traces(material)
    if re.search(rb'(?:github_pat_|gh[pousr]_)[A-Za-z0-9_]+|https://[^\s"<>]+@github\.com', data):
        raise ValueError('credential-like text in learning material')
    if any(key in material for key in ['agent_prompt_score', 'review_status', 'round_scores']):
        raise ValueError('input must not present a self-assigned platform result')
    return {
        'status': 'PASS', 'bytes': len(data), 'sha256': digest,
        'both_material_paths_identical': True,
        'references_checked': len(material['evidence_navigation']),
        'learning_episodes_checked': episodes,
        'episode_references_checked': episode_references,
        'historical_fields_preserved': True,
        'runtime_files_checked': len(manifest['runtime_files']),
        'repository_evidence_sources_checked': repository_sources,
        'development_traces_checked': development_traces,
        'development_references_checked': development_references,
        'platform_prompt_score_verified': False,
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    result = check(args.repo)
    text = json.dumps(result, ensure_ascii=False, indent=2) + '\n'
    if args.output:
        args.output.write_text(text)
    print(text, end='')
