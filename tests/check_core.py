"""Exercise the local SQL adapter, recovery, and exact search against an oracle."""

import argparse
import contextlib
import json
import math
from pathlib import Path
import queue
import random
import subprocess
import tempfile
import threading
import uuid


class Server:
    def __init__(self, args, data, memory=None):
        data.mkdir(exist_ok=True)
        self.container = None
        if args.image:
            self.container = "mvdb-check-" + uuid.uuid4().hex[:12]
            command = ["docker", "run", "--rm", "-i", "--network=none", "--cpus=2",
                       "--name", self.container, "--mount", f"type=bind,source={data},target=/data"]
            if memory:
                command += ["--memory", str(memory), "--memory-swap", str(memory)]
            command.append(args.image)
        else:
            command = [str(args.binary), "--data-dir", str(data)]
        self.log = tempfile.TemporaryFile(mode="w+t", encoding="utf-8")
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=self.log, text=True, encoding="utf-8", bufsize=1)
        self.lines = queue.Queue()
        self.reader = threading.Thread(target=self.read_output, daemon=True)
        self.reader.start()
        try:
            assert self.line() == "READY\tminivectordb-local-v1"
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

    def sql(self, statement):
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
    size = (directory / "store.wal").stat().st_size
    assert size > memory
    with Server(args, directory, memory) as db:
        db.query("SELECT id FROM wide WHERE id>=100 ORDER BY L2_DISTANCE(v,[1599,0]) LIMIT 5;",
                 [[1599], [1598], [1597], [1596], [1595]])
        db.query("SELECT id FROM wide ORDER BY note LIMIT 3;", [[0], [11], [22]])
        expected = sorted(range(rows), key=lambda key: (-(key * 37 % 11), key))
        db.query("SELECT id FROM wide ORDER BY note DESC;", [[key] for key in expected])
    print(f"{size} bytes of persisted rows reopened, searched and sorted under a {memory}-byte container limit",
          flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    launch = parser.add_mutually_exclusive_group(required=True)
    launch.add_argument("--binary", type=Path)
    launch.add_argument("--image")
    parser.add_argument("--large", action="store_true", help="also use the 32 MiB Docker memory limit case")
    args = parser.parse_args()
    if args.large and not args.image:
        parser.error("--large requires --image")
    if args.binary:
        args.binary = args.binary.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="mvdb-check-") as work:
        root = Path(work).resolve()
        check_sql(args, root / "sql")
        check_transactions(args, root / "transactions")
        check_crash(args, root / "crash")
        check_exact_oracle(args, root / "oracle")
        if args.large:
            check_disk_budget(args, root / "large")
    print("Local integration checks passed", flush=True)


if __name__ == "__main__":
    main()
