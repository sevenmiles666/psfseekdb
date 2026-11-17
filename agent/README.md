# MiniVectorDB 材料说明

主文件 `agent/conversation.json` 与根目录 `conversation.json` 字节相同，包含用户给出的 25 项验证方案及复盘、实际响应和测量结果。实验以 `9c0aa92005ab4858f42dc4ca32793056e20a5c73` 为基础，当前分支的性能对照另有记录。[性能自测](../tests/round2-exact-topk-benchmarks.json)

提测版保留了全部 25 项和 93 条定位引用。为符合 128 KiB 的本地检查上限，第 5 项仅保留建表、表 B 写入和重启查询的请求响应及完整结果摘要。原始 JSON 逐字节保存。[完整原件](evidence/learning-conversation.full.json)

`material-manifest.json` 记录双副本、原件和运行源码的摘要。运行 `python3 tests/check_prompt_material.py` 校验材料、引用及源码一致性。正式评分由评测平台生成。

第 26～50 项覆盖事务、SQL、向量缓存、性能和协议。每项保留工具实测摘要、适用范围、原始记录及 SHA-256。[逐项报告](evidence/units-26-50/results.md) · [实测 JSON](evidence/units-26-50/tool-evidence.json)

## 历史材料

下文记载提交 `1f09a83` 及以前的材料。早期实验以 ltc 代码 `dccb61eb98b9f6a98cee30d59aca5684d9fedc15` 为基线。

历史导入时清理了来源无法确认的发言和评分引用。当前新增的用户记录均来自本会话。

当前材料保留 Agent 编写的开发指令、各历史版本的真实工具实验、本地正确性与资源验证、六项工程实现的实际结果及限制。开发指令与技术复盘保持 assistant 来源，实验输出保持 tool 来源。详细工程结果见 [optimization_results.md](optimization_results.md)。

2026-11-15 在 `learning_record` 中加入四部分导航，关联协议握手、Prompt 迭代、语句级撤销和离线恢复的原始记录。用当前引擎源码 `f8b3115` 在 Ubuntu WSL 复测了两组用例。过滤和舍入并列用例中，重复键错误只撤销当条语句，事务回滚与重开均返回原结果。强杀恢复用例中，WAL 从 248 B 增至 96,508 B，重启后恢复到原来的 248 B 并与已提交日志逐字节一致。两组均使用独立空目录和 JSONL 接口。

新增的 `recorded_user_participation` 保存用户指定 `f8b3115` 优化基线、要求固定 Prompt、确认正式评测提交和询问 Prompt 零分的原话。另附 Git 对照和用户提供的成绩截图内容。该记录体现版本选择和结果反馈，技术解释与实验仍按实际作者记录。为符合 128 KiB 上限，省去一个旧 Agent 讨论选段，原文保存在 `eaa56da`。
