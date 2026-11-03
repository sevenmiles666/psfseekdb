# V5 GPU paired benchmarks

Paired tuning reports: [first round](../../../docs/AMD_V5_PERFORMANCE.md),
[second round](../../../docs/AMD_V5_PERFORMANCE_2.md),
[third round](../../../docs/AMD_V5_PERFORMANCE_3.md). Each report keeps its own
source hashes and results; do not combine measurements from different builds.

These are development benchmarks, not the private official scoring harness.
Run on Linux x86-64 with AMD GPU, ROCm 7.2.4, hipVS and a GPU-enabled V5 seekdb.
Python dependencies used for validation: NumPy 2.4.6 and PyMySQL 1.1.2.
They are not dependencies of the submitted bridge or the offline build.

`benchmark_bridge.py` measures build/first query, batched GPU Top-10/100,
independent FP64 exact-L2 Recall, result completeness, snapshot reload and
sampled device VRAM/process RSS. Each round creates a new index; its first
query is timed before warmup. Three batch timings follow one warmup per k.
The first round includes process-local GPU initialization. Report the rounds
and their median rather than selecting a best run.

`benchmark_sql.py` starts an isolated observer, loads data before timing,
measures cold SQL DDL through the first legal query, checks noncontiguous ID
mapping, Top-10/100, CPU VSAG and service restart/reload. Its query loop uses
individual SQL requests; the private official query harness is not supplied.
It refuses an occupied port or an existing database directory. It creates
and rebuilds only the dedicated `perf` database in its own observer process.
The first timed query is checked for complete unique legal IDs. Reload checks
repeat the entire requested query set for Top-10 and Top-100, with a 0.005
maximum Recall loss and the development data's absolute floors. TSV inputs
are reused only when their source hash and conversion convention match.
`nq` is the number of individual SQL queries, not a CAGRA API batch size.

GPU memory is sampled only for the PCI device visible through `rocm-smi`;
the benchmark requires one visible GPU and records the device and counter path.
Older reports used the maximum over all host DRM devices, which can include
other GPUs. Those older VRAM comparisons must not be treated as the current
instance's resource evidence. The third-round call trace checks that the
counter's PCI address matches the GPU used by HIP.

The default database location is `/var/tmp/amd-v5-perf-<label>`; use a new label
for another run. The selected bridge must be named `libseekdb_cuvs_bridge.so`;
the test verifies its actual mapping in `/proc/<pid>/maps`.

Example (Linux bash; paths must be absolute):

```bash
export OPENBLAS_NUM_THREADS=8 OMP_NUM_THREADS=8
export LD_LIBRARY_PATH=/opt/hipvs/lib:/opt/rocm/lib
WORK=/absolute/path/to/benchmark-work
mkdir -p "$WORK/data" "$WORK/baseline" "$WORK/selected"
# Build each bridge from its corresponding source with the standard build.sh.
# Put the baseline/candidate DSO in baseline/selected, using its SONAME filename.
python tools/hipvs_bridge/benchmarks/benchmark_bridge.py \
  --work-dir "$WORK" --prepare gaussian256
python tools/hipvs_bridge/benchmarks/benchmark_bridge.py \
  --work-dir "$WORK" --prepare clustered256
for label in baseline selected; do
  python tools/hipvs_bridge/benchmarks/benchmark_bridge.py \
    --work-dir "$WORK" --dataset gaussian256 --label "$label" \
    --lib "$WORK/$label/libseekdb_cuvs_bridge.so" --rounds 3
done
python tools/hipvs_bridge/benchmarks/benchmark_sql.py \
  --work-dir "$WORK" --binary /absolute/path/to/seekdb \
  --bridge "$WORK/selected/libseekdb_cuvs_bridge.so" \
  --label selected-run1 --datasets gaussian256 --nq 256 --port 2902
```

Public SIFT setup: put `sift_base.fvecs` and `sift_query.fvecs` in `$WORK/data`,
then use `--prepare sift250k`. The first 250,000 base vectors and first 4,096
queries are selected; ground truth is recomputed for that subset, not reused
from the full million-vector corpus. Source: INRIA TexMex SIFT1M, also used
by [FAISS](https://github.com/facebookresearch/faiss/blob/main/demos/demo_sift1M.cpp).
The validation downloaded a public [SIFT1M mirror](https://huggingface.co/datasets/hhy3/ann-datasets/tree/main/sift1M):

| File | SHA-256 |
| --- | --- |
| Original 516,000,000-byte base | `21f66e2975057b5728ba56de1c825bac4f4d89d596609ae985741c6242631816` |
| First 250K base, 129,000,000 bytes | `fab6b3f6c68d8bca09c72b0ee84a8126b80aebc635765fb52c0ab3efbda51960` |
| Query fvecs | `f7fc9be140accdfd64116c2fa2365ecdb69b8f084970c6b0532db5ff79ac8fdc` |

No data download occurs during compilation or evaluation. Public SIFT and
seeded Gaussian/clustered 256-dimensional samples do not reproduce Hidden.
The Gaussian samples use seed 20261102. Clustered samples use 1,024 normally
distributed centers plus Gaussian noise of standard deviation 0.5.

`--prepare sift10k` now uses the first 10,000 public SIFT base vectors and first
100 public queries from the same `.fvecs` inputs. It does not use the course
image's embedded `.f32` files as proof of SIFT provenance.

The initial experiment demanded byte-identical IDs after persistence. Frozen
V5 itself does not satisfy that condition on SIFT: equal-distance ties and
approximate search can alter returned ordering/sets. The retained benchmark
therefore checks valid unique results and reports before/after Recall and
set agreement across the entire query batch, requiring Recall loss <=0.005.
Both SQL and bridge restart checks cover the full supplied query set.

For a complete SQL query-set development comparison, run each version with
`--nq 4096`, then compare the resulting JSON records:

```bash
python tools/hipvs_bridge/benchmarks/compare_sql4096.py \
  --baseline "$WORK/evidence/sql-baseline.json" \
  --candidate "$WORK/evidence/sql-candidate.json" --output "$WORK/sql-comparison.json"
```

This verifies paired data hashes, per-group Recall, per-dataset SQL QPS,
sampled resources and recorded execution checks. It labels the weighted score
as a development proxy, since generated 256D data is not official Hidden and
the private harness is not supplied. C ABI batch QPS is reported separately;
it must not be substituted for the measured end-to-end SQL query rate.

The historical first/second-round developer summary remains available below.

After both pairs finish, summarize their records with:

```bash
python tools/hipvs_bridge/benchmarks/compare_results.py \
  --evidence "$WORK/evidence" --baseline final-baseline \
  --candidate final-selected --output "$WORK/paired-summary.json"
```

The comparator uses the best baseline and worst candidate Recall across
rounds, applies per-group floors and the 0.005 gap, checks each dataset's
geometric QPS ratio >=0.90 and sampled resource ceilings. The Hidden-type
absolute floors are applied to generated 256-dimensional data only as a
development filter. It does not predict the private Hidden dataset's result.
