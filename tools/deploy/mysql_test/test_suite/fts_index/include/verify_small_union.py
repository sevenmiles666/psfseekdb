#!/usr/bin/env python3
"""Compare small postings unions with the full scoring path across reuse and DML."""
import argparse
import json
from pathlib import Path

import pymysql

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--revision', required=True)
p.add_argument('--output', required=True)
a = p.parse_args()
report = dict(revision=a.revision, passed=False, checks=0)
db = pymysql.connect(host='127.0.0.1', port=2881, user='root', charset='utf8mb4',
                     autocommit=True, read_timeout=180)
try:
    with db.cursor() as c:
        c.execute('DROP DATABASE IF EXISTS fts_small_union_regression')
        c.execute('CREATE DATABASE fts_small_union_regression')
        c.execute('USE fts_small_union_regression')
        terms = ['alpha', 'beta', 'gamma', 'delta', 'epsilon', 'zeta']
        queries = [' '.join(terms[:n]) for n in range(1, 7)] + [None, '', 'unmatchedword']
        for parser in ('beng', 'ik'):
            for explicit_pk in (False, True):
                table = 'docs_' + parser + ('_pk' if explicit_pk else '_hidden')
                pk = ',PRIMARY KEY(tenant,id)' if explicit_pk else ''
                c.execute(f'CREATE TABLE {table}(tenant INT,id INT,body TEXT{pk})')
                docs = [(i % 3, i, None if i % 43 == 0 else ' '.join(
                    term for n, term in enumerate(terms) if i % (n + 2) == 0)) for i in range(1, 514)]
                c.executemany(f'INSERT INTO {table} VALUES(%s,%s,%s)', docs)
                c.execute(f'ALTER TABLE {table} ADD FULLTEXT INDEX ft(body) WITH PARSER {parser}')

                def verify():
                    for term in queries:
                        for limit, offset in ((0, 0), (1, 0), (65, 63), (600, 0)):
                            suffix = f' ORDER BY tenant,id LIMIT {limit} OFFSET {offset}'
                            match = 'MATCH(body) AGAINST(%s)'
                            c.execute(f'SELECT tenant,id FROM {table} WHERE {match}' + suffix, (term,))
                            actual = c.fetchall()
                            c.execute(f'SELECT tenant,id FROM {table} WHERE {match}>0' + suffix, (term,))
                            assert actual == c.fetchall(), (parser, explicit_pk, term, limit, offset)
                            report['checks'] += 1

                verify()
                c.execute('START TRANSACTION')
                c.execute(f"UPDATE {table} SET body='alpha delta zeta' WHERE id BETWEEN 31 AND 130")
                c.execute(f'DELETE FROM {table} WHERE id BETWEEN 200 AND 250')
                verify()
                c.execute('ROLLBACK')
                verify()
                c.execute(f"UPDATE {table} SET body='beta gamma epsilon' WHERE id BETWEEN 31 AND 130")
                c.execute(f'DELETE FROM {table} WHERE id BETWEEN 200 AND 250')
                verify()
                for operator in ('', '>0'):
                    c.execute('PREPARE small_union_stmt FROM %s', (
                        f'SELECT tenant,id FROM {table} WHERE MATCH(body) AGAINST(?){operator} ORDER BY tenant,id LIMIT 65 OFFSET 63',))
                    values = []
                    for term in queries + queries[::-1]:
                        c.execute('SET @small_union_term=%s', (term,))
                        c.execute('EXECUTE small_union_stmt USING @small_union_term')
                        values.append(c.fetchall())
                    c.execute('DEALLOCATE PREPARE small_union_stmt')
                    if operator:
                        assert values == scoreless_values, (parser, explicit_pk, 'prepared reuse')
                    else:
                        scoreless_values = values
                report['checks'] += len(values)
        report['passed'] = True
finally:
    db.close()
    Path(a.output).write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report))
