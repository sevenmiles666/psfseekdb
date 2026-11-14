# MiniVectorDB

基于官方 `2026-competition-round2` 分支开发的轻量向量数据库，提供 C++17 引擎和 Linux x86_64 容器。Docker 默认使用官方 JSONL 协议。

## 构建和运行

Dockerfile 锁定 GCC 13.4.0 构建镜像的摘要，从源码编译静态可执行文件。引擎使用 C++ 标准库和 Linux 文件接口。容器以前台方式处理标准输入，数据目录为 `/data`。

```bash
docker build --platform linux/amd64 -t minivectordb-round2:dev .
docker run --rm -i --network=none -v minivectordb-demo:/data minivectordb-round2:dev
```

容器恢复数据后先输出 `{"type":"ready"}`。使用空数据卷时，可逐行发送以下请求。

```jsonl
{"id":1,"op":"execute","sql":"CREATE TABLE items (id BIGINT PRIMARY KEY, label VARCHAR(12), v VECTOR(2));"}
{"id":2,"op":"execute","sql":"INSERT INTO items VALUES (1,'near',[1,0]),(2,'keep',[2,0]);"}
{"id":3,"op":"execute","sql":"CREATE VECTOR INDEX idx_items ON items(v);"}
{"id":4,"op":"execute","sql":"SELECT id,L2_DISTANCE(v,[0,0]) AS distance FROM items WHERE label='keep' ORDER BY distance LIMIT 2;","search_mode":"exact"}
{"id":5,"op":"shutdown"}
```

