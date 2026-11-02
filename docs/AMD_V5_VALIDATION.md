# V5 基线修复验收（2026-11-02）

## 修复内容

官方 AMD 文档将 `failMsg=8` 定义为冻结 V5 基线校验失败。
本分支真正从 `634888d9b037153fa143d2c913ba4b25503e1d62` 建立；
未将旧 CPU VSAG 查询方案合并进来，也没有用空合并伪造继承关系。

本次完整构建与 SQL 验证对应 `d4840ee3ea658250810883a63b299b428290d3c0`。
该提交的父提交就是冻结 V5，产品源码与 V5 完全一致，只新增说明和祖先检查脚本。
后续提交仅加入本报告和验证记录，不表示进行了性能优化。

本机 Git 祖先检查、GPU 实例中 `script/check_amd_v5.sh` 的检查均通过；
并核对 `src`、`include`、`cmake`、`deps`、`rust`、`tools` 及根构建脚本与 V5 无差异。
9,000 个跟踪文件构建前后哈希一致，并与本机独立浅克隆逐一交叉核对。

## 构建与运行

环境：Linux x86-64、AMD gfx1100、ROCm 7.2.4、16 CPU / 55 GiB 内存配额。
使用镜像自带 Rust/Cargo 1.97.1；V5 锁文件的 70 个 registry 包按其校验和准备。
实际执行官方 bridge 构建脚本及以下 Release 入口并通过：

```bash
tools/hipvs_bridge/build.sh /workspace/amd-v5-repair-20261102/bridge/libseekdb_cuvs_bridge.so
./build.sh release \
  -DOB_BUILD_CUVS=ON \
  -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB=/workspace/amd-v5-repair-20261102/bridge/libseekdb_cuvs_bridge.so \
  --make -j4
```

依赖由独立验证驱动预先准备，未改动产品源码或锁文件。
V5 清单中的 obshell 4.5.0.1 RPM 下载返回 404，保留了首轮失败日志；
核心 Release 构建不需要该安装包，因此之后验证核心构建和 SQL，未声称通过 CPack/RPM/DEB。
此处不是对组织方私有离线构建镜像的完整仿真，也不要求组织方执行本地准备驱动。

V5 自带 `tools/hipvs_bridge/test_persistence.cpp` 通过真实 GPU 建图、
快照写入/读取及错误形状拒绝检查。SQL 验证使用本次新构建的主程序和 bridge，
不使用镜像预装 seekdb 代替。

## 小规模 SQL 验证

| 数据 | 行数 × 维度 | 查询数 | Recall@10 | Recall@100 |
|---|---:|---:|---:|---:|
| 课程镜像 SIFT 数据 | 10,000 × 128 | 64 | 1.000 | 1.000 |
| 固定种子自生成数据 | 10,000 × 256 | 64 | 1.000 | 1.000 |

索引使用 `lib=cuvs`，先导入数据再运行 CREATE VECTOR INDEX。
每条近似查询均检查恰好 k 个合法、不重复的原始 ID；采用非连续主键检查 ID 映射。
独立 NumPy float64 精确距离计算真值。

主进程实际打开 `/dev/kfd` 和 GPU render 设备；重启后两组 Top-100
查询 Recall 均为 1.000，日志记录了 GPU CAGRA 快照反序列化。
另检查了 CPU `lib=vsag` 索引基本查询。最后正常停止数据库。

这些结果不是正式 250K 数据、`nq=4096` 验收，不是 Hidden 数据结果，
没有与 V5 配对测量构建/查询加速比；**本轮不声称获得额外性能分或正式通过全部门槛**。
原始烟测记录的旧 `ddl_seconds` 字段包含后续真值准备时间，已附口径说明；
汇总中改用 `ddl_plus_ground_truth_seconds`，不将其当作正式建索引计时。

## 验证材料

- [汇总](amd-v5-validation-20261102/summary.json)、[SQL 检查](amd-v5-validation-20261102/sql-smoke.json)。
- [祖先检查](amd-v5-validation-20261102/ancestry.log)、[完整构建日志](amd-v5-validation-20261102/release-build.log)。
- [GPU 快照事件](amd-v5-validation-20261102/snapshot-events.log)、[跟踪文件哈希](amd-v5-validation-20261102/source-sha256.json)。
- [原始证据及独立重放驱动](amd-v5-validation-20261102/full-evidence.tgz)，包含环境准备失败与恢复过程；可读日志仅规范化行尾空白，原始字节保留在压缩包。

官方规则来源：https://github.com/AMD-DEV-CONTEST/Oceanbase-2026-competition-AMD-Doc

下一次提测应使用本分支 `competition/amd-v5-fix` 和已经推送的最新提交。
该初始版本用于恢复正确的 V5 提交前提；性能优化应在本分支上以 V5 GPU 为对照继续开发。
