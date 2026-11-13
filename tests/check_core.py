"""Exercise the local SQL adapter, recovery, and exact search against an oracle."""

import argparse
import contextlib
import json
import math
from pathlib import Path
import queue
import random
import struct
import subprocess
import tempfile
import threading
import uuid
import zlib


class Server:
    def __init__(self, args, data, memory=None, *, mode="exact", volume=None, probe=None):
        if data is not None:
            data.mkdir(exist_ok=True)
        self.container = None
        self.mode = mode
        self.sequence = 0
        if args.image:
            self.container = "mvdb-check-" + uuid.uuid4().hex[:12]
            mount = f"type=volume,source={volume},target=/data" if volume else f"type=bind,source={data},target=/data"
            command = ["docker", "run", "--rm", "-i", "--network=none", "--cpus=2",
                       "--name", self.container, "--mount", mount]
            if memory:
                command += ["--memory", str(memory), "--memory-swap", str(memory)]
            if probe:
                command += ["--mount", f"type=bind,source={probe},target=/bench-probe,readonly"]
            command.append(args.image)
            if mode != "exact":
                command += ["--search-mode", mode]
        else:
            command = [str(args.binary), "--data-dir", str(data)]
            if mode != "exact":
                command += ["--search-mode", mode]
        self.log = tempfile.TemporaryFile(mode="w+t", encoding="utf-8")
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=self.log, text=True, encoding="utf-8", bufsize=1)
        self.lines = queue.Queue()
        self.reader = threading.Thread(target=self.read_output, daemon=True)
        self.reader.start()
        try:
            greeting = self.line()
            self.legacy = greeting == "READY\tminivectordb-local-v1"
            if not self.legacy:
                assert json.loads(greeting) == {"type": "ready"}, greeting
        except BaseException:
            self.stop(crash=True)
            raise

    def read_output(self):
        try:
            for line in self.process.stdout:
                self.lines.put(line.rstrip("\r\n"))
        finally:
            self.lines.put(None)

    def line(self):
        try:
            output = self.lines.get(timeout=25)
        except queue.Empty:
            output = None
        if output is None:
            self.log.seek(0)
            raise AssertionError(f"no response, process status={self.process.poll()}\n{self.log.read()}")
        return output

    def request(self, message, *, ensure_ascii=False):
        self.process.stdin.write(json.dumps(message, ensure_ascii=ensure_ascii) + "\n")
        self.process.stdin.flush()
        response = json.loads(self.line())
        assert type(response["id"]) is type(message["id"]) and response["id"] == message["id"]
        assert type(response["ok"]) is bool
        return response

    def sql(self, statement):
        if not self.legacy:
            self.sequence += 1
            response = self.request({"id": self.sequence, "op": "execute",
                                     "sql": statement, "search_mode": self.mode})
            if not response["ok"]:
                return [], None, response["error"]["code"]
            if "columns" in response:
                rows = response["rows"]
                assert all(len(row) == len(response["columns"]) for row in rows)
                return rows, len(rows), None
            return [], response.get("affected_rows", 0), None

        self.process.stdin.write(statement + "\n")
        self.process.stdin.flush()
        rows = []
        while True:
            fields = self.line().split("\t", 2)
            if fields[0] == "ROW":
                rows.append(json.loads(fields[1]))
            elif fields[0] == "OK":
                return rows, int(fields[1]), None
            elif fields[0] == "ERR":
                assert not rows, "error followed partial results"
                return rows, None, fields[1]
            else:
                raise AssertionError(f"invalid response {fields!r}")

    def ok(self, statement, count=0):
        assert self.sql(statement) == ([], count, None), statement[:200]

    def query(self, statement, expected):
        assert self.sql(statement) == (expected, len(expected), None), statement

    def error(self, statement, code):
        assert self.sql(statement) == ([], None, code), statement

    def stop(self, crash=False):
        if self.log.closed:
            return
        try:
            if crash and self.process.poll() is None:
                if self.container:
                    subprocess.run(["docker", "kill", "--signal=KILL", self.container],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=15)
                else:
                    self.process.kill()
            with contextlib.suppress(OSError):
                self.process.stdin.close()
            status = self.process.wait(timeout=15)
            self.reader.join(timeout=3)
            if not crash and status != 0:
                self.log.seek(0)
                raise AssertionError(f"unclean shutdown: {status}\n{self.log.read()}")
        finally:
            if self.process.poll() is None:
                if self.container:
                    subprocess.run(["docker", "kill", "--signal=KILL", self.container],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
                self.process.kill()
                self.process.wait(timeout=5)
            self.process.stdout.close()
            self.log.close()

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback):
        self.stop(crash=exc_type is not None)


