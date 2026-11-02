# 2026-11-02 新 SSH 实例完整复现

按用户提供的流程，在 Radeon Cloud 创建启用 SSH 的 seekdb-hipvs 镜像实例，并在新实例中验证提交 `f3cfe8b1738c2ce0d71a7cd04e8c2c69f6f938c1`。本轮不修改产品源码；GitHub 后续提交只新增本报告及证据。

**结果：上传精确源码后的完整构建、真实 TGZ 打包和运行验收通过；实例直接拉取 GitHub 失败。两者分别记录，不能称为整条网络冷启动流程全部通过，也不能据此确认官方 failMsg=8 的根因。**

## 新实例与源码获取

实例为 Linux x86-64、gfx1100、约 48 GiB 显存，16 CPU、55 GiB 内存配额，课程 hipVS/ROCm 已安装。记录硬件架构，不从架构字符串推断商业显卡型号。

通过用户现有 SSH 密钥连接成功。实例保留平台注入的 HTTP/HTTPS 代理，未修改路由、绕过代理或关闭 TLS 校验。实际 GitHub 检查出现：

- `git ls-remote`：`CONNECT tunnel failed, response 503`。
- 浅克隆：`GnuTLS recv error (-110): The TLS connection was non-properly terminated`。
- 同一实例中 GitHub Git 入口、raw 入口的 HTTPS 检查在代理 CONNECT 阶段超时；API 读取相同提交返回 HTTP 200。

以上是本轮 Radeon 实例的实测，**不是官方评测机的网络日志，也不等同于权限或 Token 验证失败**。见 [克隆日志](results/radeon-20261102-ssh-reproduction/clone.log)及[端点检查](results/radeon-20261102-ssh-reproduction/github-endpoint-checks.json)。未使用聊天截图中的任何 Token。

为继续验证源码，在本机独立浅克隆比赛分支，核对精确提交号后，将包含 Git 元数据的源码副本上传到实例。源码传输包 SHA-256 为 `95add40e722a8397376de8d1d80c8870a5c8f1e162b7d8728db16c8ebb1142fd`；实例恢复 Git 跟踪的原始文件及执行权限。构建及运行前后 9,512 个跟踪文件哈希一致，本机独立克隆也逐文件交叉核对通过。

## 实际编译入口

在独立工作目录 `/workspace/amd-repro-20261102/source-host` 执行真实 `.github/script/seekdb/compile.sh`：

```bash
export PATH=/workspace/amd-repro-20261102/tooling/rust-1.98.1/bin:/opt/venv/bin:/opt/rocm/bin:$PATH
export CARGO_HOME=/workspace/amd-repro-20261102/tooling/cargo-home
export CARGO_NET_OFFLINE=true CARGO_BUILD_JOBS=1
export GITHUB_WORKSPACE=/workspace/amd-repro-20261102/source-host
export SEEKDB_TASK_DIR=/workspace/amd-repro-20261102/evidence/task
export PACKAGE_TYPE=tgz MAKE_ARGS=-j4 SEEKDB_BUILD_HIPVS=ON
bash .github/script/seekdb/compile.sh
```

前置工具链由先前校验的官方 Rust/Cargo 1.98.1 原包安装到私有工作目录，123 个锁定 Cargo 依赖经 SHA-256 校验后离线使用。本次没有重复验收从官方网络冷下载 Rust。29 个实际 RPM 由真实依赖初始化下载，并用候选的 libarchive fallback 解包；没有安装系统 RPM 工具。

实际缓存中 CUVS/HIPVS 均 ON；另核对了主程序的 `OB_BUILD_HIPVS` 编译宏和实际 GPU worker/bridge 生成目标。完整编译入口耗时 **1176 秒，最终阶段 complete，退出码 0**。随后执行 `generate_syspack_source` 及完整 `make -j2`，再运行真实 CPack。不是只配置后用占位 make 提前结束。

48 项 Linux 回归通过，无跳过；本轮 cgroup OOM/oom_kill 计数均为 0。见[编译原始输出的可读副本](results/radeon-20261102-ssh-reproduction/compile-entry.log)、[回归输出](results/radeon-20261102-ssh-reproduction/regressions.log)和[完整证据包](results/radeon-20261102-ssh-reproduction/full-evidence.tgz)。

## 安装包、SQL 与真实构建性能

CPack 生成 server/libs 两个 TGZ。解包核对主程序、obshell、GPU worker、桥接库、libaio、配置和 systemd 文件；主程序/worker 的 `ldd` 均无缺库。不执行系统安装钩子，不设置 `LD_LIBRARY_PATH` 或 `LD_PRELOAD`，启动解包后的主程序。

主程序版本包含本次完整提交号；主程序 SHA-256：`6c8687bd459401d7c477d6a22c3734302c1e9b61eeb7022051ccc67b7791dbea`。未使用镜像预装的 seekdb 作为被测程序。

27 项 SQL 检查通过。构建基准为 50,000×128 float32、L2、M=32、ef_construction=200、ef_search=1000，100 条独立查询，CPU/GPU 各三轮 fresh 建索引，交替顺序。计时从建索引到同一首次可用性查询完成，包括扫描、GPU 传输/同步、构图、图转换、导入及持久化，不包括数据生成和导入。

| 轮次 | CPU/VSAG 秒 | GPU/hipVS 秒 | CPU Recall@10 | GPU Recall@10 |
|---|---:|---:|---:|---:|
| 1 | 24.993573 | 2.995347 | 0.996 | 1.000 |
| 2 | 25.464238 | 2.998627 | 0.996 | 0.999 |
| 3 | 25.291845 | 2.999152 | 0.996 | 0.998 |
| 中位数 | **25.291845** | **2.998627** | | |

中位数比为 **8.434475715×**。三轮 GPU 基准 JSON 均保留实际 `gpu=true fallback=false` 构建事件；日志观察器共保存 4 条 GPU 构建事件，包含后续重建。它们不是 profiler 内核轨迹。

更新、删除、重插通过，过滤 Recall@10=1.000。隐藏 GPU 后重启同一数据库，100 条持久化查询通过，Recall@10=0.999，并通过精确 SQL 交叉检查。最终数据库已正常停止。0.99 是本地验证门槛，不是已确认的官方评分门槛。

## 产物及边界

- [完整汇总](results/radeon-20261102-ssh-reproduction/final-summary.json)、[原始 SQL/基准结果](results/radeon-20261102-ssh-reproduction/package-sql.json)、[安装包记录](results/radeon-20261102-ssh-reproduction/package-status.json)。
- [跟踪源码哈希](results/radeon-20261102-ssh-reproduction/source-sha256.json)、[完整原始日志与重放驱动](results/radeon-20261102-ssh-reproduction/full-evidence.tgz)。可读文本日志仅规范化行尾空白，原始字节保存在证据包内。
- 本地交付目录 `E:\qcoder\.research\amd-repro-20261102\delivery` 保存本次两个 TGZ、证据包和 SHA256SUMS。317 MB 的运行包不放入 Git 源码仓库。
- 未重新验收 RPM/DEB 系统安装钩子、Bazel GPU 或从零联网下载全部 Rust/Cargo 依赖。本轮依赖缓存条件已明确记录。
- **官方 failMsg=8 仍未取得解释或首错日志。** 当前可向官方补充具体网络错误，并请求核对官方拉取端的代理、首个失败命令及对应 stderr；本轮成功不能替代正式成绩。
