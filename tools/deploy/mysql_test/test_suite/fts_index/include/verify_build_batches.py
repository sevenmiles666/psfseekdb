#!/usr/bin/env python3
"""Compare long-document postings with DML IDs and frozen scalar-build scores.

Documents exceed typical posting batch boundaries. Compare complete IDs across
DML and post-build paths, and exact BM25 scores against each path's frozen
baseline after construction, updates, deletes and rollback. Existing estimators
produce different BM25 statistics for these two construction paths; preserving
both results independently avoids assuming that their scores are identical.
"""
import argparse
import json
import os
from pathlib import Path
import time

import pymysql


def word(index):
    return "prefix" + "".join(chr(97 + (index // (26 ** digit)) % 26) for digit in range(4))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=2881)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--reference")
    parser.add_argument("--freeze", action="store_true",
                        help="Flush source documents to SSTables before post-building each index")
    args = parser.parse_args()
    report = dict(revision=args.revision, started=time.time(), checks=0, cases=[], passed=False)
    db = pymysql.connect(host="127.0.0.1", port=args.port, user="root",
                         password=os.environ.get("SEEKDB_TEST_PASSWORD", ""),
                         charset="utf8mb4", autocommit=True, read_timeout=180)
    words = [word(i) for i in range(2051)]
    documents = [(1, " ".join(words)), (2, " ".join(words[::2])),
                 (3, " ".join(words[250:270] * 5)), (4, ""), (5, None),
                 (6, " ".join(words[1020:1040] * 3)),
                 (7, " ".join(words[:257])), (8, " ".join(words[-257:]))]
    queries = [words[i] for i in (0, 1, 254, 255, 256, 257, 511, 512, 1023,
                                  1024, 2047, 2048, 2050)]
    queries += [" ".join(words[i:i + 4]) for i in (0, 254, 510, 1022, 2047)]
    try:
        with db.cursor() as cursor:
            cursor.execute("DROP DATABASE IF EXISTS fts_build_batches")
            cursor.execute("CREATE DATABASE fts_build_batches")
            cursor.execute("USE fts_build_batches")
            for tokenizer in ("ik", "beng", "space"):
                for name in ("incremental", "postbuilt"):
                    cursor.execute(f"CREATE TABLE {name}(id BIGINT PRIMARY KEY, content LONGTEXT) CHARSET utf8mb4")
                    if name == "incremental":
                        cursor.execute(f"ALTER TABLE {name} ADD FULLTEXT INDEX ft(content) WITH PARSER {tokenizer}")
                    cursor.executemany(f"INSERT INTO {name} VALUES(%s,%s)", documents)
                    if name == "postbuilt":
                        if args.freeze:
                            cursor.execute("ALTER SYSTEM MINOR FREEZE")
                            deadline = time.monotonic() + 120
                            while True:
                                cursor.execute("SELECT COUNT(*) FROM oceanbase.__all_virtual_tablet_memstore_info "
                                               "WHERE is_active='NO'")
                                pending = cursor.fetchone()[0]
                                if pending == 0:
                                    break
                                assert time.monotonic() < deadline, "Source minor freeze did not finish"
                                time.sleep(0.2)
                            cursor.execute("SELECT COUNT(*) FROM oceanbase.__all_virtual_tablet_sstable_macro_info "
                                           "WHERE row_count > 0 AND tablet_id IN "
                                           "(SELECT tablet_id FROM oceanbase.__all_table WHERE table_name='postbuilt' "
                                           "AND database_id IN (SELECT database_id FROM oceanbase.__all_database "
                                           "WHERE database_name='fts_build_batches'))")
                            blocks = cursor.fetchone()[0]
                            assert blocks > 0, "No persisted source blocks; SSTable path was not exercised"
                            report.setdefault("persisted_sources", []).append(dict(tokenizer=tokenizer, blocks=blocks))
                        cursor.execute(f"ALTER TABLE {name} ADD FULLTEXT INDEX ft(content) WITH PARSER {tokenizer}")

                def compare(phase):
                    for query in queries:
                        for projection in ("id", "id,MATCH(content) AGAINST(%s)"):
                            outputs = []
                            for name in ("incremental", "postbuilt"):
                                sql = f"SELECT {projection} FROM {name} WHERE MATCH(content) AGAINST(%s) ORDER BY id"
                                params = (query, query) if "%s" in projection else (query,)
                                cursor.execute(sql, params)
                                rows = cursor.fetchall()
                                outputs.append([[int(row[0])] + ([float(row[1]).hex()] if len(row) > 1 else [])
                                                for row in rows])
                            assert [row[0] for row in outputs[0]] == [row[0] for row in outputs[1]], (
                                tokenizer, phase, query, projection, outputs)
                            report["cases"].append(dict(tokenizer=tokenizer, phase=phase,
                                                        query=query, projection=projection, outputs=outputs))
                            report["checks"] += 1

                compare("constructed")
                cursor.execute("START TRANSACTION")
                for name in ("incremental", "postbuilt"):
                    cursor.execute(f"UPDATE {name} SET content=%s WHERE id=3", (" ".join(words[::3]),))
                    cursor.execute(f"DELETE FROM {name} WHERE id=6")
                compare("uncommitted")
                cursor.execute("ROLLBACK")
                compare("rolled-back")
                for name in ("incremental", "postbuilt"):
                    cursor.execute(f"UPDATE {name} SET content=%s WHERE id=2", (" ".join(words[-513:]),))
                    cursor.execute(f"DELETE FROM {name} WHERE id=7")
                compare("committed")
                for name in ("incremental", "postbuilt"):
                    cursor.execute(f"DROP TABLE {name}")
        if args.reference:
            expected = json.loads(Path(args.reference).read_text())["cases"]
            assert report["cases"] == expected, "Complete row IDs or exact scores differ from scalar baseline"
        report["passed"] = True
        print(json.dumps({key: value for key, value in report.items() if key != "cases"}))
    except Exception as error:
        report["error"] = str(error)
        raise
    finally:
        db.close()
        report["finished"] = time.time()
        Path(args.output).write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")


if __name__ == "__main__":
    main()