def check_protocol(args, directory):
    with Server(args, directory) as db:
        if db.legacy:
            return
        db.ok("CREATE TABLE unicode_rows (id BIGINT PRIMARY KEY, text VARCHAR(32));")
        identifier = 18446744073709551617
        created = db.request({"id": identifier, "op": "execute",
                              "sql": "INSERT INTO unicode_rows VALUES (9007199254740993,'你好😀;');"},
                             ensure_ascii=True)
        assert created["ok"] and created["affected_rows"] == 1
        selected = db.request({"id": "编号😀\n", "op": "execute",
                               "sql": 'SELECT id,text FROM unicode_rows\nORDER BY id;'},
                              ensure_ascii=True)
        assert selected["columns"] == ["id", "text"]
        assert selected["rows"] == [[9007199254740993, "你好😀;"]]
        invalid = db.request({"id": True, "op": "execute", "sql": "SELECT * FROM unicode_rows;",
                              "search_mode": 1})
        assert invalid["ok"] is False and invalid["error"]["code"] == "INVALID_REQUEST"
        db.error("SELECT text FROM unicode_rows ORDER BY missing;", "UNKNOWN_COLUMN")
        empty = db.request({"id": None, "op": "execute",
                            "sql": "SELECT text FROM unicode_rows LIMIT 0;"})
        assert empty["columns"] == ["text"] and empty["rows"] == []
        assert db.request({"id": "done", "op": "shutdown"}) == {"id": "done", "ok": True}
    print("JSONL ID types, Unicode escapes, empty columns, error recovery and shutdown passed", flush=True)


def check_sql(args, directory):
    with Server(args, directory) as db:
        db.ok("CREATE TABLE items (v VECTOR(2), label VARCHAR(2), id BIGINT, PRIMARY KEY(id));")
        db.ok("INSERT INTO items(id,label,v) VALUES (4,'乙😀','[0,4]'), (1,'甲',[1,0]),"
              "(3,'乙😀',[4,0]), (2,'甲',[2,0]);", 4)
        db.query("SELECT id,L2_DISTANCE(v,'[0,0]') AS d FROM items WHERE label='乙😀' "
                 "ORDER BY d LIMIT 10;", [[3, 4], [4, 4]])
        db.query("SELECT id FROM items WHERE id>=2 AND (id<=4 AND label>'甲') ORDER BY id DESC;", [])
        db.query("SELECT id FROM items ORDER BY label DESC;", [[1], [2], [3], [4]])
        db.query("SELECT id FROM items WHERE label='longer than the column width';", [])
        db.query("SELECT * FROM items WHERE id=2;", [[[2, 0], "甲", 2]])
        db.query("SELECT id FROM items ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 0;", [])
        db.error("INSERT INTO items VALUES ([1,0],'三个字',7);", "VARCHAR_LENGTH")
        db.error("INSERT INTO items VALUES ([1],'甲',7);", "VECTOR_DIMENSION")
        db.error("INSERT INTO items VALUES ([3.5e38,0],'甲',7);", "INVALID_VECTOR")
        db.error("INSERT INTO items VALUES ([NaN,0],'甲',7);", "SYNTAX")
        db.error("INSERT INTO items VALUES ('[Infinity,0]','甲',7);", "SYNTAX")
        db.error("UPDATE items SET id=7 WHERE id=2;", "UNSUPPORTED")
        db.error("UPDATE items SET label='乙' WHERE id=1 OR id=2;", "SYNTAX")
        db.ok("UPDATE items SET label='乙',v=[-3,4] WHERE id>1 AND id<3;", 1)
        db.query("SELECT id,L2_DISTANCE(v,[0,0]) FROM items WHERE id=2;", [[2, 5]])
        db.ok("DELETE FROM items WHERE id<=1;", 1)
        db.ok("CREATE TABLE numbers (id BIGINT PRIMARY KEY, name VARCHAR(9));")
        db.ok("INSERT INTO numbers VALUES (-9223372036854775808,'a''b;'),(9223372036854775807,'z');", 2)
        db.error("INSERT INTO numbers VALUES (9223372036854775808,'out');", "INVALID_NUMBER")
    with Server(args, directory) as db:
        db.query("SELECT id FROM items ORDER BY id;", [[2], [3], [4]])
        db.query("SELECT * FROM numbers ORDER BY id DESC;",
                 [[9223372036854775807, "z"], [-9223372036854775808, "a'b;"]])
    print("SQL, Unicode, L2, scalar order and persisted schema passed", flush=True)


