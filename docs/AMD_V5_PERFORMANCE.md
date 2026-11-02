# AMD V5 性能优化与配对验证

这是第一轮优化记录；后续 `competition/amd-v5-perf2` 的参数、配对结果和
原始证据见 [第二轮性能优化](AMD_V5_PERFORMANCE_2.md)。本页保留原测量，
不将不同版本的结果混用。

优化分支：`competition/amd-v5-perf`，从 `df1c8f48785294c120755c28da163249c43fd21d`
建立，仍包含冻结 V5 `634888d9b037153fa143d2c913ba4b25503e1d62`。
原 `competition/amd-v5-fix` 分支保留，便于回退。

## 改动

仅修改 `tools/hipvs_bridge/seekdb_cuvs_bridge.c` 的两项 CAGRA 参数：

| 参数 | 冻结 V5 | 本版 |
| --- | ---: | ---: |
| NN-descent 最大迭代数 | 100 | 80 |
| CAGRA search_width | 4 | 16 |

减少图细化的工作量，并扩大每轮搜索前沿以补偿质量、减少查询轮数。
graph_degree=128、intermediate_graph_degree=256、itopk>=512 均保持原值；
仍采用 FP32 数据、真实 GPU 建图和固定 hipVS/cuVS CAGRA 搜索。
未修改 ABI、快照格式、seekdb C++、CPU VSAG、构建入口或编译开关，
不根据评测数据身份、顺序或计时阶段切换策略。

20/32/50 轮配原搜索参数会损失过多 Recall，未采用。增大图度数的部分组合
虽然召回更高，但查询回归超过或接近 90% 门槛，也未采用。部分更大的 itopk
组合在当前 hipVS 上报错，未进入提交版。

## 验证口径

同一 Radeon Cloud GPU，gfx1100 / ROCm 7.2.4，16 核 CPU、55 GiB 内存配额。
冻结 V5 bridge 与候选 bridge 都由标准 `tools/hipvs_bridge/build.sh` 从源码编译。
实际改动只有动态 bridge，SQL 验证复用此前完整编译的、C++ 未改动的 V5
Release 主程序；这不是一次重新编译全部 seekdb 的记录。测试检查了实际加载的
DSO 路径和 SHA-256，防止 `LD_LIBRARY_PATH` 选错版本。

公开 SIFT 的前 250K 条、前 4096 查询，以及自建 Gaussian/clustered 256 维
250K 条、4096 查询使用独立 FP64 穷举 L2 真值，分别检查 Top-10/100。
另检查课程 10K 样例。每项做三轮独立建图，每轮批量查询预热一次、计时三次。
统计以各轮中位数汇总，保留原始轮次，不选择最好成绩。

独立 bridge 的建图/查询计时仅用于定位；正式分数的构建口径是冷 SQL DDL
直到首条合法固定查询完成。SQL 配对另测该完整区间，在计时前完成导入，
使用非连续主键，检查 Top-10/100、CPU VSAG 基本功能和停服后的索引重载。
SQL 自测每组 256 查询逐条发送；4096 批量查询质量/速度另由 bridge 测试覆盖。
两种计时不能混作正式平台 QPS。

## 本轮结果（2026-11-02）

冷 SQL DDL 至首条查询，三次独立数据库运行的中位数；第二轮交换先后顺序：

| 数据 | V5 秒 | 候选秒 | 加速比 |
| --- | ---: | ---: | ---: |
| 公开 SIFT250K / 128 维 | 19.9434 | 17.6377 | 1.1307× |
| 自建 Gaussian250K / 256 维 | 20.7464 | 18.9182 | 1.0966× |

独立 bridge 三轮中位数结果，查询列为 Top-10/100 QPS 比的几何平均，
Recall 列为候选三轮最差值。这些是开发指标，不是官方端到端成绩：

| 数据 | 构建至首查加速 | 批量查询加速 | Recall@10 | Recall@100 |
| --- | ---: | ---: | ---: | ---: |
| SIFT10K | 1.1338× | 1.4170× | 1.000000 | 1.000000 |
| SIFT250K | 1.1703× | 1.5211× | 0.999927 | 0.999971 |
| Gaussian256 / 250K | 1.0890× | 1.0741× | 0.950342 | 0.899534 |
| Clustered256 / 250K | 1.1461× | 1.2371× | 1.000000 | 1.000000 |

四组批量结果均通过绝对下限、相对基线差、查询回归和采样资源检查。
bridge 按最好基线对最差候选作保守质量检查；SQL 的 256 条单查询诊断
按每一轮的对应基线比较，不能把不同轮的 Recall 极值当作同轮基线。
全部六次 SQL 运行的 CPU VSAG、实际 DSO 加载校验及停服重载通过。
官方 `test_persistence.cpp` 的写入、重载和两种错误形状拒绝也通过。
所有开发测试中最大采样显存约 9.46 GB；SQL 候选最大采样 RSS 约 4.40 GB；
cgroup OOM/OOM-kill 计数均为 0。采样间隔为 50 ms，正式资源峰值仍由平台判断。

[完整对比 JSON](amd-v5-performance-20261102/paired-summary.json) ·
[执行与源码哈希](amd-v5-performance-20261102/execution.json) ·
[原始记录及逐轮返回 ID](amd-v5-performance-20261102/full-evidence.tgz)。
压缩包不包含二进制、凭据或数据集；SHA-256：
`014e01c6600ca819dfa8eb10fc088102f4350fd1eaea04bc3857c929b36d082f`。
包内保留未采用的探索结果；部分旧探索因过严的快照 ID 字节一致断言中断，
不能当作完整验收。最终结果以 `final-*` 和三组 SQL 配对记录为准。

汇总三组 SQL 对照：

```bash
python tools/hipvs_bridge/benchmarks/compare_results.py \
  --evidence /path/to/extracted/evidence \
  --sql-pair round2-baseline round2-selected \
  --sql-pair round3-baseline round3-selected \
  --output /path/to/paired-summary.json
```

正式平台成绩以实际提测为准，未运行或获取 Hidden 数据。

## 复现和限制

运行脚本、数据来源和 SHA-256 见
[benchmark 说明](../tools/hipvs_bridge/benchmarks/README.md)。
正式 [AMD 规则](https://github.com/AMD-DEV-CONTEST/Oceanbase-2026-competition-AMD-Doc)
还要求逐组满足同轮 baseline Recall-0.005、资源和查询回归门槛；这些阈值
不能用平均 Recall 抵消。合法生成的 256 维样本无法证明 Hidden 数据通过。
本报告不把 bridge 的加速比直接换算为正式分数。
