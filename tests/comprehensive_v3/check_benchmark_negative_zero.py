"""Check that untimed persistence validation distinguishes JSON -0 and 0."""
import argparse
import json
from pathlib import Path
import struct
import tempfile
from types import SimpleNamespace

from benchmark_local import SignedZeroCheckServer


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--image")
    target.add_argument("--binary", type=Path)
    args = parser.parse_args()
    # Exercise the actual request decoder without changing the timed adapter.
    decoder = object.__new__(SignedZeroCheckServer)
    class Sink:
        def write(self, text):
            json.loads(text)
        def flush(self):
            pass
    decoder.process = SimpleNamespace(stdin=Sink())
    decoder.line = lambda: '{"id":1,"ok":true,"rows":[[-0,0,-0.0,1]]}'
    reply = decoder.request({"id": 1})
    assert struct.pack("<4f", *reply["rows"][0]) == struct.pack("<4f", -0.0, 0.0, -0.0, 1.0)
    assert type(reply["id"]) is int and type(reply["rows"][0][3]) is int
    with tempfile.TemporaryDirectory(prefix="mvdb-zero-") as directory:
        data = Path(directory)
        with SignedZeroCheckServer(args, data) as db:
            db.ok("CREATE TABLE zeros (id BIGINT PRIMARY KEY, v VECTOR(2));")
            db.ok("INSERT INTO zeros VALUES (1,'[-0.00000,0.00000]');", 1)
        with SignedZeroCheckServer(args, data) as db:
            rows, count, error = db.sql("SELECT v FROM zeros WHERE id=1;")
            assert count == 1 and error is None
            assert struct.pack("<2f", *rows[0][0]) == struct.pack("<2f", -0.0, 0.0)
    print("JSON -0 sign, integer IDs, and restart persistence passed", flush=True)


if __name__ == "__main__":
    main()
