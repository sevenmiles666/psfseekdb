# MiniVectorDB 材料说明

本地材料以 ltc 代码 `dccb61eb98b9f6a98cee30d59aca5684d9fedc15` 为基线。主文件为 `agent/conversation.json`，根目录 `conversation.json` 是字节相同的副本。

此前将操作者与 Agent 的工作对话用作学生发言，来源归属错误。本次已删除全部这类对话、学生证据引用、候选学习实例及对应的贡献判断。操作者身份不等于学生身份，不能由 JSON 中的 `role: user` 推导出学生贡献。

当前材料保留 Agent 编写的开发指令、各历史版本的真实工具实验、本地正确性与资源验证、六项工程实现的实际结果及限制。开发指令与技术复盘保持 assistant 来源，实验输出保持 tool 来源。详细工程结果见 [optimization_results.md](optimization_results.md)。

2026-11-15 在 `learning_record` 中加入四部分导航，关联协议握手、Prompt 迭代、语句级撤销和离线恢复的原始记录。用当前引擎源码 `f8b3115` 在 Ubuntu WSL 复测了两组用例。过滤和舍入并列用例中，重复键错误只撤销当条语句，事务回滚与重开均返回原结果。强杀恢复用例中，WAL 从 248 B 增至 96,508 B，重启后恢复到原来的 248 B 并与已提交日志逐字节一致。两组均使用独立空目录和 JSONL 接口。

当前没有经核实的学生对话或学习证据，JSON 的 `attribution.student_evidence_status` 标为 `not_available`。没有新增虚构发言、实验结果或自评分数。工具验证结果只说明工程行为，不能代替学生理解。

`material-manifest.json` 记录当前双副本、工具载荷、开发指令及运行源码的摘要。运行：

```sh
python3 tests/check_prompt_material.py
```

校验检查 JSON、文件摘要、结果引用及来源一致性，并阻止本份材料再次混入 user/student 对话或学生评分实例。它不模拟官方评分。

本次整理沿用已有的引擎实现，并记录材料复核及本地实验。正式成绩以平台报告为准。