def check_transactions(args, directory):
    with Server(args, directory) as db:
        db.ok("CREATE TABLE items (id BIGINT PRIMARY KEY, v VECTOR(2));")
        db.ok("INSERT INTO items VALUES (1,[1,0]),(2,[2,0]);", 2)
        db.ok("BEGIN;")
        db.ok("INSERT INTO items VALUES (3,[3,0]);", 1)
        db.error("INSERT INTO items VALUES (4,[4,0]),(3,[5,0]);", "DUPLICATE_KEY")
        db.error("INSERT INTO items VALUES (4,[4,0]),(5,[5]);", "VECTOR_DIMENSION")
        db.query("SELECT id FROM items ORDER BY id;", [[1], [2], [3]])
        db.ok("UPDATE items SET v=[0,0] WHERE id=3;", 1)
        db.ok("DELETE FROM items WHERE id=1;", 1)
        db.query("SELECT id FROM items ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 2;", [[3], [2]])
        db.ok("CREATE TABLE transient (id BIGINT PRIMARY KEY);")
        db.ok("INSERT INTO transient VALUES (1);", 1)
        db.error("BEGIN;", "TRANSACTION_ACTIVE")
        db.ok("ROLLBACK;")
        db.query("SELECT id FROM items ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 3;", [[1], [2]])
        db.error("SELECT * FROM transient;", "UNKNOWN_TABLE")
        db.error("ROLLBACK;", "NO_TRANSACTION")
        db.error("INSERT INTO items VALUES (9,[9,0]),(1,[0,0]);", "DUPLICATE_KEY")
        db.ok("BEGIN;")
        db.ok("INSERT INTO items VALUES (5,[5,0]);", 1)
        db.error("INSERT INTO items VALUES (6,[6,0]),(5,[7,0]);", "DUPLICATE_KEY")
        db.ok("COMMIT;")
        db.ok("BEGIN;")
        db.ok("DELETE FROM items;", 3)
        db.query("SELECT * FROM items;", [])
    with Server(args, directory) as db:
        db.query("SELECT id FROM items ORDER BY id;", [[1], [2], [5]])
    print("Transaction visibility, statement rollback, commit and EOF rollback passed", flush=True)


