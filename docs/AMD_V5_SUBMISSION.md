# AMD V5 基线修复分支

本分支为 `competition/amd-v5-fix`，直接从官方冻结 V5 提交
`634888d9b037153fa143d2c913ba4b25503e1d62` 建立，不合并旧实现来伪造祖先关系。

官方来源：
https://github.com/AMD-DEV-CONTEST/Oceanbase-2026-competition-AMD-Doc

旧 `competition/amd-hipvs-build` 基于通用 seekdb 比赛分支，不包含 AMD 冻结 V5，
不满足本题的版本前提。官方文档将此类失败定义为 `failMsg=8`。
旧分支的 CPU/GPU 构建对照不是官方 V5 GPU 基线对照，不能映射为正式性能分。

## 本次范围

这是恢复正确基线的初始版本：保留官方 V5 的 CAGRA GPU 构建、GPU 查询、持久化、
bridge ABI 及 CPU VSAG 基本功能，不迁入旧版“GPU 构图后改用 CPU VSAG 查询”的实现。
本提交只增加资格检查脚本和说明；尚未新增性能优化，不承诺正式分数或完整隐藏用例通过。

**2026-11-02 验证完成**：本机及 GPU 实例的 V5 祖先检查通过；官方 bridge 与完整 Release 构建通过，
128/256 维各 10K 行的 Top-10/100、GPU 快照重载和 CPU VSAG 基本查询通过。
范围、原始记录及 obshell 下载 404 的安装包限制见 [验证报告](AMD_V5_VALIDATION.md)。

在仓库根目录执行：

```bash
bash script/check_amd_v5.sh
```

预期 `ANCESTRY=PASS`。该检查不能代替编译和运行验收。浅克隆若缺少基线对象，
先获取官方 V5 分支再检查。

## 构建约定

按官方 V5 bridge 和 seekdb 入口构建，不要求评测端追加旧项目的 HIPVS worker 开关：

```bash
tools/hipvs_bridge/build.sh /work/bridge/libseekdb_cuvs_bridge.so
./build.sh release \
  -DOB_BUILD_CUVS=ON \
  -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB=/work/bridge/libseekdb_cuvs_bridge.so \
  --make -j4
```

路径和并行数需适配本地资源；比赛实际构建命令及离线依赖约定以组织方为准。
源码需满足 ROCm 7.2.4 / gfx1100，不通过预编译提交物代替源码构建。

后续优化须以同轮 V5 GPU 为基线，覆盖正式规则中的 128/256 维数据、Top-10/100、
批量查询、质量相对门槛和资源上限。自测合法生成数据不能代表 Hidden 数据通过。
