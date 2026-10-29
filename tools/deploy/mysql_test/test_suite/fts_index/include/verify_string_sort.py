#!/usr/bin/env python3
"""Compare complete sort, grouping and join results across registered collations."""
import argparse
from decimal import Decimal
import hashlib
import json
from pathlib import Path
import random
import re

import pymysql

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--port", type=int, default=2881)
parser.add_argument("--revision", required=True)
parser.add_argument("--output", required=True)
parser.add_argument("--reference")
args = parser.parse_args()
report = {"revision": args.revision, "cases": [], "passed": False,
          "suite_source_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
db = pymysql.connect(host="127.0.0.1", port=args.port, user="root", charset="utf8mb4",
                     autocommit=True, read_timeout=180)
try:
    with db.cursor() as cursor:
        cursor.execute("SET ob_query_timeout=180000000")
        cursor.execute("SET sql_mode=''")
        cursor.execute("DROP DATABASE IF EXISTS string_sort_regression")
        cursor.execute("CREATE DATABASE string_sort_regression")
        cursor.execute("USE string_sort_regression")

        def capture(name, sql, values=()):
            cursor.execute(sql, values)
            report["cases"].append({"name": name, "rows": [
                [v.hex() if isinstance(v, bytes) else str(v) if isinstance(v, Decimal) else v for v in row]
                for row in cursor.fetchall()]})

        cursor.execute("SHOW COLLATION")
        collations = sorted((row[0], row[1]) for row in cursor.fetchall())
        report["collations"] = collations
        rng = random.Random(48191)
        values = [None, "", " ", "  ", "a", "A", "a ", "a  ", "á", "ä", "ß", "ss",
                  "数据库", "数据", "全文索引", "😀", "😃", "a\0b", "a\0c", "\0", "\t", "\n"]
        for length in (1, 7, 8, 15, 16, 31, 32, 63, 64, 65, 127, 128, 511, 1024):
            values += ["x" * length, "x" * length, "x" * (length - 1) + "y"]
        values += ["数据" * 10, "数据" * 10, "数据" * 10 + " "]
        values += [rng.choice(["a", "A", "é", "数据库", "", "aa "]) for _ in range(70)]
        for collation, charset in collations:
            assert re.fullmatch(r"[a-zA-Z0-9_]+", collation)
            assert re.fullmatch(r"[a-zA-Z0-9_]+", charset)
            cursor.execute(f"CREATE TABLE strings(id INT PRIMARY KEY, "
                           f"v VARCHAR(4096), t TEXT, c CHAR(64)) "
                           f"CHARACTER SET `{charset}` COLLATE `{collation}`")
            for index, value in enumerate(values):
                cursor.execute(f"INSERT INTO strings VALUES(%s,CONVERT(%s USING {charset}),"
                               f"CONVERT(%s USING {charset}),LEFT(CONVERT(%s USING {charset}),64))",
                               (index, value, value, value))
            for column in ("v", "t", "c"):
                for direction in ("ASC", "DESC"):
                    capture(f"{collation}/{column}/{direction}",
                            f"SELECT id,HEX({column}) FROM strings ORDER BY {column} {direction},id")
                capture(f"{collation}/{column}/group",
                        f"SELECT COUNT(*),MIN(id),MAX(id),SUM(id) FROM strings "
                        f"GROUP BY {column} ORDER BY MIN(id)")
                capture(f"{collation}/{column}/join",
                        f"SELECT a.id,b.id FROM strings a JOIN strings b ON a.{column}=b.{column} "
                        "WHERE a.id<22 ORDER BY a.id,b.id")
            capture(f"{collation}/pairwise",
                    "SELECT a.id,b.id,SIGN(STRCMP(a.v,b.v)),a.v=b.v,a.v<b.v,a.v<=>b.v "
                    "FROM strings a CROSS JOIN strings b WHERE a.id<22 AND b.id<22 ORDER BY a.id,b.id")
            capture(f"{collation}/page",
                    "SELECT id,HEX(v) FROM strings ORDER BY v,id LIMIT 65 OFFSET 63")
            cursor.execute("DROP TABLE strings")

        # Binary inputs let expression comparators see malformed sequences too.
        cursor.execute("CREATE TABLE raw_strings(id INT PRIMARY KEY,v VARBINARY(1024))")
        raw = [b"", b"\xff", b"\xc0\x80", b"\xed\xa0\x80", b"\xf4\x90\x80\x80", b"\xe4\xb8",
               b"\x80", b"a\0b", b"a ", b"A", b"a"]
        raw += [bytes(rng.randrange(256) for _ in range(n)) for n in (1, 2, 3, 7, 16, 31, 63, 64, 65)]
        cursor.executemany("INSERT INTO raw_strings VALUES(%s,%s)", list(enumerate(raw + raw)))
        for collation, charset in collations:
            expression = f"CAST(v AS CHAR CHARACTER SET `{charset}`) COLLATE `{collation}`"
            capture(f"{collation}/raw-sort", f"SELECT id,HEX(v) FROM raw_strings ORDER BY {expression},id")
            capture(f"{collation}/raw-group", "SELECT COUNT(*),MIN(id),MAX(id) FROM raw_strings "
                    f"GROUP BY {expression} ORDER BY MIN(id)")
        if args.reference:
            expected = json.loads(Path(args.reference).read_text())
            assert report["cases"] == expected["cases"], "Complete sorted rows or comparison results differ"
        report["result_sha256"] = hashlib.sha256(json.dumps(report["cases"], sort_keys=True).encode()).hexdigest()
        report["passed"] = True
finally:
    db.close()
    Path(args.output).write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
print(json.dumps({k: v for k, v in report.items() if k != "cases"}))
