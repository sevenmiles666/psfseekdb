# Comprehensive v3 local validation

This is the unchanged comprehensive v3 candidate measured on 2026-11-17 against `dccb61eb98b9f6a98cee30d59aca5684d9fedc15`. Engine source and Dockerfile hashes match the measured snapshot. Prompt materials and the protocol specification are unchanged.

Three complete paired rounds per distribution used 10,000 rows, 128 dimensions, 1,000 exact TopK queries, K=10, half with equality filters, and durable batches of 100 rows. Unlike the historical decimal workload, `decimal-short` preserves the five-decimal SQL input separately from FP32 truth. JSON negative zero is preserved only in the untimed post-restart verifier; the timed adapter remains unchanged.

| Input | Write time | Query time | Write I/O | Query I/O | Write peak memory | Query peak memory | Local E: reference → v3 |
|---|---:|---:|---:|---:|---:|---:|---:|
| Grid | -1.12% | -11.34% | -37.99% | -37.91% | -17.47% | -21.27% | 0.500160 → 0.536615 |
| Long FP32 text | -6.89% | +0.84% | -13.16% | -13.15% | -9.93% | -38.18% | 0.497952 → 0.526374 |
| Five-decimal SQL text | -23.97% | -2.63% | -28.92% | -28.90% | -23.70% | -46.03% | 0.497690 → 0.556433 |

All write/query correctness checks passed: 18 durable write projects and 18 query projects, 18,000 checked query responses, both Recall@10 groups equal to 1. The negative-zero restart regression passed on both images. Existing public MySQL 20/20, 185 protocol differential requests, core regression and resource-limit checks are reused from the same verified v3 binary, not represented as newly executed tests.

This is a promising experimental candidate, not an established improvement in every metric: long-FP32 query median is 0.84% higher, within the observed timing variation. Every round is retained. Local E uses the fast competitor dccb61e as reference, not the official fixed full-scan baseline, and cannot predict platform points. Historical `tests/round2-comprehensive-backtest.json` remains as a separate earlier run; the current raw rounds are in `results/`.

Measurements ran on Linux x86_64 under UTM/QEMU, GCC 13.3, two CPUs, 64 MiB per container, no swap. Private data volumes and image storage were on separate virtual devices; B/M include startup, loading, index preparation, eight warmup queries and completed writeback. T is end-to-end single-client time. This does not establish native x86 performance or validate the pinned production GCC 13.4 image. No platform evaluation was requested.

To reproduce on a dedicated Linux host, build the two images with the same compiler, compile `tests/prompt_oracle.cpp` as an independent oracle, enable cgroup v2 I/O accounting, and place the data directory on a different device from Docker image storage. Repeat this command for `grid`, `fp32-long`, and `decimal-short`:

```bash
sudo python3 tests/comprehensive_v3/benchmark_matrix.py \
  --reference-image minivectordb:dccb61e-backtest \
  --candidate-image minivectordb:comprehensive-local \
  --oracle /absolute/path/to/prompt-oracle \
  --data-root /measured-device/benchmark-data \
  --distribution decimal-short --seed 20261117 --rounds 3 \
  --output /absolute/path/to/decimal-short.json
```

The driver explicitly resets host caches, so use an isolated benchmark machine. The recorded source hashes, binary hash and image IDs are in `results/source-provenance.json` and `results/execution-audit.json`. Flags such as `github_pushed: false` inside archived records describe the measurement time, before publication.
