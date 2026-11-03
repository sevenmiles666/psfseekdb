# V5 第二轮性能优化

后续版本见 [第三轮性能优化](AMD_V5_PERFORMANCE_3.md)。用户反馈本轮提交
`b775ded989531f42f4a3b700aa713971fb1912d3` 正式成绩为 8.9 分。
第三轮发现旧显存采样会取所有主机 DRM 设备的最大值，可能包含其他 GPU；
因此本文历史显存数字不能作为本实例资源门槛的证据。新报告按实际 HIP
设备的 PCI 地址采样并验证，不改写本文其他历史测量。

分支：`competition/amd-v5-perf2`。父提交为
`f224065668dbf09bbfd24154321637235cf79baf`，用户反馈该版本平台成绩为 6 分。
原 `competition/amd-v5-perf` 保留用于回退。本版继续继承官方冻结 V5
`634888d9b037153fa143d2c913ba4b25503e1d62`。

## 产品改动

仅修改 `tools/hipvs_bridge/seekdb_cuvs_bridge.c` 四项固定 CAGRA 参数：

| 参数 | 冻结 V5 | 上一版 | 本版 |
| --- | ---: | ---: | ---: |
| intermediate_graph_degree | 256 | 256 | 192 |
| nn_descent_niter | 100 | 80 | 64 |
| search_width | 4 | 16 | 32 |
| num_random_samplings | 库默认 | 库默认 | 4 |

减少中间图和细化工作，用更宽的搜索前沿和更多入口采样补偿搜索质量。
所有数据统一使用这些参数；输出图度数仍为 128，itopk 下限仍为 512，
输入和内部数据保持 FP32。保留真实 GPU NN-descent 建图和 GPU CAGRA
搜索；ABI、快照格式、seekdb 主程序、CPU VSAG、标准编译入口均不改变。

## 数据和验证口径

Radeon Cloud，gfx1100 / ROCm 7.2.4，16 CPU、55 GiB 内存配额。
公开 SIFT250K/128 维、种子 20261102 的 Gaussian250K/256 维及
Clustered250K/256 维均用 4096 条查询、独立 FP64 穷举 L2 真值，
检查 Top-10/100。SIFT10K 单独覆盖 Quick 场景，种子 20261103 的另一组
Gaussian 数据复核泛化。自生成数据不是官方 Hidden 数据。

冻结 V5 Release 主程序在本实例从源码完整构建，候选 bridge 使用本提交
源码及标准 `tools/hipvs_bridge/build.sh` 重新构建。SQL 检查实际加载的
动态库路径和 SHA-256，校验非连续主键映射、CPU VSAG 以及停服重载。

恢复开发源码包时发现一个未跟踪、零字节的生成文件 `type_name.c`，初轮
链接缺少 `get_type_name`；用 V5 自带生成器和规范头文件重新生成后构建
成功。没有修改跟踪源码或锁文件，失败和恢复日志均保留。

最终 bridge 每组做三轮独立建图，每轮每个 k 预热一次、计时三次。
冷构建时间取三轮中位数，查询加速取 Top-10/100 中位 QPS 比的几何平均；
质量按最好基线对最差候选检查，资源以 50 ms 间隔采样。
这些是开发验证，不能代替正式平台评分。

## 最终 bridge 三轮结果

| 数据 | V5 构建至首查秒 | 本版秒 | 构建加速 | 查询加速 | 最差 Recall@10 | 最差 Recall@100 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| sift10k | 0.5637 | 0.3728 | 1.5121× | 0.9186× | 1.000000 | 1.000000 |
| sift250k | 14.6098 | 8.1705 | 1.7881× | 1.3615× | 0.999927 | 0.999956 |
| gaussian256 | 15.3552 | 9.2196 | 1.6655× | 1.0220× | 0.950952 | 0.905339 |
| clustered256 | 15.5636 | 9.3183 | 1.6702× | 1.3391× | 1.000000 | 1.000000 |

