#!/usr/bin/env python3
"""Compare bare MATCH filters across batches, rescans, DML and exact scores."""
import argparse
from decimal import Decimal
import hashlib
import json
from pathlib import Path

import pymysql


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--port", type=int, default=2881)
parser.add_argument("--revision", required=True)
parser.add_argument("--output", required=True)
parser.add_argument("--reference")
args = parser.parse_args()
report = {"revision": args.revision, "cases": [], "passed": False,
          "suite_source_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
db = pymysql.connect(host="127.0.0.1", port=args.port, user="root",
                     charset="utf8mb4", autocommit=True, read_timeout=180)
try:
    with db.cursor() as cursor:
        cursor.execute("SET ob_query_timeout=180000000")
        cursor.execute("DROP DATABASE IF EXISTS fts_bool_score_regression")
        cursor.execute("CREATE DATABASE fts_bool_score_regression")
        cursor.execute("USE fts_bool_score_regression")

        def capture(name, sql, values=()):
            cursor.execute(sql, values)
            rows = [[v.hex() if isinstance(v, float) else str(v) if isinstance(v, Decimal) else v
                     for v in row]
                    for row in cursor.fetchall()]
            report["cases"].append({"name": name, "rows": rows})

        documents = [(i, None if i % 89 == 0 else "" if i % 43 == 0 else
                      "database " * (1 + i % 13) +
                      ("audit " * (1 + i % 7) if i % 9 else "") +
                      "全文索引 数据库 " * (1 + i % 3)) for i in range(1, 2058)]
        for tokenizer in ("beng", "ik"):
            table = "docs_" + tokenizer
            cursor.execute(f"CREATE TABLE {table}(id INT PRIMARY KEY, body TEXT)")
            for offset in range(0, len(documents), 200):
                cursor.executemany(f"INSERT INTO {table} VALUES(%s,%s)", documents[offset:offset + 200])
            cursor.execute(f"ALTER TABLE {table} ADD FULLTEXT INDEX ft(body) WITH PARSER {tokenizer}")
            for index, (term, mode) in enumerate((
                ("database audit", ""), ("全文索引 数据库", ""), ("", ""),
                ("missinguniqueterm", ""), ("+database -audit", "IN BOOLEAN MODE"),
            )):
                match = f"MATCH(body) AGAINST(%s {mode})"
                for limit in (0, 1, 64, 65, 513, 2057):
                    capture(f"{tokenizer}/{index}/limit/{limit}",
                            f"SELECT id,{match} FROM {table} WHERE {match} ORDER BY id LIMIT {limit}",
                            (term, term))
                capture(f"{tokenizer}/{index}/aggregate",
                        f"SELECT COUNT(*),SUM(id) FROM {table} WHERE {match}", (term,))
                capture(f"{tokenizer}/{index}/threshold",
                        f"SELECT id,{match} FROM {table} WHERE {match}>0.25 ORDER BY id",
                        (term, term))
            prepared = (f"SELECT id,MATCH(body) AGAINST(?) FROM {table} "
                        "WHERE MATCH(body) AGAINST(?) ORDER BY id LIMIT 129 OFFSET 63")
            cursor.execute("PREPARE bool_query FROM %s", (prepared,))
            for index, term in enumerate(("database", "", "audit", "missinguniqueterm", "database", "audit")):
                cursor.execute("SET @bool_term=%s", (term,))
                capture(f"{tokenizer}/prepared/{index}",
                        "EXECUTE bool_query USING @bool_term,@bool_term")
            cursor.execute("DEALLOCATE PREPARE bool_query")
            cursor.execute(f"UPDATE {table} SET body='fresh database' WHERE id BETWEEN 60 AND 130")
            cursor.execute(f"DELETE FROM {table} WHERE id BETWEEN 510 AND 520")
            capture(f"{tokenizer}/after-dml",
                    f"SELECT id,MATCH(body) AGAINST('fresh database') FROM {table} "
                    "WHERE MATCH(body) AGAINST('fresh database') ORDER BY id")
    if args.reference:
        expected = json.loads(Path(args.reference).read_text())["cases"]
        assert report["cases"] == expected, "Complete row identities, scores, or aggregates differ"
    report["result_sha256"] = hashlib.sha256(json.dumps(report["cases"], sort_keys=True).encode()).hexdigest()
    report["passed"] = True
finally:
    db.close()
    Path(args.output).write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
print(json.dumps({k: v for k, v in report.items() if k != "cases"}))
