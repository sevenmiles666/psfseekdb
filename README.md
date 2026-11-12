# MiniVectorDB

面向 OceanBase `2026-competition-round2` 的独立 C++20 实现。存储、SQL 执行、主键树、向量检索与事务自行实现；只依赖标准库、POSIX 文件接口和锁定的 nlohmann/json 3.11.3。官方题包基线为 `f4d4e26653af823a7601668802f80e374ad6643c`。

## 构建和运行

Linux x86_64 需要 GCC 13 或兼容的 C++20 编译器；本地进程可直接构建：

```sh
sh build.sh
./build/minivectordb --data-dir ./local-data
```

macOS 脚本使用 Command Line Tools 的 clang 和 SDK，也可用 `CXX` 指定编译器。程序先恢复数据并输出 `{"type":"ready"}`，再按 [STDIO_PROTOCOL.md](STDIO_PROTOCOL.md) 接收 JSONL；stdout 仅用于协议。

```json
{"id":1,"op":"execute","sql":"CREATE TABLE docs (id BIGINT PRIMARY KEY, content VARCHAR(64), embedding VECTOR(2));","search_mode":"exact"}
{"id":2,"op":"execute","sql":"INSERT INTO docs VALUES (1,'你好','[1,0]');","search_mode":"exact"}
{"id":3,"op":"execute","sql":"SELECT id FROM docs ORDER BY L2_DISTANCE(embedding,'[0,0]') LIMIT 10;","search_mode":"exact"}
{"id":4,"op":"shutdown"}
```

构建正式入口镜像，默认启动参数为 `--data-dir /data`：

```sh
docker build --network=none -t minivectordb:local .
docker run --rm -i --network=none --read-only --cap-drop=ALL \
  --security-opt=no-new-privileges --cpus=2 --memory=2g --memory-swap=2g \
  --pids-limit=128 -v "$PWD/local-data:/data" minivectordb:local
```

Dockerfile 锁定 GCC 13.3.0 和 Debian bookworm-slim 的镜像摘要，构建不下载依赖。首次构建仍需拉取两个基础镜像。网络需要镜像站时，可通过 `MVDB_BUILD_BASE`、`MVDB_RUNTIME_BASE` 替换仓库地址并保留相同摘要。

## 已实现能力

- `CREATE TABLE`、多行 `INSERT`、`SELECT`、`UPDATE`、`DELETE`，列映射、投影、别名、AND 比较、多列排序和 LIMIT。
- BIGINT 保留完整 int64 精度；VARCHAR 按 Unicode 字符数校验、按码点排序；向量按有限 float32 存储。
- `BEGIN`、`COMMIT`、`ROLLBACK`；事务读自己的修改；失败语句整体撤销，保留同一事务中此前成功的语句。
- 磁盘 B+ 树、写时复制、双槽提交根、提交同步与异常退出恢复；自动压实废弃记录和页面。
- `L2_DISTANCE`、过滤后精确 TopK、主键处理并列；`CREATE VECTOR INDEX` 元数据持久化，向量缓存随修改失效并在恢复时重建。

`exact` 和 `ann` 当前都返回精确结果。向量索引采用有上限的连续向量缓存和穷举距离计算，超出预算后扫描磁盘；尚未实现近似候选检索算法。无 LIMIT 查询的结果受协议 16 MiB 限制，超过时返回 `response_too_large`，不实现外部排序。详见 [设计与限制](docs/design.md)。

## 验证

使用原版官方驱动和公开用例，输出目录必须不存在：

```sh
python3 protocol_check/run.py --image minivectordb:local \
  --output protocol-results/docker-public
python3 tests/test_engine.py --binary build/minivectordb
python3 tests/resource_probe.py --image minivectordb:local \
  --output protocol-results/resource-256m
python3 tests/topk_resource_probe.py --image minivectordb:local \
  --output protocol-results/topk-resource
python3 tests/benchmark.py --binary build/minivectordb \
  --output protocol-results/benchmark
```

资源测试默认在 256 MiB、禁 swap 的容器中写入约 375 MiB 文本，检查索引回退、查询和重启，并读取 cgroup v2 内存与 I/O 数据；需要有 Docker 和 cgroup 读取权限的 Linux 主机。使用正式内存预算检查更大数据：

```sh
python3 tests/resource_probe.py --image minivectordb:local --memory=2g \
  --rows=70000 --output protocol-results/resource-2g
python3 tests/verify_large_reopen.py --image minivectordb:local \
  --data protocol-results/resource-2g/data --output protocol-results/resource-reopen.json
```

独立回归覆盖 20,000 个乱序主键、750 步随机事务对照、float32 真值、提交和压实故障点及重复恢复，并包含索引前后舍入并列、大字段 TopK、超过排序键缓存及转义边界回归。故障注入仅在设置 `MVDB_CRASH_AT` 时启用，正常运行无需设置环境变量。

同机四轮大文本写入对照（B-C-C-B），每轮完成后重启核对：

```sh
python3 tests/benchmark_text.py --baseline /path/to/baseline-binary \
  --candidate build/minivectordb --output protocol-results/text-comparison
```

本地结果与环境限制保存在 [验收记录](docs/validation.md)。公开用例通过不等于正式 29 个用例通过；本地基准不代表比赛得分。学习材料保存在 `agent/conversation.json`，根目录提供相同副本兼容题面路径差异，仅记录真实交互和实验。

## 依赖

`third_party/json.hpp` 为 nlohmann/json 3.11.3，SHA-256：
`9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6`。
许可证见 `third_party/LICENSE.nlohmann-json`。没有调用现成数据库或向量检索库。
