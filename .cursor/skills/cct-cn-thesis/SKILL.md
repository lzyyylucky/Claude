---
name: cct-cn-thesis
description: >-
  Aligns AI assistance with the graduation thesis on Chinese-localized LLM code
  tooling: layered CLI/Web architecture, prompt-as-code, agents/skills, and
  CCT_WORKSPACE file writes. Use when editing cct-cn-cpp, 毕业设计开题报告.md,
  or discussing agents, skills, templates, or token/context limits for this project.
---

# cct-cn-cpp 与毕业设计对齐

## 项目要点

- **Web 路径**：用户数据目录下 `users/<id>/components/agents|skills/*.md` 由「组件工作室」维护；聊天请求可带 `agent`、`skill` 字段，服务端在调用模型前把对应 Markdown 注入本条 user 消息前。
- **工作区**：服务端或本机授权目录可提供 `workspace_bundle`；多文件写回依赖回复末尾的 **`CCT_WORKSPACE:`** JSON（见仓库内 `component-templates/skills/cct-workspace-output.md` 模板）。
- **开题报告概念映射**：开题中的「角色模版」对应本系统的 **Agent**；「任务指令 / 流程约束」对应 **Skill**。

## 协助用户时的偏好

- 解释与文档用**简体中文**；代码标识符与仓库一致。
- 区分 **Cursor Skill**（本目录，给 Cursor Agent 用）与 **站内 Agent/Skill**（浏览器里 `component-templates` + 组件工作室）。
- 修改后端时保持改动面小；新增能力优先接在现有 `handle_api` 与 `chat.js` 模式上。
