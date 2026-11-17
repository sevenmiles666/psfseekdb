"""Paired dccb61e-based Linux diagnostic with explicit SQL encodings (run as root)."""
import argparse
from contextlib import contextmanager
import ctypes
from decimal import Decimal, ROUND_HALF_UP
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
from statistics import median
import struct
import subprocess
import sys
import time
import tempfile
import uuid


def command(*args):
    return subprocess.check_output(args, text=True)


def block_device(filesystem_device):
    block = (Path("/sys/dev/block") / filesystem_device).resolve()
    if (block / "partition").exists():
        block = block.parent
    return (block / "dev").read_text().strip()


def resources(db):
    info = json.loads(command("docker", "inspect", db.container))[0]
    group = next(line.split(":", 2)[2] for line in
                 Path(f"/proc/{info['State']['Pid']}/cgroup").read_text().splitlines()
                 if line.startswith("0:"))
    cg = Path("/sys/fs/cgroup") / group.lstrip("/")
    assert "io" in (cg.parent / "cgroup.subtree_control").read_text().split()
    source = Path(next(m["Source"] for m in info["Mounts"] if m["Destination"] == "/data"))
    descriptor = os.open(source / "store.wal", os.O_RDONLY)
    try:
        assert ctypes.CDLL(None, use_errno=True).syncfs(descriptor) == 0
    finally:
        os.close(descriptor)
    # Keep the database/cgroup alive while writeback completes. No probe is
    # executed inside the measured container.
    previous = None
    for _ in range(20):
        counters = (cg / "io.stat").read_text()
        if counters == previous:
            break
        previous = counters
        time.sleep(.05)
    else:
        raise AssertionError("writeback did not settle")
    stat = (source / "store.wal").stat()
    filesystem_device = f"{os.major(stat.st_dev)}:{os.minor(stat.st_dev)}"
    device = block_device(filesystem_device)
    devices = {fields[0]: {k: int(v) for k, v in
                         (field.split("=", 1) for field in fields[1:])}
               for fields in (line.split() for line in counters.splitlines())}
    assert device in devices, ("data-device writeback was not charged", device, devices)
    io = devices[device]
    assert io["wbytes"] > 0
    peak = int((cg / "memory.peak").read_text())
    limit = int((cg / "memory.max").read_text())
    swap = int((cg / "memory.swap.max").read_text())
    events = (cg / "memory.events").read_text()
    assert 0 < peak <= limit == 64 * 1024 * 1024 and swap == 0
    assert "oom 0\n" in events and "oom_kill 0\n" in events and not info["State"]["OOMKilled"]
    return {"io_bytes": io["rbytes"] + io["wbytes"], "io_stat": devices,
            "data_device": device, "memory_peak_bytes": peak,
            "filesystem_device": filesystem_device,
            "memory_limit_bytes": limit, "swap_limit_bytes": swap,
            "data_file_bytes": stat.st_size, "memory_events": events,
            "cpu_stat": (cg / "cpu.stat").read_text()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work", type=Path, help="Toolkit folder containing candidate/tests and oracle")
    parser.add_argument("--oracle", type=Path, help="Independent prompt_oracle executable")
    parser.add_argument("--reference-image", default="minivectordb:dccb61e-backtest")
    parser.add_argument("--candidate-image", default="minivectordb:dccb-lean-local")
    parser.add_argument("--data-root", type=Path, required=True,
                        help="Private test directory on a filesystem separate from Docker image storage")
    parser.add_argument("--distribution", choices=("grid", "fp32-long", "decimal-short", "scientific"), required=True)
    parser.add_argument("--seed", type=int, default=20261117)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error("--rounds must be positive")
    support = Path(__file__).resolve().parent
    if not (support / "benchmark_local.py").exists():
        if args.work is None:
            parser.error("--work is required outside the repository tests folder")
        support = args.work / "candidate/tests"
    oracle = args.oracle or (args.work / "oracle" if args.work else None)
    if oracle is None:
        parser.error("--oracle is required without --work")
    sys.path.insert(0, str(support))
    import benchmark_local as b
    args.data_root.mkdir(parents=True, exist_ok=True)
    docker_root = Path(command("docker", "info", "--format", "{{.DockerRootDir}}").strip())
    def device_at(path):
        dev = path.stat().st_dev
        return block_device(f"{os.major(dev)}:{os.minor(dev)}")
    assert device_at(args.data_root) != device_at(docker_root), "data and image storage must use separate block devices"
    @contextmanager
    def data_volume():
        with tempfile.TemporaryDirectory(prefix="mvdb-data-", dir=args.data_root) as directory:
            name = "mvdb-isolated-" + uuid.uuid4().hex[:16]
            b.docker("volume", "create", "--driver", "local", "--opt", "type=none", "--opt", "o=bind",
                     "--opt", "device=" + directory, name)
            try:
                yield name
            finally:
                b.docker("volume", "rm", name)
    b.fresh_volume = data_volume
    b.resources = resources
    original_write, original_query = b.run_write, b.run_query
    def cold_call(function, *arguments):
        free = shutil.disk_usage(args.data_root).free
        assert free >= 96 * 1024 * 1024, ("insufficient test-host data capacity", free)
        # This dedicated VM has no unrelated workloads. The driver applies the
        # same external cold-cache initialization before every scored container.
        os.sync()
        Path("/proc/sys/vm/drop_caches").write_text("3\n")
        return function(*arguments)
    b.run_write = lambda *arguments: cold_call(original_write, *arguments)
    b.run_query = lambda *arguments: cold_call(original_query, *arguments)
    images = {"reference": args.reference_image, "candidate": args.candidate_image}
    from workload_matrix import dataset, fingerprint
    buckets, vectors, queries, inserts, statements = dataset(args.seed, args.distribution)
    packed = bytearray(struct.pack("<III", b.ROWS, b.DIMENSION, b.QUERIES))
    packed.extend(struct.pack(f"<{b.ROWS}i", *buckets))
    for vector in vectors:
        packed.extend(struct.pack(f"<{b.DIMENSION}f", *vector))
    for bucket, query in queries:
        packed.extend(struct.pack(f"<i{b.DIMENSION}f", bucket, *query))
    out = subprocess.check_output([str(oracle)], input=packed)
    truth = [[int(x) for x in line.split()] for line in out.splitlines()]
    assert len(truth) == 1000 and all(len(row) == 10 for row in truth)
    report = {"status": "running", "reference_commit": "dccb61eb98b9f6a98cee30d59aca5684d9fedc15",
              "environment": platform.platform(), "machine": platform.machine(),
              "scope": "Local paired measurements against platform-best dccb61e; E is not an official performance score.",
              "measurement": "Independent containers/volumes; 2 CPUs, 64 MiB, zero swap. "
              "Guest data-device cgroup v2 I/O and whole-container memory peak from startup through "
              "load/index/eight warmup queries/measurement/writeback; no in-container probe. "
              "Fresh private bind-backed volumes, sync and global drop_caches=3 before each scored container, identical warmup. "
              "Data and Docker image storage are on different filesystems/block devices; only data-device I/O enters B. "
              "Guest virtual-block I/O is not host SSD traffic.",
              "distribution": args.distribution, "seed": args.seed,
              "data_device": device_at(args.data_root), "image_device": device_at(docker_root),
              "dataset_sha256": hashlib.sha256(packed).hexdigest(),
              "sql_text_sha256": fingerprint(inserts, statements),
              "official_baseline_available": False,
              "local_E_is_platform_score": False,
              "rows": 10000, "dimension": 128, "queries": 1000, "top_k": 10,
              "images": {k: command("docker", "inspect", image, "--format", "{{.Id}}").strip()
                         for k, image in images.items()}, "rounds": []}
    for turn in range(args.rounds):
        result = {"write": {}, "query": {}}
        order = list(images)
        if turn % 2:
            order.reverse()
        for label in order:
            result["write"][label] = b.run_write(images[label], None, inserts, buckets, vectors)
            print(f"round {turn+1} {label} write {result['write'][label]['seconds']:.6f}", flush=True)
        for label in reversed(order):
            result["query"][label] = b.run_query(images[label], None, "exact", inserts, statements,
                                                 truth, buckets, vectors, queries, True)
            print(f"round {turn+1} {label} query {result['query'][label]['seconds']:.6f}", flush=True)
        report["rounds"].append(result)
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    baseline = {item: {key: median(r[item]["reference"][key] for r in report["rounds"])
                      for key in ("seconds", "io_bytes", "memory_peak_bytes")}
                for item in ("write", "query")}
    report["local_fixed_baseline"] = baseline
    report["summary"] = {}
    for label in images:
        values = [(b.efficiency(baseline["write"], r["write"][label]) +
                   2 * b.efficiency(baseline["query"], r["query"][label])) / 3
                  for r in report["rounds"]]
        report["summary"][label] = {item: {key: median(r[item][label][key] for r in report["rounds"])
                                               for key in ("seconds", "io_bytes", "memory_peak_bytes")}
                                          for item in ("write", "query")}
        rounded = float(Decimal(str(median(values))).quantize(Decimal("0.000001"), rounding=ROUND_HALF_UP))
        report["summary"][label].update({"local_E_rounds": values, "local_E_median": rounded})
    report["status"] = "PASS"
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report["summary"], indent=2), flush=True)


if __name__ == "__main__":
    main()
