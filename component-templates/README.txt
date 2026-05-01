本目录为「组件工作室」可用的示例 Markdown，与《毕业设计开题报告》中的角色模版 / 提示词流程一致。

如何导入到本系统 Web：
1. 登录后打开侧边栏「组件工作室」。
2. 选择类型 Agents 或 Skills，点「新建」。
3. 名称填写与文件名 stem 一致（不含 .md），例如 architect、think-chain-codegen。
4. 将对应 .md 全文粘贴到编辑器，保存。
5. 回到「新对话」：输入框下方「角色 Agent / 技能 Skill」下拉框会刷新；选好后发消息，服务端会把该文档注入本轮模型请求。

与 Cursor IDE 的 Skills 区别：Cursor 使用仓库内 .cursor/skills/*/SKILL.md；本 Web 应用使用上述用户目录下的 agents/skills。二者可并行维护相似内容。
