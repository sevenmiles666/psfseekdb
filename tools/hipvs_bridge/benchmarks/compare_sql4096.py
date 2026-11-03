"""Compare two complete SQL query-set runs; not the private official harness.

nq is the number of SQL queries in each set. This tool makes no assumption
that the implementation sends all query vectors to one CAGRA API call.
The 256-dimensional generated dataset is a development proxy for Hidden.
Only the gates observable in the input records are assessed here.
"""
from __future__ import annotations

import argparse
import copy
import json
import math
from pathlib import Path
import sys


SCOPE = (
    "Paired complete SQL query-set development comparison using public SIFT "
    "and a 256D development proxy. The proxy is not official Hidden; this "
    "result and proxy_score are not an official score or full-score proof. "
    "nq counts SQL queries and is not assumed to be CAGRA API batch size."
)
MAX_VRAM = 45_000_000_000
MAX_RSS = 96_000_000_000


def number(value, field, *, positive=False, unit_interval=False):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{field} must be a real JSON number")
    result = float(value)
    if not math.isfinite(result) or (positive and result <= 0):
        raise ValueError(f"{field} must be finite" + (" and positive" if positive else ""))
    if unit_interval and not 0 <= result <= 1:
        raise ValueError(f"{field} must be between 0 and 1")
    return result


def integer(value, field):
    if isinstance(value, bool) or not isinstance(value, int) or value < 1:
        raise ValueError(f"{field} must be a positive integer")
    return value


def dataset_map(run, side):
    values = run.get("datasets")
    if not isinstance(values, list) or len(values) != 2:
        raise ValueError(f"{side} must contain exactly two formal-scale datasets")
    result = {}
    for row in values:
        name = row.get("name")
        if not isinstance(name, str) or not name or name in result:
            raise ValueError(f"{side} has an invalid or duplicate dataset name")
        rows = integer(row.get("rows"), f"{side}.{name}.rows")
        dim = integer(row.get("dim"), f"{side}.{name}.dim")
        nq = integer(row.get("nq"), f"{side}.{name}.nq")
        if rows != 250_000:
            raise ValueError(f"{side}.{name}: formal-scale comparison requires 250000 rows")
        if nq < 4096:
            raise ValueError(f"{side}.{name}: nq must be at least 4096 SQL queries")
        if dim not in (128, 256):
            raise ValueError(f"{side}.{name}: expected 128 or 256 dimensions")
        result[name] = row
    if sorted(row["dim"] for row in result.values()) != [128, 256]:
        raise ValueError(f"{side} must have one 128D SIFT dataset and one 256D proxy")
    return result


