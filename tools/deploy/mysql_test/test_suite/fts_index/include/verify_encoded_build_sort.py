#!/usr/bin/env python3
"""Compare complete encoded sort results, including binary NUL and docid keys."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random

import pymysql


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=2881)
    parser.add_argument('--revision', required=True)
    parser.add_argument('--reference', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    report = {'revision': args.revision, 'cases': [], 'passed': False,
              'suite_source_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    # Keep the seed and corpus identical to the preserved pre-change failure.
    rng = random.Random(7239801)
    values = [None, '', ' ', '  ', '\0', '\0\0', 'a', 'A', 'a ', 'a\0', 'a\0 ',
              'a\tb', 'a b', 'a  b', 'a\0b', 'a\x01b', 'a\x1fb', 'a!b',
              'é', 'É', 'e', 'ß', 'ss', 'İ', 'I', 'ı', 'i', 'Σ', 'σ', 'ς',
              '数据库', '数据库 ', '数据', '数\0据', '😀', '😁', 'Ａ', 'a' * 2047]
    alphabet = 'aAzZ09éÉßİıΣσς数据库😀 \t\0\x01\x1f'
    for _ in range(650):
        values.append(''.join(rng.choices(alphabet, k=rng.randrange(1, 50))))
    for i in range(160):
        values.extend(['common-prefix-' + str(i), 'x' * 1020 + str(i), 'A' * 61 + str(i)])
    rng.shuffle(values)
    reference = json.loads(Path(args.reference).read_text())
    report['input_sha256'] = hashlib.sha256(json.dumps(values, ensure_ascii=False).encode()).hexdigest()
    assert report['input_sha256'] == reference['input_sha256']
    assert len(reference['cases']) == 12
    db = pymysql.connect(host='127.0.0.1', port=args.port, user='root',
                         password=os.environ.get('SEEKDB_TEST_PASSWORD', ''),
                         charset='utf8mb4', autocommit=True, read_timeout=180)
    try:
        with db.cursor() as cur:
            cur.execute('DROP DATABASE IF EXISTS encoded_build_sort_regression')
            cur.execute('CREATE DATABASE encoded_build_sort_regression')
            cur.execute('USE encoded_build_sort_regression')
            cur.execute('CREATE TABLE strings(id INT PRIMARY KEY, v VARCHAR(2048)) COLLATE utf8mb4_general_ci')
            for offset in range(0, len(values), 100):
                cur.executemany('INSERT INTO strings VALUES(%s,%s)',
                                list(enumerate(values[offset:offset + 100], offset)))

            def capture(table, projection, order, metadata, expected=None):
                case = dict(metadata)
                for flag in ('false', 'true'):
                    sql = ("SELECT /*+ OPT_PARAM('enable_newsort' '%s') */ %s FROM %s ORDER BY %s"
                           % (flag, projection, table, order))
                    cur.execute('EXPLAIN EXTENDED ' + sql)
                    case['plan_' + flag] = '\n'.join(str(row[0]) for row in cur.fetchall())
                    cur.execute(sql)
                    case['ordinary' if flag == 'false' else 'encoded'] = [list(r) for r in cur.fetchall()]
                case['passed'] = case['ordinary'] == case['encoded']
                if expected is not None:
                    case['reference_equal'] = case['ordinary'] == expected
                    case['passed'] &= case['reference_equal']
                report['cases'].append(case)

            for coll in ('utf8mb4_general_ci', 'utf8mb4_bin', 'binary'):
                expr = 'BINARY v' if coll == 'binary' else 'v COLLATE ' + coll
                for direction in ('ASC', 'DESC'):
                    for suffix in ('', ' LIMIT 25 OFFSET 1000'):
                        base = reference['cases'][len(report['cases'])]
                        assert (base['collation'], base['direction'], base['suffix']) == (coll, direction, suffix)
                        capture('strings', 'id,HEX(v)', '%s %s,id %s%s' % (expr, direction, direction, suffix),
                                dict(collation=coll, direction=direction, suffix=suffix), base['ordinary'])

            # Docids are arbitrary bytes. Test all byte values, zero-filled
            # prefixes and variable lengths in compound ascending/descending keys.
            raw_values = [None, b'', b'\0', b'\0\0', b'\0' * 16, b'\xff' * 16]
            raw_values += [bytes([i]) for i in range(256)]
            raw_values += [bytes(rng.randrange(256) for _ in range(rng.randrange(1, 17))) for _ in range(1400)]
            records = [(i, value) for i, value in enumerate(raw_values)]
            cur.execute('CREATE TABLE docids(id INT PRIMARY KEY, v VARBINARY(16))')
            for offset in range(0, len(records), 100):
                cur.executemany('INSERT INTO docids VALUES(%s,%s)', records[offset:offset + 100])
            for direction in ('ASC', 'DESC'):
                ordered = sorted(records, key=lambda r: (r[1] is not None, r[1] or b'', r[0]),
                                 reverse=direction == 'DESC')
                expected = [[i, None if value is None else value.hex().upper()] for i, value in ordered]
                for suffix in ('', ' LIMIT 37 OFFSET 1000'):
                    capture('docids', 'id,HEX(v)', 'v %s,id %s%s' % (direction, direction, suffix),
                            dict(collation='varbinary', direction=direction, suffix=suffix),
                            expected[1000:1037] if suffix else expected)
            report['passed'] = all(c['passed'] for c in report['cases'])
    finally:
        db.close()
        Path(args.output).write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'passed': report['passed'], 'cases': len(report['cases']),
                      'mismatches': [{k: c[k] for k in ('collation', 'direction', 'suffix')}
                                     for c in report['cases'] if not c['passed']]}))
    assert report['passed']


if __name__ == '__main__':
    main()