def check_crash(args, directory):
    journal = directory / "store.wal"
    with Server(args, directory) as db:
        db.ok("CREATE TABLE items (id BIGINT PRIMARY KEY, note VARCHAR(20000), v VECTOR(2));")
        db.ok("BEGIN;")
        db.ok("INSERT INTO items VALUES (1,'old',[1,0]),(2,'old',[2,0]);", 2)
        db.ok("COMMIT;")
        db.stop(crash=True)
    committed = journal.read_bytes()
    with Server(args, directory) as db:
        db.query("SELECT id,note FROM items ORDER BY id;", [[1, "old"], [2, "old"]])
        db.ok("BEGIN;")
        db.ok("UPDATE items SET note='uncommitted',v=[0,0] WHERE id=1;", 1)
        db.ok("DELETE FROM items WHERE id=2;", 1)
        db.ok("CREATE TABLE temporary (id BIGINT PRIMARY KEY);")
        large = "x" * 20000
        db.ok("INSERT INTO items VALUES " + ",".join(f"({i},'{large}',[0,0])" for i in range(3, 9)), 6)
        db.query("SELECT id FROM items WHERE id=8;", [[8]])
        db.stop(crash=True)
    assert journal.stat().st_size > len(committed) + 65536, "uncommitted writes did not reach the file"
    with Server(args, directory) as db:
        db.query("SELECT id,note FROM items ORDER BY id;", [[1, "old"], [2, "old"]])
        db.error("SELECT * FROM temporary;", "UNKNOWN_TABLE")
    assert journal.read_bytes() == committed
    with journal.open("ab") as output:
        output.write(b"\x15\x00\x00")
    with Server(args, directory) as db:
        db.query("SELECT id FROM items ORDER BY id;", [[1], [2]])
    assert journal.read_bytes() == committed
    with Server(args, directory) as db:
        db.ok("BEGIN;")
        db.ok("DELETE FROM items WHERE id=2;", 1)
        db.ok("UPDATE items SET note='new' WHERE id=1;", 1)
        db.ok("COMMIT;")
    complete = journal.read_bytes()
    journal.write_bytes(complete[:-1])
    for _ in range(2):
        with Server(args, directory) as db:
            db.query("SELECT id,note FROM items ORDER BY id;", [[1, "old"], [2, "old"]])
        assert journal.read_bytes() == committed
    print("SIGKILL after commit, uncommitted tail over 64 KiB, and repeatable torn-frame recovery passed",
          flush=True)


def check_exact_oracle(args, directory):
    randomizer = random.Random(20261109)
    count, dimension = 257, 128

    def vector():
        return [randomizer.randrange(-32, 33) / 8 for _ in range(dimension)]

    def literal(values):
        return json.dumps(values, separators=(",", ":"))

    data = [(key, key % 4, vector()) for key in range(count)]
    with Server(args, directory) as db:
        db.ok("CREATE TABLE vectors (id BIGINT PRIMARY KEY, bucket BIGINT, v VECTOR(128));")
        for start in range(0, count, 64):
            batch = data[start:start + 64]
            db.ok("INSERT INTO vectors VALUES " + ",".join(
                f"({key},{bucket},{literal(values)})" for key, bucket, values in batch), len(batch))
        for index in range(24):
            query = vector()
            bucket = index % 4 if index % 2 else None
            candidates = [(math.sqrt(sum((a - b) ** 2 for a, b in zip(values, query))), key)
                          for key, label, values in data if bucket is None or label == bucket]
            expected = sorted(candidates)[:10]
            where = f"WHERE bucket={bucket}" if bucket is not None else ""
            rows, returned, error = db.sql(f"SELECT id,L2_DISTANCE(v,{literal(query)}) AS d "
                                           f"FROM vectors {where} ORDER BY d LIMIT 10;")
            assert error is None and returned == 10
            assert [row[0] for row in rows] == [key for _, key in expected]
            assert all(math.isclose(actual[1], exact[0], rel_tol=1e-12)
                       for actual, exact in zip(rows, expected))
    print("24 independent exact TopK queries passed, including 12 filtered queries in 128 dimensions", flush=True)