def compare(baseline, candidate):
    a = dataset_map(baseline, "baseline")
    b = dataset_map(candidate, "candidate")
    if set(a) != set(b):
        raise ValueError("Dataset names differ between baseline and candidate")

    rows = []
    all_quality = True
    all_queries = True
    weighted_log_b = weighted_log_q = 0.0
    for name in sorted(a, key=lambda name: a[name]["dim"]):
        x, y = a[name], b[name]
        for field in ("rows", "dim", "nq"):
            if x[field] != y[field]:
                raise ValueError(f"{name}: paired {field} differs")
        hashes=x.get("dataset_sha256")
        if not isinstance(hashes,dict) or set(hashes)!={"base.npy","queries.npy","truth.npy"}:
            raise ValueError(f"{name}: missing base/query/truth hashes")
        if hashes != y.get("dataset_sha256"):
            raise ValueError(f"{name}: paired dataset hashes differ")
        is_sift = x["dim"] == 128
        weight = 0.4 if is_sift else 0.6
        # A 128D input must really be public SIFT, not an arbitrary substitute.
        if is_sift and "sift" not in name.lower():
            raise ValueError("128D dataset name must identify public SIFT")
        x_build = number(x.get("ddl_plus_first_query_seconds"), f"baseline.{name}.cold_seconds", positive=True)
        y_build = number(y.get("ddl_plus_first_query_seconds"), f"candidate.{name}.cold_seconds", positive=True)
        build_ratio = x_build / y_build
        checks = {}
        log_query_ratios = []
        for k in ("10", "100"):
            try:
                xp, yp = x["topk"][k], y["topk"][k]
            except (KeyError, TypeError) as error:
                raise ValueError(f"{name}: missing Top-{k} record") from error
            xr = number(xp.get("recall"), f"baseline.{name}.recall{k}", unit_interval=True)
            yr = number(yp.get("recall"), f"candidate.{name}.recall{k}", unit_interval=True)
            floor = .95 if is_sift else (.90 if k == "10" else .85)
            threshold = max(floor, xr - .005)
            quality_ok = yr >= threshold
            xq = number(xp.get("qps"), f"baseline.{name}.qps{k}", positive=True)
            yq = number(yp.get("qps"), f"candidate.{name}.qps{k}", positive=True)
            ratio = yq / xq
            log_query_ratios.append(math.log(ratio))
            checks[k] = {
                "baseline_recall": xr,
                "candidate_recall": yr,
                "absolute_floor": floor,
                "relative_floor": xr - .005,
                "threshold": threshold,
                "margin": yr - threshold,
                "quality_passed": quality_ok,
                "baseline_sql_qps": xq,
                "candidate_sql_qps": yq,
                "qps_ratio": ratio,
            }
            all_quality = all_quality and quality_ok
        query_ratio = math.exp(sum(log_query_ratios) / 2)
        query_ok = query_ratio >= .90
        all_queries = all_queries and query_ok
        weighted_log_b += weight * math.log(build_ratio)
        weighted_log_q += weight * math.log(query_ratio)
        rows.append({
            "name": name, "role": "public_sift" if is_sift else "development_proxy_not_hidden",
            "rows": x["rows"], "dim": x["dim"], "sql_query_count": x["nq"], "weight": weight,
            "baseline_cold_seconds": x_build, "candidate_cold_seconds": y_build,
            "build_ratio": build_ratio, "topk": checks,
            "query_geometric_ratio": query_ratio, "query_gate_passed": query_ok,
        })

    xv = number(baseline.get("peak_vram_device_bytes"), "baseline.peak_vram_device_bytes", positive=True)
    yv = number(candidate.get("peak_vram_device_bytes"), "candidate.peak_vram_device_bytes", positive=True)
    yrss = number(candidate.get("peak_seekdb_rss_bytes"), "candidate.peak_seekdb_rss_bytes", positive=True)
    resources = {
        "baseline_peak_vram_bytes": xv, "candidate_peak_vram_bytes": yv,
        "vram_ratio": yv / xv, "candidate_peak_rss_bytes": yrss,
        "vram_absolute_passed": yv <= MAX_VRAM,
        "vram_relative_passed": yv / xv <= 1.25,
        "rss_passed": yrss <= MAX_RSS,
        "sampling_caveat": "Input sampled peaks do not establish the official resource peak.",
    }
    resources_ok = all(resources[key] for key in ("vram_absolute_passed", "vram_relative_passed", "rss_passed"))
    execution = {}
    for side, run in (("baseline", baseline), ("candidate", candidate)):
        for key in ("completed", "loaded_bridge_verified", "cpu_vsag_passed", "reload_validated"):
            execution[f"{side}_{key}"] = run.get(key) is True
    execution_ok = all(execution.values())
    duration = candidate.get("gpu_evaluation_seconds")
    base_duration = baseline.get("gpu_evaluation_seconds")
    duration_checked = duration is not None and base_duration is not None
    duration_ok = True
    total_duration = None
    if duration_checked:
        total_duration = (number(duration, "candidate.gpu_evaluation_seconds", positive=True) +
                          number(base_duration, "baseline.gpu_evaluation_seconds", positive=True))
        duration_ok = total_duration <= 900
    passed = all_quality and all_queries and resources_ok and execution_ok and duration_ok
    B, Q = math.exp(weighted_log_b), math.exp(weighted_log_q)
    clamp = lambda value: min(1.0, max(0.0, value))
    build_points = 14 * clamp(weighted_log_b / math.log(3.0))
    query_points = 2 * clamp(weighted_log_q / math.log(1.5))
    score = 4 + build_points + query_points if passed else 0.0
    return {
        "scope": SCOPE, "input_validated": True,
        "baseline_label": baseline.get("label"), "candidate_label": candidate.get("label"),
        "baseline_bridge_sha256": baseline.get("bridge_sha256"),
        "candidate_bridge_sha256": candidate.get("bridge_sha256"),
        "datasets": rows, "resources": resources, "execution_checks": execution,
        "gates": {"recall_passed": all_quality, "per_dataset_query_regression_passed": all_queries,
                  "sampled_resources_passed": resources_ok, "recorded_execution_checks_passed": execution_ok,
                  "duration_checked": duration_checked, "duration_passed_if_checked": duration_ok},
        "passed": passed, "paired_evaluation_seconds": total_duration,
        "combined_build_ratio": B, "combined_query_ratio": Q,
        "proxy_points": {"hard_gates": 4.0 if passed else 0.0,
                         "build": build_points if passed else 0.0,
                         "query": query_points if passed else 0.0},
        "proxy_score": score,
        "measured_proxy_full_score_thresholds_met": passed and B >= 3.0 and Q >= 1.5,
        "official_score": None, "all_official_gates_verified": False,
        "unverified": [
            "Official Hidden data and its paired frozen V5 baseline",
            "Exact private harness SQL, warmup, timer and repetition policy",
            "GPU-served queries, which need the separate API-call trace",
            "All returned IDs and distances, which must be checked by the benchmark",
            *([] if duration_checked else ["15-minute total GPU evaluation time"]),
        ],
    }