四组均通过当前数据的绝对 Recall、相对基线差、查询回归和采样资源检查。
最大采样设备显存 9.464 GB。
第二随机种子的最差 Recall@10/100 为 0.950610/0.904912，查询比 1.0232，配对检查通过。


## SQL 冷建索引三轮结果

导入完成后测量冷 DDL 至第一条合法查询。每轮使用独立新数据库。
基线、上一版在前序轮换顺序测试；本版在搜索参数确定后单独执行三轮，
不宣称三者交错随机顺序。每轮随后用 256 条逐条查询检查 ID 和召回，
这个查询方式是诊断，不是正式 nq=4096 批量 QPS。

| 数据 | 冻结 V5 中位秒 | 6 分版本中位秒 | 本版中位秒 | 对 V5 加速 | 对 6 分版加速 |
| --- | ---: | ---: | ---: | ---: | ---: |
| gaussian256 | 20.9342 | 19.1796 | 14.8865 | 1.4063× | 1.2884× |
| sift250k | 19.6636 | 17.5438 | 12.0922 | 1.6261× | 1.4508× |

三轮 SQL 均完成动态库核对、CPU VSAG 和停服重载；每对 SQL Recall
检查的详细阈值和结果见原始汇总。不将 bridge 速度当作 SQL 冷 DDL 速度，
也不将上述两组数据套用为官方 Hidden 分数。

## 筛选与排除

- 迭代数 50 的部分组合 Recall 不通过或过于接近基线差门槛，未采用。
- search_width=64/128 在本次镜像首查询返回 rc=15，未采用。
- 默认 IVF-PQ 初始化较快，但质量及部分资源检查不通过，未采用。
- 中间图 160、64 轮更快，但第二随机种子 Top-10 距通过线仅约 0.00095；
  本次开发额外要求该数据至少留 0.0015 余量，因此选择 192。此余量不是赛事规定。
- 最初 192/64/32 候选批量检查通过，但三轮 SQL 中两轮逐条查询 Recall@10
  比配对基线下降超过 0.005，没有将其作为最终版。2 次入口采样改善有限；
  最终采用固定 4 次采样，并重新从仓库源码执行所有最终检查。

探索结果也保留，`final-selected` 是未采用的三参数候选；
最终提交对应证据标签 **`release-selected`**，三轮 SQL 标签为
`release-selected`、`release2-selected`、`release3-selected`。

## 原始证据和复现

[完整比较](amd-v5-performance-2-20261102/release-summary.json) ·
[对上一版比较](amd-v5-performance-2-20261102/compared-with-scored.json) ·
[源码/二进制哈希](amd-v5-performance-2-20261102/execution.json) ·
[原始实验记录](amd-v5-performance-2-20261102/full-evidence.tgz)

证据包包含最终返回 ID、独立种子记录、构建日志、测试驱动和候选参数源码，
不含数据库、向量数据、预编译二进制或凭据。`release_validate.sh` 给出
本次最终验证命令；通用数据准备和脚本说明见
[benchmark README](../tools/hipvs_bridge/benchmarks/README.md)。
复核比较可对解压目录执行：

```bash
python compare_results.py --evidence evidence --candidate release-selected \
  --sql-pair round2-baseline release2-selected \
  --sql-pair round3-baseline release3-selected --output checked.json
```

正式构建不需要新增自定义开关或额外数据下载。运行记录保留 SHA-256：

- 产品源码 LF SHA-256：`970959e84fae685a9264c44935aa470280ba6f32c4aba9cdcdf16ab0edf19d18`。
- 原始证据包 SHA-256：`987bf4152b9752ab807d775f9701018e5947546d8ae2bdf4a0c7c721957eef58`。

采样显存峰值和本机环境不代替官方资源统计；正式成绩及 Hidden 验收以平台为准。
官方规则：[AMD 赛事说明](https://github.com/AMD-DEV-CONTEST/Oceanbase-2026-competition-AMD-Doc)。