def check_topk_rounding(args, directory):
    rng = random.Random(20261112)

    def f32(value):
        return struct.unpack("<f", struct.pack("<f", value))[0]

    def squared_distance(vector, query):
        total = 0.0
        for left, right in zip(vector, query):
            difference = left - right
            total += difference * difference
        return total

    with Server(args, directory) as db:
        workloads = (("blocks", 1.0, 2061, 128), ("ordinary", 1.0, 2200, 129),
                     ("tiny", 4e-23, 39, 129), ("large", 3e38, 39, 129))
        for table, scale, size, dimension in workloads:
            def vector():
                return [f32(rng.uniform(-1.0, 1.0) * scale) for _ in range(dimension)]

            data = [vector() for _ in range(size)]
            if table == "blocks":
                data[3] = [-1.0] * dimension
                data[4] = [1.0] * dimension
                data[8] = data[0]
                data[-1] = data[0]
            data[6] = data[0]
            db.ok(f"CREATE TABLE {table} (id BIGINT PRIMARY KEY, bucket BIGINT, tag VARCHAR(8), "
                  f"v VECTOR({dimension}));")
            if table == "ordinary":
                data[12] = data[0].copy()
                data[12][-1] = f32(data[12][-1] + 1e-5)
            rows = list(reversed(list(enumerate(data))))
            for offset in range(0, len(rows), 100):
                batch = rows[offset:offset + 100]
                values = ",".join(f"({key},{key % 3},'{['even', 'odd'][key % 2]}',{json.dumps(v)})"
                                  for key, v in batch)
                db.ok(f"INSERT INTO {table} VALUES {values};", len(batch))

            queries = [("WHERE bucket=0 AND tag='even'", lambda key: key % 6 == 0, data[0]),
                       ("WHERE tag='even'", lambda key: key % 2 == 0, data[0]),
                       ("", lambda key: True, vector())]
            if table == "ordinary":
                queries.append(("", lambda key: True, [2.0, *data[0][1:]]))
            if table == "blocks":
                queries.append(("", lambda key: True, data[0]))
                queries.append(("", lambda key: True, data[3]))
            for where, eligible, query in queries:
                expected = sorted((math.sqrt(squared_distance(v, query)), key)
                                  for key, v in enumerate(data) if eligible(key))[:5]
                output, count, error = db.sql(
                    f"SELECT id,v,L2_DISTANCE(v,{json.dumps(query)}) AS d FROM {table} "
                    f"{where} ORDER BY d LIMIT 5;")
                assert error is None and count == len(expected)
                assert [row[0] for row in output] == [key for _, key in expected]
                for (key, stored, distance), (exact, _) in zip(output, expected):
                    assert [f32(value) for value in stored] == data[key]
                    assert math.isclose(distance, exact, rel_tol=1e-12)

        db.ok("CREATE TABLE rounded (id BIGINT PRIMARY KEY, v VECTOR(2));")
        db.ok("INSERT INTO rounded VALUES (2,[1,0]),(1,[1,0.00000001490116119384765625]);", 2)
        rounded = "SELECT id,L2_DISTANCE(v,[0,0]) AS d FROM rounded ORDER BY d"
        db.query(rounded + " LIMIT 1;", [[1, 1.0]])
        db.query(rounded + ";", [[1, 1.0], [2, 1.0]])
        db.query(rounded + " DESC;", [[1, 1.0], [2, 1.0]])
    print("Exact TopK passed integer screening, FP32 ties, subnormals, overflow and filter cases", flush=True)