def self_test():
    base = {
        "label": "synthetic arithmetic fixture", "completed": True,
        "loaded_bridge_verified": True, "cpu_vsag_passed": True, "reload_validated": True,
        "peak_vram_device_bytes": 1_000_000_000, "peak_seekdb_rss_bytes": 2_000_000_000,
        "datasets": [],
    }
    for name, dim in (("sift250k", 128), ("gaussian256", 256)):
        base["datasets"].append({"name": name, "rows": 250000, "dim": dim, "nq": 4096,
                                 "ddl_plus_first_query_seconds": 30,
                                 "dataset_sha256":{key:letter*64 for key,letter in (("base.npy","a"),("queries.npy","b"),("truth.npy","c"))},
                                 "topk": {k: {"recall": .99, "qps": 100} for k in ("10", "100")}})
    candidate = copy.deepcopy(base)
    for row in candidate["datasets"]:
        row["ddl_plus_first_query_seconds"] = 10
        for top in row["topk"].values():top["qps"] = 150
    result = compare(base, candidate)
    assert result["passed"] and math.isclose(result["proxy_score"], 20, abs_tol=1e-12)
    assert math.isclose(result["combined_build_ratio"], 3)
    assert math.isclose(result["combined_query_ratio"], 1.5)
    bad = copy.deepcopy(candidate)
    bad["datasets"][1]["topk"]["100"]["recall"] = .984
    assert not compare(base, bad)["passed"] and compare(base, bad)["proxy_score"] == 0
    bad = copy.deepcopy(candidate)
    for top in bad["datasets"][0]["topk"].values():top["qps"] = 85
    for top in bad["datasets"][1]["topk"].values():top["qps"] = 300
    assert not compare(base, bad)["passed"], "Other dataset must not cancel query regression"
    bad = copy.deepcopy(candidate);bad["peak_vram_device_bytes"] = 1_250_000_001
    assert not compare(base, bad)["passed"]
    bad = copy.deepcopy(candidate);bad["datasets"][0]["nq"] = 256
    try:compare(base, bad)
    except ValueError:pass
    else:raise AssertionError("nq=256 must not be accepted as full query set")
    bad = copy.deepcopy(candidate);bad["datasets"][1]["dim"] = 128
    try:compare(base, bad)
    except ValueError:pass
    else:raise AssertionError("Mismatching paired dimensions must be rejected")
    print("6 SQL comparison arithmetic/gate checks passed; fixtures are not measurements.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--candidate", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test();return 0
    if not all((args.baseline, args.candidate, args.output)):
        parser.error("--baseline, --candidate and --output are required")
    try:
        result = compare(json.loads(args.baseline.read_text(encoding="utf-8")),
                         json.loads(args.candidate.read_text(encoding="utf-8")))
    except (ValueError, KeyError, TypeError) as error:
        args.output.write_text(json.dumps({"scope": SCOPE, "input_validated": False,
                                         "passed": False, "error": str(error)}, indent=2), encoding="utf-8")
        print(f"Invalid/incomplete comparison inputs: {error}", file=sys.stderr)
        return 2
    args.output.write_text(json.dumps(result, indent=2, allow_nan=False), encoding="utf-8")
    print(json.dumps(result, indent=2, allow_nan=False))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
