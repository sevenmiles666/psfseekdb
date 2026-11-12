"""Validate learning-input integrity, evidence references and the selected code base.

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
        value = value[int(key)] if isinstance(value, list) else value[key]
    return value


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
        value = resolve_pointer(material, ref['pointer'])
        quote = ref['quote']
        if not isinstance(value, str) or not isinstance(quote, str) or not 0 < len(quote) <= 240 or quote not in value:
            raise ValueError(f'evidence quote does not match {ref["pointer"]}')
    history = {key: material[key] for key in manifest['historical_top_level_keys']}
    canonical = json.dumps(history, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode()
    if hashlib.sha256(canonical).hexdigest() != manifest['historical_payload_sha256']:
        raise ValueError('imported historical fields were changed')
    for entry in manifest['unchanged_runtime_files']:
        body = (repo / entry['path']).read_bytes()
        git_sha = hashlib.sha1(b'blob ' + str(len(body)).encode() + b'\0' + body).hexdigest()
        if git_sha != entry['git_blob_sha1'] or hashlib.sha256(body).hexdigest() != entry['sha256']:
            raise ValueError(f'code differs from selected old base: {entry["path"]}')
    if re.search(rb'(?:github_pat_|gh[pousr]_)[A-Za-z0-9_]+|https://[^\s"<>]+@github\.com', data):
        raise ValueError('credential-like text in learning material')
    if any(key in material for key in ['agent_prompt_score', 'review_status', 'round_scores']):
        raise ValueError('input must not present a self-assigned platform result')
    return {
        'status': 'PASS', 'bytes': len(data), 'sha256': digest,
        'both_material_paths_identical': True,
        'references_checked': len(material['evidence_navigation']),
        'historical_fields_preserved': True,
        'runtime_files_identical_to_c16935a1': len(manifest['unchanged_runtime_files']),
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