def check_wal_vector_formats(args, directory, reference=None):
    schema = "CREATE TABLE storage (label VARCHAR(16), v VECTOR(17), id BIGINT PRIMARY KEY, bucket BIGINT);"
    half = [-0.0, 65504.0, 2**-14, *([0.125, -3.5] * 7)]
    packed = [(-1)**i * (0.1 + i * 0.03125) for i in range(17)]
    full = [1e-39, 3e38, *([0.2] * 15)]
    rows = [half, packed, full]
    packed_bits = lambda values: struct.pack("<17f", *values)
    query = "SELECT v FROM storage ORDER BY id;"

    def insert(db, key):
        db.ok(f"INSERT INTO storage VALUES ('记录😀',{json.dumps(rows[key])},{key},{key % 2});", 1)

    def check(db):
        actual, count, error = db.sql(query)
        assert error is None and count == 3
        # JSON permits -0 and the Python parser returns that spelling as integer zero.
        assert [packed_bits(row[0]) for row in actual] == [packed_bits([abs(v) if v == 0 else v for v in vector])
                                                          for vector in rows]
        zeros = json.dumps([0] * 17)
        db.query(f"SELECT id,bucket,label FROM storage WHERE label='记录😀' AND bucket=0 "
                 f"ORDER BY L2_DISTANCE(v,{zeros}) LIMIT 2;", [[0, 0, "记录😀"], [2, 0, "记录😀"]])

    with Server(reference or args, directory) as db:
        db.ok(schema)
        insert(db, 2)
        db.ok("CREATE VECTOR INDEX formats ON storage(v);")
    with Server(args, directory) as db:
        insert(db, 0)
        insert(db, 1)
        check(db)
        db.ok("BEGIN;")
        db.ok(f"UPDATE storage SET v={json.dumps(full)} WHERE id=1;", 1)
        db.ok("DELETE FROM storage WHERE id=0;", 1)
        db.ok("ROLLBACK;")
        check(db)
        db.ok("BEGIN;")
        db.ok("UPDATE storage SET label='pending😀' WHERE id=2;", 1)
        db.stop(crash=True)
    with Server(args, directory, mode="ann") as db:
        check(db)

    # Check committed frame kinds so the comparison exercises every codec.
    journal = (directory / "store.wal").read_bytes()
    kinds = set()
    offset = 8
    while offset < len(journal):
        size, crc = struct.unpack_from("<II", journal, offset)
        frame = offset + 8
        assert zlib.crc32(journal[frame:frame + size]) == crc
        kind = journal[frame]
        kinds.add(kind)
        if kind == 6:
            text_size = struct.unpack_from("<I", journal, frame + 13)[0]
            assert struct.unpack_from("<H", journal, frame + 17 + text_size)[0] == 0x8000
        offset += 8 + size
    assert offset == len(journal) and {2, 6, 7} <= kinds
    print("Mixed vector WAL encodings survived bitwise round-trip, rollback and crash recovery", flush=True)


def check_index(args, directory):
    query = "SELECT id FROM items WHERE label='g' ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 1;"
    with Server(args, directory, mode="ann") as db:
        db.ok("CREATE TABLE items (label VARCHAR(4), id BIGINT PRIMARY KEY, stamp BIGINT, v VECTOR(2));")
        db.ok("INSERT INTO items VALUES ('g',1,5,[1,0.04]),('g',2,7,[1.0004,0]),('x',3,9,[0,0]);", 3)
        db.error("CREATE VECTOR INDEX wrong ON items(stamp);", "TYPE_MISMATCH")
        db.ok("BEGIN;")
        db.ok("CREATE VECTOR INDEX rolled_back ON items(v);")
        db.query(query, [[1]])
        db.ok("ROLLBACK;")
        db.query(query, [[2]])
        db.ok("CREATE VECTOR INDEX idx ON items(v);")
        db.error("CREATE VECTOR INDEX second ON items(v);", "INDEX_EXISTS")
        db.query(query, [[1]])
        db.query("SELECT id FROM items WHERE label='g' ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 10;",
                 [[2], [1]])
        db.query("SELECT id FROM items WHERE label='none' ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 10;", [])
        db.ok("BEGIN;")
        db.ok("UPDATE items SET v=[0,0] WHERE id=2;", 1)
        db.query(query, [[2]])
        db.ok("DELETE FROM items WHERE id=2;", 1)
        db.query(query, [[1]])
        db.ok("INSERT INTO items VALUES ('g',8,10,[0,0]);", 1)
        db.error("INSERT INTO items VALUES ('g',9,0,[0,0]),('g',8,0,[0,0]);", "DUPLICATE_KEY")
        db.query("SELECT id FROM items WHERE label='g' ORDER BY L2_DISTANCE(v,[0,0]) LIMIT 5;",
                 [[8], [1]])
        db.ok("ROLLBACK;")
        db.query(query, [[1]])
        db.ok("BEGIN;")
        db.ok("UPDATE items SET v=[-2,0] WHERE id=2;", 1)
        db.ok("COMMIT;")
        db.stop(crash=True)
    with Server(args, directory, mode="ann") as db:
        db.error("CREATE VECTOR INDEX again ON items(v);", "INDEX_EXISTS")
        db.query(query, [[1]])
        db.ok("BEGIN;")
        db.ok("UPDATE items SET v=[0,0] WHERE id=2;", 1)
        db.query(query, [[2]])
        db.stop(crash=True)
    with Server(args, directory, mode="ann") as db:
        db.query(query, [[1]])
    with Server(args, directory) as db:
        db.query(query, [[1]])
        db.ok("BEGIN;")
        db.ok("UPDATE items SET label='g' WHERE id=3;", 1)
        db.query(query, [[3]])
        db.ok("ROLLBACK;")
        db.query(query, [[1]])
        db.ok("UPDATE items SET v=[1.0004,0] WHERE id=2;", 1)
        db.query(query, [[2]])
    with Server(args, directory, mode="ann") as db:
        db.query(query, [[1]])
    print("Exact/ANN distinction, index DDL rollback, index changes, cache invalidation and restart passed",
          flush=True)


