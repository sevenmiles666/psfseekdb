"""Check decimal conversion, WAL compatibility and exact reranking."""
import argparse
import json
import math
from pathlib import Path
import random
import struct
import tempfile
from types import SimpleNamespace
import zlib

from check_core import Server


def config(binary):
    return SimpleNamespace(binary=binary.resolve(), image=None)


def frame_checks(directory):
    raw = (directory / "store.wal").read_bytes()
    offset, count = 8, 0
    while offset < len(raw):
        length, checksum = struct.unpack_from("<II", raw, offset)
        payload = raw[offset + 8:offset + 8 + length]
        assert len(payload) == length and zlib.crc32(payload) == checksum
        offset += 8 + length
        count += 1
    assert offset == len(raw)
    return count


def decimals(reference, candidate, root):
    rng = random.Random(20261116)
    literals = ["0", "-0", "+0", "0.", "-0.000", ".125", "-.125", "+.125",
                "999999", "-999999", "1e-44", "1e-46", "16777217", "1.234567890123",
                "1.0000000596046447", "1.0000000596046448", "-1.0000000596046447",
                "-1.0000000596046448", "999999999999999999", "0.00000000000000001"]
    for _ in range(12000):
        numerator = rng.randrange(1000000)
        fraction = rng.randrange(4)
        digits = str(numerator).zfill(fraction + 1)
        text = digits if fraction == 0 else digits[:-fraction] + "." + digits[-fraction:]
        literals.append(rng.choice(("", "+", "-")) + text)
    for _ in range(12000):
        digits = str(rng.randrange(10**17, 10**18))
        fraction = rng.randrange(18)
        text = digits if fraction == 0 else digits[:-fraction] + "." + digits[-fraction:]
        literals.append(rng.choice(("", "+", "-")) + text)
    results = []
    for label, binary in (("reference", reference), ("candidate", candidate)):
        directory = root / label
        with Server(config(binary), directory) as db:
            db.ok("CREATE TABLE d (id BIGINT PRIMARY KEY,v VECTOR(1),s VARCHAR(8192));")
            for start in range(0, len(literals), 100):
                batch = literals[start:start + 100]
                db.ok("INSERT INTO d VALUES " + ",".join(
                    f"({start+i},[{value}],'{'x' * (i % 67)}')" for i, value in enumerate(batch)), len(batch))
            rows, count, error = db.sql("SELECT id,v FROM d ORDER BY id;")
            assert count == len(literals) and error is None
            bits = [struct.pack("<f", row[1][0]).hex() for row in rows]
            # JSON's integer token -0 loses its sign in Python's decoder. The
            # byte-identical WAL assertion below also checks the stored sign.
            if label == "reference":
                # A Python double can itself round a literal onto a float32
                # midpoint. Use the unchanged engine as conversion reference.
                reference_bits = bits
            else:
                assert bits == reference_bits
            errors = [db.sql(f"INSERT INTO d VALUES (-1,[{value}],'');")[2]
                      for value in ("+-1", "+-0", "--1", ".", "1e", "1e+", "1.2.3", "1e99")]
            assert all(errors)
            results.append((bits, errors))
        frame_checks(directory)
    assert results[0] == results[1]
    assert (root / "reference/store.wal").read_bytes() == (root / "candidate/store.wal").read_bytes()
    return len(literals)


