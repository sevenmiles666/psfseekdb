# Native AI COLUMN validation

The integrated implementation at 90bb798b80324fafce7baf1a1571d7c0fc37bac8 was compiled on Linux x86_64 with the repository's release build and bundled Clang 17 toolchain. The resulting seekdb binary has SHA-256 `46e9c55ce49a4b4a635bb68cbe88b5f42da6586c0c2d0f4b4340e740c3582b07`.

All five supplied AI COLUMN mysqltests passed against an independent managed instance with 4 GiB memory and four configured CPUs:

* `ai_column_ddl`
* `ai_column_t1_dml`
* `ai_column_worker`
* `ai_column_retry_strict`
* `ai_column_vector_index`

The test sources and expected result files were unchanged. Run mysqltest from `tools/deploy`, because the official tests start their mock servers using paths relative to that directory. For example, after starting seekdb on port 2882 and creating the `test` database:

```sh
cd tools/deploy
for name in ai_column_ddl ai_column_t1_dml ai_column_worker ai_column_retry_strict ai_column_vector_index; do
  ../../deps/3rd/u01/obclient/bin/mysqltest \
    --host=127.0.0.1 --port=2882 --user=root --database=test \
    --test-file="mysql_test/test_suite/ai_column/t/$name.test" \
    --result-file="mysql_test/test_suite/ai_column/r/mysql/$name.result" \
    --logdir=/tmp --tmpdir=/tmp || exit 1
done
```

The same binary also passed the existing AI function regression, response-root, endpoint lifecycle and model matrix checks, both supplied AI function mysqltests, and the File SQL public test shape with complete CSV, JSONL and Parquet rows, DESCRIBE output, glob input and COPY readback assertions. The File SQL enablement variable remains disabled by default.

These results establish the supplied cases and integration regressions. They are local validation evidence, not a platform score or a claim of coverage beyond the cases executed. Public source attribution is in `AI_COLUMN_SOURCES.md`.