def check_disk_budget(args, directory):
    payload = "z" * 32760
    rows, batch = 1600, 16
    memory = 32 * 1024 * 1024
    with Server(args, directory, memory) as db:
        db.ok("CREATE TABLE wide (id BIGINT PRIMARY KEY, note VARCHAR(32768), v VECTOR(2));")
        for start in range(0, rows, batch):
            db.ok("INSERT INTO wide VALUES " + ",".join(
                f"({key},'{key * 37 % 11:08d}{payload}',[{key},0])"
                for key in range(start, start + batch)), batch)
        db.ok("CREATE VECTOR INDEX wide_vector ON wide(v);")
    size = (directory / "store.wal").stat().st_size
    assert size > memory
    with Server(args, directory, memory, mode="ann") as db:
        db.query("SELECT id FROM wide WHERE id>=100 ORDER BY L2_DISTANCE(v,[1599,0]) LIMIT 5;",
                 [[1599], [1598], [1597], [1596], [1595]])
        db.query("SELECT id FROM wide ORDER BY note LIMIT 3;", [[0], [11], [22]])
        expected = sorted(range(rows), key=lambda key: (-(key * 37 % 11), key))
        db.query("SELECT id FROM wide ORDER BY note DESC;", [[key] for key in expected])
    with Server(args, directory, memory) as db:
        db.query("SELECT id FROM wide ORDER BY L2_DISTANCE(v,[1599,0]) LIMIT 5;",
                 [[1599], [1598], [1597], [1596], [1595]])
    print(f"{size} bytes of rows reopened, indexed, searched and sorted under a {memory}-byte container limit",
          flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    launch = parser.add_mutually_exclusive_group(required=True)
    launch.add_argument("--binary", type=Path)
    launch.add_argument("--image")
    parser.add_argument("--reference-binary", type=Path, help="seed the mixed-format recovery test using an older build")
    parser.add_argument("--large", action="store_true", help="also use the 32 MiB Docker memory limit case")
    args = parser.parse_args()
    if args.large and not args.image:
        parser.error("--large requires --image")
    if args.binary:
        args.binary = args.binary.resolve(strict=True)
    reference = None
    if args.reference_binary:
        reference = argparse.Namespace(image=None, binary=args.reference_binary.resolve(strict=True))
    with tempfile.TemporaryDirectory(prefix="mvdb-check-") as work:
        root = Path(work).resolve()
        check_protocol(args, root / "protocol")
        check_sql(args, root / "sql")
        check_transactions(args, root / "transactions")
        check_crash(args, root / "crash")
        check_exact_oracle(args, root / "oracle")
        check_topk_rounding(args, root / "topk_rounding")
        check_wal_vector_formats(args, root / "wal_vector_formats", reference)
        check_index(args, root / "index")
        if args.large:
            check_disk_budget(args, root / "large")
    print("Local integration checks passed", flush=True)


if __name__ == "__main__":
    main()