def cached_vectors(reference, candidate, directory):
    rng = random.Random(20261117)
    vectors = [list(struct.unpack("<128f", struct.pack("<128f", *
                [rng.uniform(-4, 4) for _ in range(128)]))) for _ in range(4609)]
    queries = [vectors[17], vectors[200], [9.0] * 128]
    literal = lambda v: json.dumps(v, separators=(",", ":"))
    with Server(config(reference), directory) as db:
        db.ok("CREATE TABLE t (id BIGINT PRIMARY KEY,a BIGINT,b BIGINT,v VECTOR(128));")
        db.ok("CREATE TABLE u (id BIGINT PRIMARY KEY,s VARCHAR(8192));")
        for start in range(0, len(vectors), 100):
            batch = vectors[start:start + 100]
            db.ok("INSERT INTO t VALUES " + ",".join(
                f"({start+i},{(start+i)%3},{(start+i)%7},{literal(v)})" for i, v in enumerate(batch)), len(batch))

    def verify(db):
        for turn in range(12):
            q = queries[turn % len(queries)]
            allowed = [i for i in range(len(vectors)) if turn % 2 == 0 or i % 3 == 1 and i % 7 == 2]
            expected = sorted(((math.sqrt(sum((x-y)**2 for x,y in zip(vectors[i], q))), i)
                               for i in allowed))[:10]
            where = "" if turn % 2 == 0 else " WHERE a=1 AND b=2"
            rows, count, error = db.sql(
                f"SELECT id,L2_DISTANCE(v,{literal(q)}) AS d,v FROM t{where} ORDER BY d LIMIT 10;")
            assert error is None and count == 10
            assert [row[0] for row in rows] == [key for _, key in expected]
            for row, (distance, key) in zip(rows, expected):
                assert math.isclose(row[1], distance, rel_tol=1e-12, abs_tol=1e-12)
                assert struct.pack("<128f", *row[2]) == struct.pack("<128f", *vectors[key])

    with Server(config(candidate), directory) as db:
        verify(db)
        db.ok("CREATE VECTOR INDEX vi ON t(v);")
        verify(db)
        # Another table can change without invalidating t's cache.
        db.ok("INSERT INTO u VALUES (1,'before');", 1)
        verify(db)
        db.ok("BEGIN;")
        db.ok(f"UPDATE t SET v={literal(queries[2])} WHERE id=17;", 1)
        db.ok("ROLLBACK;")
        verify(db)
        # Compact a different table while the t cache remains live.
        for i in range(1100):
            db.ok(f"UPDATE u SET s='{'x' * 8190}{i % 10}' WHERE id=1;", 1)
        verify(db)
        db.ok("BEGIN;")
        db.ok("DELETE FROM t WHERE id=17;", 1)
        db.stop(crash=True)
    with Server(config(reference), directory) as db:
        verify(db)
    return frame_checks(directory)


def integer_blocks(candidate, directory):
    rng = random.Random(20261118)
    vectors = [[rng.randrange(-127, 128) / 16 for _ in range(128)] for _ in range(4801)]
    vectors[-1] = vectors[0]
    queries = [[4.0] * 128, [4.0, 4.0625] * 64, [-4.0, -4.0625] * 64,
               [7.9375] * 128, [0.0] * 128, vectors[0]]
    with Server(config(candidate), directory) as db:
        db.ok("CREATE TABLE t (id BIGINT PRIMARY KEY,v VECTOR(128));")
        for start in range(0, len(vectors), 100):
            batch = vectors[start:start + 100]
            db.ok("INSERT INTO t VALUES " + ",".join(
                f"({start+i},{json.dumps(v)})" for i, v in enumerate(batch)), len(batch))
        for q in queries:
            distances = [(math.sqrt(sum((x-y)**2 for x,y in zip(v,q))), i) for i,v in enumerate(vectors)]
            for descending in (False, True):
                expected = sorted(distances, key=lambda x: (x[0], -x[1] if descending else x[1]))[:10]
                rows, count, error = db.sql(
                    f"SELECT id,L2_DISTANCE(v,{json.dumps(q)}) AS d FROM t ORDER BY d,id "
                    f"{'DESC' if descending else 'ASC'} LIMIT 10;")
                assert error is None and count == 10
                assert rows == [[key, distance] for distance, key in expected]
    return len(queries) * 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="mvdb-performance-paths-") as temp:
        root = Path(temp)
        count = decimals(args.reference, args.candidate, root)
        print(f"{count} decimal literals, invalid literals and byte-identical WAL: PASS", flush=True)
        frames = cached_vectors(args.reference, args.candidate, root / "mapped")
        print(f"Cached rerank, predicates, projection, rollback, checkpoint, SIGKILL, old-version reopen; {frames} CRCs: PASS", flush=True)
        count = integer_blocks(args.candidate, root / "integer")
        print(f"{count} large exact scans at signed-dot saturation boundaries, partial blocks and primary-key ties: PASS", flush=True)


if __name__ == "__main__":
    main()
