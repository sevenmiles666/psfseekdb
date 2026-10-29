#!/usr/bin/env python3
"""Compare full COUNT/LIMIT results and preserve scoring for value consumers."""
import argparse
from decimal import Decimal
import hashlib
import json
from pathlib import Path

import pymysql

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--revision', required=True)
parser.add_argument('--output', required=True)
parser.add_argument('--reference')
parser.add_argument('--expect-count-only', action='store_true')
args = parser.parse_args()
report = dict(revision=args.revision, cases=[], plans=[], passed=False,
              suite_source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
db = pymysql.connect(host='127.0.0.1', port=2881, user='root', charset='utf8mb4',
                     autocommit=True, read_timeout=180)


def canonical(v):
    return v.hex() if isinstance(v, (float, bytes)) else str(v) if isinstance(v, Decimal) else v


def capture(c, name, sql, params=()):
    c.execute(sql, params if params else None)
    report['cases'].append(dict(name=name, rows=[[canonical(v) for v in r] for r in c.fetchall()]))


try:
    with db.cursor() as c:
        c.execute('SET ob_query_timeout=180000000')
        c.execute('DROP DATABASE IF EXISTS fts_count_limit_regression')
        c.execute('CREATE DATABASE fts_count_limit_regression')
        c.execute('USE fts_count_limit_regression')
        terms = ['tok' + chr(97 + i // 26) + chr(97 + i % 26) for i in range(300)]
        docs = [(i, None if i % 7 == 0 else i,
                 None if i % 89 == 0 else '' if i % 43 == 0 else
                 'database ' * (1 + i % 13) + ('audit ' if i % 9 else '') +
                 '全文索引 数据库 ' + terms[i % 300]) for i in range(1, 2058)]
        for tokenizer in ('beng', 'ik'):
            table = 'docs_' + tokenizer
            c.execute(f'CREATE TABLE {table}(id INT PRIMARY KEY,n INT,body TEXT)')
            for offset in range(0, len(docs), 200):
                c.executemany(f'INSERT INTO {table} VALUES(%s,%s,%s)', docs[offset:offset + 200])
            c.execute(f'ALTER TABLE {table} ADD FULLTEXT INDEX ft(body) WITH PARSER {tokenizer}')
            for i, term in enumerate(('database audit', '全文索引 数据库', '', None,
                                      'the and of', 'absentuniqueterm', ' '.join(terms))):
                for limit, offset in ((0, 0), (1, 0), (20, 0), (65, 63),
                                      (512, 1024), (3000, 0), (20, 3000)):
                    capture(c, f'{tokenizer}/{i}/{limit}/{offset}',
                            f'SELECT COUNT(*) FROM (SELECT id FROM {table} '
                            f'WHERE MATCH(body) AGAINST(%s) LIMIT {limit} OFFSET {offset}) v', (term,))
            match = "MATCH(body) AGAINST('database audit')"
            for name, sql, scoreless in (
                ('count', f'SELECT COUNT(*) FROM (SELECT id FROM {table} WHERE {match} LIMIT 20) v', True),
                ('residual', f'SELECT COUNT(*) FROM (SELECT id FROM {table} WHERE {match} AND n>100 LIMIT 65 OFFSET 63) v', True),
                ('columns', f'SELECT COUNT(*) FROM (SELECT id,n FROM {table} WHERE {match} LIMIT 20) v', True),
                ('explicit-score-order', f'SELECT COUNT(*) FROM (SELECT id FROM {table} WHERE {match} ORDER BY {match} DESC,id LIMIT 20) v', False),
                ('count-nullable', f'SELECT COUNT(n) FROM (SELECT id,n FROM {table} WHERE {match} LIMIT 65) v', False),
                ('distinct', f'SELECT COUNT(DISTINCT n) FROM (SELECT id,n FROM {table} WHERE {match} LIMIT 65) v', False),
                ('sum', f'SELECT SUM(id) FROM (SELECT id FROM {table} WHERE {match} LIMIT 65) v', False),
                ('multi-aggregate', f'SELECT COUNT(*),SUM(id) FROM (SELECT id FROM {table} WHERE {match} LIMIT 65) v', False),
                ('parent-filter', f'SELECT COUNT(*) FROM (SELECT id FROM {table} WHERE {match} LIMIT 65) v WHERE id%7=0', False),
                ('join', f'SELECT COUNT(*) FROM (SELECT id FROM {table} WHERE {match} LIMIT 65) v JOIN {table} d ON d.id=v.id WHERE d.n IS NULL', False),
                ('projection-score', f'SELECT COUNT(*) FROM (SELECT id,{match} AS score FROM {table} WHERE {match} LIMIT 65) v', False),
                ('values', f'SELECT id FROM (SELECT id FROM {table} WHERE {match} LIMIT 65 OFFSET 63) v ORDER BY id', False),
            ):
                capture(c, tokenizer + '/' + name, sql)
                c.execute('EXPLAIN EXTENDED ' + sql)
                text = '\n'.join(str(v) for row in c.fetchall() for v in row)
                report['plans'].append(dict(name=tokenizer + '/' + name, text=text, scoreless_expected=scoreless))
                if args.expect_count_only:
                    marker = 'calc_relevance=false' if scoreless else 'calc_relevance=true'
                    assert marker in text, (name, marker, text)
            prepared = (f'SELECT COUNT(*) FROM (SELECT id FROM {table} '
                        'WHERE MATCH(body) AGAINST(?) LIMIT ? OFFSET ?) v')
            c.execute('PREPARE count_limit_query FROM %s', (prepared,))
            for i, (term, limit, offset) in enumerate((('database', 65, 63), ('', 20, 0),
                                                       ('audit', 1, 2000), ('database', 0, 0))):
                c.execute('SET @cl_term=%s,@cl_limit=%s,@cl_offset=%s', (term, limit, offset))
                capture(c, f'{tokenizer}/prepared/{i}', 'EXECUTE count_limit_query USING @cl_term,@cl_limit,@cl_offset')
            c.execute('DEALLOCATE PREPARE count_limit_query')
            c.execute('START TRANSACTION')
            c.execute(f'DELETE FROM {table} WHERE id<1000')
            capture(c, tokenizer + '/transaction', f'SELECT COUNT(*) FROM (SELECT id FROM {table} WHERE {match} LIMIT 3000) v')
            c.execute('ROLLBACK')
            capture(c, tokenizer + '/rollback', f'SELECT COUNT(*) FROM (SELECT id FROM {table} WHERE {match} LIMIT 3000) v')
    if args.reference:
        assert report['cases'] == json.loads(Path(args.reference).read_text())['cases'], 'Complete results differ'
    report['result_sha256'] = hashlib.sha256(json.dumps(report['cases'], sort_keys=True).encode()).hexdigest()
    report['passed'] = True
finally:
    db.close()
    Path(args.output).write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
print(json.dumps({k: v for k, v in report.items() if k not in ('cases', 'plans')}))
