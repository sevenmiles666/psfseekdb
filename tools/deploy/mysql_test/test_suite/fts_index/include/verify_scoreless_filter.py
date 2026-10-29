#!/usr/bin/env python3
"""Compare scoreless MATCH results and verify that score consumers keep BM25."""
import argparse
from decimal import Decimal
import hashlib
import json
from pathlib import Path

import pymysql


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--port', type=int, default=2881)
parser.add_argument('--revision', required=True)
parser.add_argument('--output', required=True)
parser.add_argument('--reference')
parser.add_argument('--expect-scoreless', action='store_true')
args = parser.parse_args()
report = {'revision': args.revision, 'cases': [], 'plans': [], 'passed': False,
          'suite_source_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
db = pymysql.connect(host='127.0.0.1', port=args.port, user='root',
                     charset='utf8mb4', autocommit=True, read_timeout=180)


def canonical(value):
    if isinstance(value, float):
        return value.hex()
    if isinstance(value, Decimal):
        return str(value)
    if isinstance(value, bytes):
        return value.hex()
    return value


def capture(cursor, name, sql, params=()):
    cursor.execute(sql, params)
    report['cases'].append({'name': name,
                            'rows': [[canonical(v) for v in row] for row in cursor.fetchall()]})


def correct_ranked_cases(cases, verify):
    """Use every full-scan score to check boundary ties, independently of top-k."""
    by_name = {case['name']: case for case in cases}
    corrections = []
    for tokenizer in ('beng', 'ik'):
        scores = by_name[tokenizer + '/score']['rows']
        expected = [[row[0]] for row in sorted(scores, key=lambda row: (-float.fromhex(row[1]), row[0]))[:65]]
        case = by_name[tokenizer + '/score-order']
        if verify:
            assert case['rows'] == expected, ('Boundary ties lost before secondary-key sorting', tokenizer)
        elif case['rows'] != expected:
            corrections.append(dict(name=case['name'], original_rows=case['rows'], oracle_rows=expected))
            case['rows'] = expected
    return corrections


def plan(cursor, name, sql, scoreless):
    cursor.execute('EXPLAIN EXTENDED ' + sql)
    text = '\n'.join(str(cell) for row in cursor.fetchall() for cell in row)
    report['plans'].append({'name': name, 'text': text, 'scoreless_expected': scoreless})
    if args.expect_scoreless:
        marker = 'calc_relevance=false' if scoreless else 'calc_relevance=true'
        assert marker in text, (name, marker, text)


try:
    with db.cursor() as c:
        c.execute('SET ob_query_timeout=180000000')
        c.execute('DROP DATABASE IF EXISTS fts_scoreless_regression')
        c.execute('CREATE DATABASE fts_scoreless_regression')
        c.execute('USE fts_scoreless_regression')
        tokens = ['tok' + chr(97 + i // 26) + chr(97 + i % 26) for i in range(300)]
        documents = [(i, None if i % 89 == 0 else '' if i % 43 == 0 else
                      'database ' * (1 + i % 13) + ('audit ' if i % 9 else '') +
                      '全文索引 数据库 ' + tokens[i % 300]) for i in range(1, 2058)]
        for tokenizer in ('beng', 'ik'):
            table = 'docs_' + tokenizer
            c.execute(f'CREATE TABLE {table}(id INT PRIMARY KEY, body TEXT)')
            for offset in range(0, len(documents), 200):
                c.executemany(f'INSERT INTO {table} VALUES(%s,%s)', documents[offset:offset + 200])
            c.execute(f'ALTER TABLE {table} ADD FULLTEXT INDEX ft(body) WITH PARSER {tokenizer}')
            for number, term in enumerate(('database', 'database audit', '全文索引 数据库', '',
                                           'the and of', 'missinguniqueterm', 'database database',
                                           ' '.join(tokens), ' '.join(tokens) + ' database audit')):
                match = 'MATCH(body) AGAINST(%s)'
                prefix = f'{tokenizer}/{number}'
                capture(c, prefix + '/all', f'SELECT id FROM {table} WHERE {match} ORDER BY id', (term,))
                capture(c, prefix + '/count', f'SELECT COUNT(*),SUM(id) FROM {table} WHERE {match}', (term,))
                for limit, offset in ((0, 0), (1, 0), (64, 63), (65, 512), (2057, 0)):
                    capture(c, prefix + f'/page/{limit}/{offset}',
                            f'SELECT id FROM {table} WHERE {match} ORDER BY id LIMIT {limit} OFFSET {offset}', (term,))
            match = "MATCH(body) AGAINST('database audit')"
            for name, sql, scoreless in (
                ('filter', f'SELECT id FROM {table} WHERE {match} ORDER BY id', True),
                ('aggregate', f'SELECT COUNT(*) FROM {table} WHERE {match}', True),
                ('score', f'SELECT id,{match} FROM {table} WHERE {match} ORDER BY id', False),
                ('score-order', f'SELECT id FROM {table} WHERE {match} ORDER BY {match} DESC,id LIMIT 65', False),
                ('case', f'SELECT id,CASE WHEN {match} THEN 1 ELSE 0 END FROM {table} WHERE {match} ORDER BY id', False),
                ('shared-bool', f'SELECT id,({match} IS TRUE) FROM {table} WHERE {match} ORDER BY id', False),
                ('boolean', f"SELECT id FROM {table} WHERE MATCH(body) AGAINST('+database -audit' IN BOOLEAN MODE) ORDER BY id", False),
            ):
                capture(c, f'{tokenizer}/{name}', sql)
                plan(c, f'{tokenizer}/{name}', sql, scoreless)
            for name, sql in (
                ('threshold', f'SELECT id,{match} FROM {table} WHERE {match}>0.25 ORDER BY id'),
                ('negative-threshold', f'SELECT id FROM {table} WHERE {match}>-1 ORDER BY id'),
                ('group', f'SELECT ({match} IS TRUE),COUNT(*) FROM {table} WHERE {match} GROUP BY ({match} IS TRUE) ORDER BY 1'),
                ('having', f'SELECT id,{match} AS score FROM {table} WHERE {match} HAVING score>0.25 ORDER BY id'),
                ('null-query', f'SELECT id FROM {table} WHERE MATCH(body) AGAINST(NULL) ORDER BY id'),
                ('or', f'SELECT id FROM {table} WHERE {match} OR id=89 ORDER BY id'),
                ('and', f'SELECT id FROM {table} WHERE {match} AND id BETWEEN 60 AND 900 ORDER BY id'),
            ):
                capture(c, f'{tokenizer}/{name}', sql)
            prepared = f'SELECT id FROM {table} WHERE MATCH(body) AGAINST(?) ORDER BY id LIMIT 129 OFFSET 63'
            c.execute('PREPARE no_score_query FROM %s', (prepared,))
            for i, term in enumerate(('database', '', 'audit', 'missinguniqueterm', 'database', ' '.join(tokens))):
                c.execute('SET @no_score_term=%s', (term,))
                capture(c, f'{tokenizer}/prepared/{i}', 'EXECUTE no_score_query USING @no_score_term')
            c.execute('DEALLOCATE PREPARE no_score_query')
            c.execute('START TRANSACTION')
            c.execute(f"UPDATE {table} SET body='fresh database' WHERE id BETWEEN 60 AND 130")
            c.execute(f'DELETE FROM {table} WHERE id BETWEEN 510 AND 520')
            capture(c, f'{tokenizer}/transaction', f"SELECT id FROM {table} WHERE MATCH(body) AGAINST('fresh database') ORDER BY id")
            c.execute('ROLLBACK')
            capture(c, f'{tokenizer}/rollback', f"SELECT id FROM {table} WHERE MATCH(body) AGAINST('fresh database') ORDER BY id")
            c.execute(f"UPDATE {table} SET body='fresh database' WHERE id BETWEEN 60 AND 130")
            c.execute(f'DELETE FROM {table} WHERE id BETWEEN 510 AND 520')
            capture(c, f'{tokenizer}/after-dml', f"SELECT id FROM {table} WHERE MATCH(body) AGAINST('fresh database') ORDER BY id")
        c.execute('DROP TABLE IF EXISTS composite_docs')
        c.execute('CREATE TABLE composite_docs(tenant INT,id INT,body TEXT,PRIMARY KEY(tenant,id),FULLTEXT INDEX ft(body) WITH PARSER beng)')
        c.executemany('INSERT INTO composite_docs VALUES(%s,%s,%s)', [(i % 5, i, 'database audit') for i in range(160)])
        capture(c, 'composite-rowkey', "SELECT tenant,id FROM composite_docs WHERE MATCH(body) AGAINST('database audit') ORDER BY tenant,id")
    correct_ranked_cases(report['cases'], verify=True)
    if args.reference:
        expected = json.loads(Path(args.reference).read_text())['cases']
        report['baseline_rank_corrections'] = correct_ranked_cases(expected, verify=False)
        assert report['cases'] == expected, 'Complete rows, exact scores or aggregates differ'
    report['result_sha256'] = hashlib.sha256(json.dumps(report['cases'], sort_keys=True).encode()).hexdigest()
    report['passed'] = True
finally:
    db.close()
    Path(args.output).write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n')
print(json.dumps({k: v for k, v in report.items() if k not in ('cases', 'plans')}))