每个请求返回一行 JSON 并立即刷新 stdout。查询返回 `columns` 和 `rows`，修改返回 `affected_rows`。响应保留请求 ID 的 JSON 类型。收到 `shutdown` 或 EOF 后回滚未提交事务，正常退出。再次使用同一个数据卷启动时可读取已提交记录。[官方 JSONL 协议](https://github.com/oceanbase/seekdb/blob/f4d4e26653af823a7601668802f80e374ad6643c/STDIO_PROTOCOL.md)

## 实现范围

| 模块 | 行为 |
| --- | --- |
| 表和修改 | `CREATE TABLE`、多行 `INSERT`、条件 `UPDATE` 和 `DELETE`。每表一个 BIGINT 主键，支持 VARCHAR 和一个 VECTOR 列。 |
| 查询 | 普通投影、`SELECT *`、`AND`、`= < <= > >=`、`ORDER BY`、`LIMIT`。排序键相同的记录按主键升序输出。 |
| 精确 TopK | 先应用 `WHERE`，再按存储后的 float32 向量计算距离。用 double 累加平方差，输出欧氏距离。 |
| 向量索引 | 每表可建立一个向量索引。索引随增删改和回滚更新，进程恢复时在输出就绪响应前重建。 |
| 输入检查 | 检查主键重复、BIGINT 范围、UTF-8 字符数、向量维度及有限 float32。未支持的 SQL 返回错误。 |
| 事务 | 显式 `BEGIN`、`COMMIT`、`ROLLBACK` 和隐式自动提交。事务内查询使用当前可见行。一条修改失败时撤销该语句，保留事务内此前成功的语句。 |
| 恢复 | 使用提交标记确定持久化边界，重放已提交内容并清理未提交或不完整的尾部后发出就绪标识。 |

`store.wal` 保存表定义、索引定义和行修改。每条记录有长度和 CRC32，提交成功响应在写入提交标记并完成 `fdatasync` 之后发出。内存中的主键表记录每行的文件偏移，行数据按需解码。语句和事务回滚恢复偏移、更新对应索引条目并截断相应日志尾部。

向量输入按 IEEE float32 保存。整条向量能用同一个二次幂比例和有符号整数无损表示时，WAL 每个分量写一字节，另存一个比例字节。其余向量依次尝试无损 binary16、窄指数打包和原始 float32。新版可直接读取旧 WAL。含一字节编码记录的数据卷需要用支持该格式的版本打开，切回旧镜像前应使用升级前的数据卷或从 SQL 重新导入。

精确 TopK 使用有界候选堆。`ORDER BY L2_DISTANCE(...) LIMIT K` 和带主键次排序的写法可缓存多张表的向量，全部缓存共用 8 MiB 预算并按 LRU 淘汰。准入按实际候选表示估算，支持 FP32 放不下而无损字节表示可容纳的表。最多 256 KiB 廉价整数列随向量读取，WHERE 所需其他列按需缓存，其余投影列只读取最终 K 行。表修改只失效该表的缓存；观察到单次查询即失效的模式时延迟下一次重建。超预算时逐行从 WAL 读取。

等值过滤先扫描，后续依据有界抽样及已有索引的实际区间大小选择一列，累计收益足以覆盖重建和重排后才换列。较大的排序按约 2 MiB 的键预算生成有序段，最多八路合并，临时文件位于数据目录。主键表、索引条目、未提交事务的撤销记录、超长单行和单条 SQL 请求仍需内存。

WAL 按历史追加。日志达到检查阈值且历史明显多于有效行时，在事务边界外构建只含当前有效行的替代 WAL；同步新文件、原子替换并同步目录后再更新行引用。启动丢弃未完成的替代文件，原格式及提交同步语义保留。首次压缩有额外时间、写入和临时磁盘开销，后续恢复不再扫描已移除的历史。

等值查找先定位匹配行，再检查剩余 `WHERE` 条件。整张表可共用无损整数比例时，缓存直接保存整数，查询也符合比例时用整数点积和比例计算精确距离。普通 FP32 使用量化点积和误差上界筛选，再以原始向量和 double 累加核算保留的候选。维数为 32 的倍数时，AVX2 可一次处理八行。等值条件命中足够多且有缓存余量时，量化向量按筛选列原地分组，查询可以连续读取对应的候选。量化无法适用时，可用带舍入余量的浮点筛选或标量计算。ANN 复用同一缓存，超出预算时按索引从 WAL 读取。

默认使用 `exact`，有索引时仍返回精确结果。在单条请求中设置 `"search_mode":"ann"` 可切换到近似检索。32 维及以上已有缓存时，会用有界样本估计第一坐标剪枝；剪枝弱时改用精确扫描，低维保留原路径。本地六项实现与测量范围见 [优化结果](agent/optimization_results.md)，尚未提测。

ANN 索引按向量首个坐标和主键排序。从查询坐标两侧按距离下界扫描，满足过滤条件的候选才进入 TopK 堆。收满 K 条后可按 0.1% 的相对距离容差提前停止，返回项仍以完整欧氏距离排序。高维数据中单坐标下界可能较弱，此时扫描和随机读成本较高。没有索引或查询形状超出该路径时走精确检索。

`agent/conversation.json` 保留 Agent 开发指令与实际工具结果，已删除操作对话及由这些对话建立的学生评分引用。[官方赛题说明](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/lg8enxlmwg69unt6)

当前材料没有经核实的学生发言，开发指令和实验保持 Agent／工具来源。历史工具结果保留各自的版本与环境。[材料说明](agent/README.md)

## 本地验证

官方公开检查器在 Linux 或 Ubuntu WSL 中运行。从官方提交 `f4d4e26653af823a7601668802f80e374ad6643c` 取出 `mysqltest` 与 `protocol_check` 目录，使用实际提交镜像运行 20 组、95 条 SQL 的交互式检查。检查覆盖启动握手、逐请求刷新、结果字段、事务、重启和模式切换。[官方检查器说明](https://github.com/oceanbase/seekdb/tree/f4d4e26653af823a7601668802f80e374ad6643c/protocol_check)

```bash
python3 ../minivectordb-official/protocol_check/run.py \
  --image minivectordb-round2:dev --output build/official-check
```

```bash
python tests/check_core.py --image minivectordb-round2:dev --large
```

在 Linux 或 Ubuntu WSL 中也可直接编译核心并运行测试。

```bash
mkdir -p build
g++ -std=c++17 -O2 -D_FILE_OFFSET_BITS=64 -Wall -Wextra -Wpedantic -Werror \
  src/main.cpp src/request.cpp src/sql.cpp src/journal.cpp src/database.cpp -o build/minivectordb
python3 tests/check_core.py --binary build/minivectordb
```

2026-11-09 的 Linux 构建和断网容器测试通过。独立核对了 24 组 128 维精确 TopK，其中 12 组带过滤条件。测试覆盖提交后强杀、未提交尾部、语句级撤销、EOF 回滚及断帧重放。用低维近距样例分别触发精确与近似路径，并检查索引在增删改、回滚和重启后的结果。32 MiB 内存限制的容器从 52,482,621 字节的持久化数据重建索引并完成查询和排序。新版也能直接打开首版写入的数据卷并在创建索引后重启查询。

## 2026-11-14 无损向量编码与等值过滤

以 `fd9011a30d53c625ff67f19eb6838fbf407bf8b4` 为对照。两种分布各跑五轮，每轮批量写入 10,000 条 128 维向量，重启核对记录。建索引和预热后执行 1,000 条精确 Top-10 查询，其中一半带等值过滤。每轮两组 Recall@10 均为 1.0，独立参考实现也核对了 ID、距离和排序。

| 数据分布 | 写入耗时 | 精确查询耗时 | 写入物理 I/O | 查询峰值内存 |
| --- | ---: | ---: | ---: | ---: |
| 离散向量 | 0.766 → 0.746 s | 0.998 → 0.907 s | 3,268,608 → 1,994,752 B | 17,780,736 → 11,440,128 B |
| 连续 FP32 | 0.869 → 0.881 s | 1.015 → 0.962 s | 5,197,824 → 5,197,824 B | 19,996,672 → 19,918,848 B |

表中是各项中位数。两版均使用断网容器、独立数据卷、2 CPU、64 MiB 内存和零 swap。I/O 使用数据卷所在设备的容器计数，峰值包含页缓存和采集进程。正式评测使用平台基线和环境。[离散数据五轮记录](tests/round2-byte-grid.json) · [连续数据五轮记录](tests/round2-byte-continuous.json)

官方公开检查 20/20 通过。独立回归验证了四种 WAL 编码共存、旧卷升级、事务回滚、强杀恢复和 CRC 校验。32 MiB 容器完成约 50 MiB 数据的重启恢复、建索引、查询和排序。Linux ASan/UBSan 回归通过。

## 2026-11-13 存储与查询优化

以 `9eb5b15446170c0ca001e3fb0907a963a88923e4` 为对照，在每种分布下各跑五轮。每轮写入 10,000 条 128 维向量并重启核对完整内容。查询预先建索引，计时运行 1,000 条精确 Top-10，其中 500 条带等值过滤。两组的每轮 Recall@10 均为 1.0。

| 数据分布 | 写入耗时 | 精确查询耗时 | 写入物理 I/O | 查询峰值内存 |
| --- | ---: | ---: | ---: | ---: |
| 离散向量 | 0.793 → 0.743 s | 0.951 → 0.927 s | 5.56 → 3.12 MiB | 20.86 → 16.95 MiB |
| 连续 FP32 | 0.964 → 0.835 s | 0.968 → 0.961 s | 5.56 → 4.96 MiB | 20.88 → 19.27 MiB |

表中为各项中位数。两版用断网容器、独立数据卷、2 CPU、64 MiB 内存和零 swap。物理 I/O 计入同一底层设备的容器读写，峰值包含文件页缓存和采集进程。查询耗时收益小于写入、I/O 和峰值内存收益。[离散数据记录](tests/round2-compact-grid.json) · [连续数据记录](tests/round2-compact-continuous.json)

官方公开检查 20/20 通过。独立回归覆盖旧 WAL 升级、三种向量编码混用、CRC、事务回滚及强制终止恢复。32 MiB 容器能重启并检索、排序约 50 MiB 的数据。可按下节流程构建对照镜像，用 `--index-exact --rounds 5` 复跑，连续数据另加 `--vector-distribution continuous --seed 20261113`。

## 2026-11-12 精确检索优化

使用 `c16935a15dcdd5cc3803ff38746ee774e4b948b1` 作对照。两版处理同样的 10,000 条 128 维向量和 1,000 条 Top-10 查询，其中 500 条有等值过滤。Docker 对照均先建索引再查询，各运行三轮，使用断网容器、独立卷、2 CPU、64 MiB 内存和零 swap。

| 指标（中位数） | 对照 | 优化 |
| --- | ---: | ---: |
| Docker 写入耗时 | 0.879 s | 0.798 s |
| Docker 查询耗时 | 1.416 s | 0.996 s |
| Docker 查询物理 I/O | 5,832,704 B | 5,832,704 B |
| Docker 查询内存峰值 | 20,303,872 B | 21,147,648 B |

两版的有过滤和无过滤 Recall@10 均为 1.0，重启后核对了写入内容。Linux 进程的五轮查询耗时从 0.586 秒降到 0.312 秒，所有查询输出逐项一致。两版用相同操作序列生成逐字节相同的 WAL 并能相互重开。正式评测采用平台固定基线和测试环境。[Linux 五轮记录](tests/round2-race-native.json) · [Docker 三轮记录](tests/round2-race-vs-c16935a.json)

另与 `bb7c32c1a9329e0512915d5f162a81531a28e7bc` 做了三轮 Docker 对照。写入中位数从 1.736 秒降至 0.854 秒，精确查询从 2.166 秒降至 1.162 秒。该对照使用其 Dockerfile 和缓存中的 GCC 13.4.0 基础镜像构建。[另一候选的 Docker 对照](tests/round2-race-benchmark.json)

新增回归覆盖量化筛选、平方距离不同但 L2 距离舍入相等时按主键排序、量化范围外的查询和极端 float32 值。官方公开检查器 20/20 通过，32 MiB 容器重启后完成约 50 MiB 数据的建索引、检索和排序。

已有两版 Linux 二进制时，可用下列命令复跑。Docker 对照沿用下一节的流程，换用本节的对照提交并加 `--index-exact`。

```bash
python3 tests/benchmark_native.py --reference ../minivectordb-reference/build/minivectordb \
  --candidate build/minivectordb --rounds 5 --output build/native-comparison.json
```

## 2026-11-12 JSONL 初版性能对照

两版均采用官方 JSONL。对照版本为 `001819de55ae299e0cc74be306bea5f8438cae6b`。使用相同的 10,000 条 128 维向量和 1,000 条 Top-10 查询，其中 500 条带等值过滤。每轮使用独立 Docker 数据卷、2 CPU、64 MiB 内存并关闭 swap。写入以每批 100 条自动提交，重启后核对全部记录。查询结果用独立的暴力计算核对。

| 版本和模式 | 写入中位数（秒） | 查询中位数（秒） | 查询 I/O / 峰值内存（MiB） | 带过滤 / 无过滤 Recall@10 |
| --- | ---: | ---: | ---: | ---: |
| JSONL 对照 exact | 1.297 | 2.359 | 5.559 / 20.125 | 100% / 100% |
| 优化版 exact | 1.225 | 2.107 | 5.559 / 20.598 | 100% / 100% |
| 优化版 ann | 1.225 | 4.164 | 5.562 / 21.121 | 100% / 100% |

在这批数据上，默认 `exact` 的查询耗时降低 10.7%，写入耗时降低 5.6%。两组查询都核对了结果 ID、距离及排序。本地数据卷与镜像存储共用底层设备，内存峰值包含页缓存与最终探针。正式性能指标和名次以平台评测结果为准。[三轮原始记录](tests/round2-jsonl-benchmark.json)

在 Linux x86_64 或 Ubuntu WSL 编译测试工具，使用实际构建的镜像和同样的测试参数复跑。

```bash
git worktree add --detach ../minivectordb-jsonl-reference 001819de55ae299e0cc74be306bea5f8438cae6b
docker build --platform linux/amd64 -t minivectordb-round2:jsonl-base ../minivectordb-jsonl-reference
mkdir -p build
g++ -std=c++17 -O3 -static tests/benchmark_support.cpp -o build/benchmark_support
python3 tests/benchmark_local.py --baseline-image minivectordb-round2:jsonl-base \
  --candidate-image minivectordb-round2:dev --probe build/benchmark_support --rounds 3 \
  --output build/round2-jsonl-benchmark.json
```

## 2026-11-09 本地性能记录

使用固定种子生成 10,000 条 128 维向量及 1,000 条 Top-10 查询，其中 500 条带等值过滤。首版 `590ff199321bc592cd4f3b0260645a70d995fbb3` 为本地对照。两版各跑三轮写入，三种查询模式各跑三轮，使用独立容器和数据卷，限制 2 个 CPU、64 MiB 内存并关闭 swap。写入以每批 100 条自动提交，计时后重启并核对全部行。查询在准备和预热后计时，以独立的暴力计算验证 ID、距离、排序和召回。

| 版本和模式 | 写入秒数 | 写入 I/O / 峰值内存（MiB） | 查询秒数 | 查询 I/O / 峰值内存（MiB） | 带过滤 / 无过滤 Recall@10 | 本地 E |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 首版 exact | 1.242 | 5.559 / 13.629 | 7.638 | 5.559 / 13.910 | 100% / 100% | 0.497615 |
| 新版 exact | 1.227 | 5.559 / 14.336 | 2.278 | 5.559 / 20.129 | 100% / 100% | 0.595402 |
| 新版 ann | 1.227 | 5.559 / 14.336 | 30.519 | 5.562 / 14.664 | 100% / 100% | 0.377570 |

表中 T、B、M 取三轮中位数，E 按首版各项中位数计算每轮后再取中位数。新版精确查询约快 3.35 倍，代价是更高的峰值内存。此数据上保持 `exact` 为默认。Docker Desktop 的数据卷与镜像共用底层设备，`io.stat` 记录容器生命周期内该设备的读写计数，`memory.peak` 包含页缓存及最终采集进程。本地未清理宿主缓存。各轮计数、环境及镜像 ID 见[原始记录](tests/round2-benchmark.json)。正式限额、基线和成绩由官方评测确定。

在 Linux x86_64 或 Ubuntu WSL 编译测试工具。Docker 客户端可从宿主机运行，`--probe` 指向编译所得的 Linux ELF 文件。

```bash
git worktree add --detach ../minivectordb-baseline 590ff199321bc592cd4f3b0260645a70d995fbb3
docker build --platform linux/amd64 -t minivectordb-round2:baseline-590ff19 ../minivectordb-baseline
mkdir -p build
g++ -std=c++17 -O3 -static tests/benchmark_support.cpp -o build/benchmark_support
python tests/benchmark_local.py --probe build/benchmark_support --rounds 3 \
  --output build/round2-benchmark.json
```
