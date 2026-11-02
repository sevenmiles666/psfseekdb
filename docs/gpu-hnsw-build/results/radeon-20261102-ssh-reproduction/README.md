# Fresh SSH instance reproduction, 2026-11-02

Product source: `f3cfe8b1738c2ce0d71a7cd04e8c2c69f6f938c1`. This evidence does not change the algorithm or build scripts. The new instance's direct GitHub clone failed through its inherited platform proxy; an independent host shallow clone of the same commit was uploaded over SSH to continue the build.

The **real contest compile entry completed**, including dependency initialization, GPU configuration and full C++/Rust compilation, in 1176 seconds, exit 0. All 9,512 tracked source hashes remained unchanged and were independently checked against the host clone. Official Rust 1.98.1 archives and 123 checksum-verified Cargo packages were supplied as offline caches; all 29 pinned RPMs were downloaded and extracted by the actual dependency initializer. 48 Linux regression tests passed, without skips or OOM.

Real CPack server/libs TGZs were extracted, checked with ldd and run without LD_LIBRARY_PATH/LD_PRELOAD or system install hooks. 27 SQL checks, six fresh index builds, DML/filtering and hidden-GPU restart all passed. CPU/GPU median construction: 25.291844788 / 2.998626784 seconds, speedup 8.434475715. GPU Recall@10: 1.000 / 0.999 / 0.998; restart: 0.999.

`final-summary.json` records the distinction between the failed original GitHub transport and successful build/package/SQL acceptance. `github-endpoint-checks.json` documents API HTTP 200 versus Git/raw proxy CONNECT timeouts. These are observations on this Radeon instance, not proof of the official judge's error-code meaning.

`full-evidence.tgz` includes original build/init/configuration logs, private dependency manifests, environment observations and replay drivers. Standalone text logs normalize trailing whitespace; their original bytes remain in the archive. `gpu-events-live.log` preserves four observed build events, not a profiler trace. Source and binary/package hashes are recorded in the JSON files.

The database was stopped after acceptance. Native packages are retained in the local delivery directory and are not committed to Git. RPM/DEB install hooks, Bazel GPU and a completely cold network Rust install were not revalidated. Official judging remains unresolved.
