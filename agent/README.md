# 学习材料修订与平台反馈

用户确认 `fd9011a30d53c625ff67f19eb6838fbf407bf8b4` 的正式性能原始分为 4368，Agent Prompt 原始分为 1。2026-11-14 的后续记录包含用户给出的目标、版本确认和 Agent 执行的实验。该记录位于 `conversation.json` 的 `lossless_vector_iteration`。

历史记录保留了 `c16935a` 的正式任务 `47251293`，以及 `9eb5b15` 之后的解析、向量日志和查询对照。材料使用 `user`、`assistant`、`tool` 标明交互及实验来源。

## 输入修订

- `agent/conversation.json` 为题面默认材料路径；根目录 `conversation.json` 是字节相同的兼容副本。
- `material_summary` 是 Agent 整理的材料范围摘要；`provenance` 说明代码基础、历史材料来源和新增内容来源。
- 历史用户引文、Agent 消息和实验字段原样保留。新增当前会话中用户质疑性能指标、Agent 核对并更正、用户选择旧版的实际片段，以及两次正式任务的结果。
- `evidence_navigation` 提供可定位的 JSON pointer 和精确引文。这些导航不预先指定分数，也不将同一条交互链拆成多个贡献。
- `material-manifest.json` 记录材料字节数、SHA-256、导入内容摘要和本版本运行文件哈希，便于独立核验。

使用 Python 3 检查输入完整性：

```sh
python3 tests/check_prompt_material.py
```

校验 UTF-8、顶层对象、128 KiB 上限、重复键、非有限数值、两份副本、材料摘要、引文和历史字段，同时核对运行文件的摘要。平台评审状态和原始分以正式反馈为准。

## 历史评审反馈

早于 `fd9011a` 的两个正式任务返回了同一条提示。

> Prompt结果无效、待复核、超出平台0–100范围或缺少材料摘要，本项不参与排名；未截断或换算本地分数。

后续 `fd9011a` 已返回数值。当前记录只有用户提供的原始分，具体维度、计分实例和评审报告待平台反馈。题面第 3.4 节评价选手在实际交互中体现的理解、验证与改进。[官方赛题说明](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/lg8enxlmwg69unt6)
