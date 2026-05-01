# CCT-CN 工程文件索引（源码与资源）

本文说明**仓库内主要源文件与静态资源**各自职责。  
**不含**：`build/` 构建产物、`ide-app/node_modules/`、本地 `data/` 运行数据、`.vs/` IDE 缓存等。

---

## 根目录

| 文件 | 作用 |
|------|------|
| `CMakeLists.txt` | CMake 工程入口：目标、源码、链接、复制 `ui/`、`git-worker/` 与 `cct-storage.dll` 等。 |

---

## `src/` — C++ 核心

### 入口与 CLI

| 文件 | 作用 |
|------|------|
| `main.cpp` | 可执行程序入口：`init` 生成配置、`serve` 启动 HTTP；非 Windows 下可能仅编译占位。 |

### `src/util/` — 通用工具

| 文件 | 作用 |
|------|------|
| `config.hpp` / `config.cpp` | 应用配置结构体、读/写 JSON 配置、`save_example_config`；`apply_cli_workspace_override` 在头文件内联（CLI `--workspace` 覆盖工作区根路径）。 |
| `json_minimal.hpp` / `json_minimal.cpp` | 轻量 JSON 解析/转义，避免引入大型第三方库。 |
| `paths.hpp` / `paths.cpp` | 配置文件默认路径等平台相关路径解析。 |
| `utf8_string.hpp` | UTF-8 安全截取、省略号等字符串工具。 |
| `component_name_utf8.hpp` / `component_name_utf8.cpp` | 组件（Agent/Skill）名称的 UTF-8 规范化与校验。 |

### `src/llm/` — 大模型调用

| 文件 | 作用 |
|------|------|
| `api.hpp` | 通用聊天消息结构、客户端结果类型等 LLM 抽象。 |
| `anthropic_client.cpp` | Claude / Anthropic Messages API（WinHTTP）。 |
| `zhipu_client.cpp` | 智谱 GLM OpenAI 兼容接口调用。 |
| `mock_client.cpp` | 本地 Mock，不连网时返回占位内容。 |

### `src/context/`、`src/generator/`、`src/template/` — 代码生成 CLI 路径

| 文件 | 作用 |
|------|------|
| `context/scanner.hpp` / `scanner.cpp` | 扫描工程上下文（供非 Web 的代码生成/CLI 流程使用）。 |
| `generator/writer.hpp` / `writer.cpp` | 将小片段模板或生成物写入磁盘。 |
| `template/loader.hpp` / `loader.cpp` | 加载磁盘上的模板文本。 |

### `src/web/` — Web 服务器与认证

| 文件 | 作用 |
|------|------|
| `server.hpp` | 声明 `run_http_server`（Windows 实现于 `server.cpp`）。 |
| `server.cpp` | HTTP 路由：静态 UI、`/api/*`（登录、聊天流、代码扫描、工作区与 **`/api/workspace/git/*`**、计费、数据分析等）；会话 Cookie；与存储抽象对接。 |
| `git_worker_client.hpp` / `git_worker_client.cpp` | 转发 JSON 至本机 `git-worker`（WinHTTP `127.0.0.1`）；可选拉起 Node 子进程。 |
| `http_util.hpp` / `http_util.cpp` | 原始 HTTP 解析、响应封装、Cookie 读写等底层工具。 |
| `crypto.hpp` / `crypto.cpp` | 随机字节、PBKDF2、十六进制等密码学辅助（用户密码哈希）。 |
| `user_store.hpp` / `user_store.cpp` | 基于 JSONL 的本地用户表读写；每用户 `preferences.json`（主题等）。 |

### `src/storage/` — 持久化抽象与实现

| 文件 | 作用 |
|------|------|
| `storage_iface.hpp` | 接口：`IChatPersistence`、`IUserPersistence`、`IComponentPersistence`、`ITokenBillingPersistence` 及行结构体。 |
| `storage_helpers.hpp` | 聊天线程列表、默认会话等跨后端共用辅助函数。 |
| `token_billing_common.hpp` | 订阅档位规范化、月 Token 配额、单日调用上限等计费公共逻辑（无 `.cpp`，头文件内联）。 |
| `file_user_persistence.hpp` | 文件后端：`IUserPersistence` → `UserStore`。 |
| `file_chat_store.hpp` / `file_chat_store.cpp` | 文件后端聊天线程与消息落盘。 |
| `file_component_store.hpp` / `file_component_store.cpp` | 文件后端 Agent/Skill 等组件内容。 |
| `file_token_billing.hpp` / `file_token_billing.cpp` | 文件后端订阅与 Token 消耗状态（JSON 等）。 |
| `sql_bundle_exports.hpp` | 声明由 `cct-storage.dll` 导出的 SQL bundle C 符号。 |
| `sql_bundle.cpp` | SQL Server（ODBC）实现：用户、偏好、聊天、组件、计费表；连接时 DDL 补全；编进 `cct-storage.dll`。 |

---

## `ui/` — Web 前端（由服务端静态托管）

| 文件 | 作用 |
|------|------|
| `index.html` | 登录/注册落地页入口。 |
| `login.html` / `register.html` | 重定向到 `index.html` 对应锚点。 |
| `app.html` | 主应用壳：侧栏、聊天、IDE、代码检测、**仓库同步**、数据分析、设置/个人资料/订阅弹窗等。 |
| `js/app.js` | 应用引导、主题、个人资料、侧栏视图切换、设置与计费菜单接线。 |
| `js/auth.js` | 登录注册表单与跳转 `app.html`。 |
| `js/chat.js` | 对话发送、历史、模型选择与流式事件处理。 |
| `js/chat-session.js` | 会话 ID、线程状态等与聊天相关的客户端状态。 |
| `js/chat-project.js` | 项目/工作区与聊天侧栏联动（演示向）。 |
| `js/code-scan.js` | 代码扫描面板：选文件、维度、调用检测 API、展示结果与补丁。 |
| `js/components.js` | Agent/Skill/Command 组件列表与编辑。 |
| `js/model-picker.js` | 模型选择 UI。 |
| `js/analytics.js` | 数据分析页：拉取 `/api/analytics/summary`、图表与用量条。 |
| `js/landing.js` | 着陆页交互（若有）。 |
| `js/billing/billing-modals.js` | 升级套餐、收银台（支付方式、订阅 POST）。 |
| `css/style.css` | 全局与组件样式（含设置、计费弹窗等）。 |
| `css/chat.css` | 聊天区域样式。 |
| `css/analytics.css` | 数据分析页布局与图表区样式。 |
| `css/ide.css` | 内嵌 IDE 区域样式。 |
| `ide/ide.js` | 打包/内嵌的 CodeMirror 编辑器逻辑（由 `ide-app` 构建产出后复制，以仓库内版本为准）。 |
| `css/repo-sync.css` | 仓库同步页面布局样式。 |
| `js/repo-sync/repo-sync-main.js` | 仓库同步：Git API、Monaco Diff、冲突处理。 |
| `repo-sync/repo-sync-monaco.js` 等 | 由 **`repo-sync-app`** 构建的 Monaco 打包产物（含 `style.css`、`assets/*`）。 |
| `jsconfig.json` | VS Code/Cursor 的前端 JS 工程提示配置。 |
| `assets/hero-claudecode.svg` | 着陆页插图资源。 |

---

## `scripts/sql/` — 数据库脚本

| 文件 | 作用 |
|------|------|
| `schema.sql` | SQL Server 全量建库建表（Users、UserPreferences、UserBilling、Components、ChatThreads、ChatMessages 等）。 |
| `schema_incremental_userbilling.sql` | 仅增量补丁（UserBilling 等）供已有库升级。 |

---

## `component-templates/` — 默认组件模板（Markdown）

| 文件 | 作用 |
|------|------|
| `README.txt` | 模板包说明。 |
| `agents/*.md` | 预置 Agent 角色说明（如架构、前后端、测试）。 |
| `skills/*.md` | 预置 Skill 文本（如思考链、Token 提示、工作区输出约定）。 |

---

## `ide-app/` — 浏览器 IDE 打包工程（npm）

| 文件 | 作用 |
|------|------|
| `package.json` | 依赖与构建脚本。 |
| `src/main.js` | CodeMirror 6 编辑器入口，与 `html[data-theme]` 同步主题。 |
| `README.md` | 子项目说明。 |

（`node_modules/` 为第三方库，不在此逐文件列出。）

---

## `git-worker/` — Node Git 附属服务（simple-git）

| 文件 | 作用 |
|------|------|
| `package.json` | 依赖 `express`、`simple-git`。 |
| `index.js` | 监听 `127.0.0.1`，实现 `/git/*`；由 `cct-cn serve` 尝试自动启动或手动 `npm start`。 |

部署：将仓库内 `git-worker` 复制到 exe 同目录后执行 **`npm install`**。

---

## `repo-sync-app/` — Monaco 仓库同步前端打包（npm）

| 文件 | 作用 |
|------|------|
| `package.json` / `vite.config.js` | 构建到 `ui/repo-sync/`（`base: /repo-sync/`）。 |
| `src/main.js` | 导出 `window.cctRepoSyncMonaco.mountDiffEditor`。 |

构建：`npm install && npm run build`。

---

## `docs/` — 文档

| 文件 | 作用 |
|------|------|
| `billing-api.md` | 计费相关 HTTP API、表结构、调用链说明。 |
| `git-sync-api.md` | 仓库同步：`/api/workspace/git/*`、git-worker、Monaco 产物与配置项。 |
| `project-file-index.md` | 本索引：工程文件职责总览。 |

---

## `.cursor/` — Cursor 编辑器配置（可选）

| 文件 | 作用 |
|------|------|
| `skills/cct-cn-thesis/SKILL.md` | 与本毕设课题对齐的 AI 技能说明（架构、技能、工作区约定等）。 |
| `plans/*.md` | 开发计划或盘点（若有）。 |

---

## 说明

- **权威行为以 `src` 与 `ui` 源为准**；`build/*/ui/` 为 CMake 复制产物，与源同步。
- 若某文件未出现在上表中，多为**构建输出、依赖缓存或本地数据**，可用 `.gitignore` 与仓库实际结构对照。

---

## 附录 A：`build/` 下的 `ALL_BUILD.dir`、`*.dir` 为何常常是空文件夹？

CMake 为 **Visual Studio / MSBuild** 生成工程时，会为某些目标（如 `ALL_BUILD`、`ZERO_CHECK`）生成**规则用的占位目录**（名称常见为 `XXX.dir`）。这些目录主要给**增量编译与依赖路径**使用，里面**不一定存放任何源文件**；**为空是正常现象**，不代表工程损坏。编译日志、tlog 往往在同级子目录（例如 `*.tlog/`、`cct-cn.dir/`）中。

---

## 附录 B：仓库根目录其它文件（若存在）

| 文件/目录 | 作用 |
|-----------|------|
| `需求.txt` | 项目需求记录（文本）。 |
| `论文模板/` | 毕业论文相关模板或文稿（非运行时代码）。 |
| `.cct-cn/config.json` | 本机 `cct-cn init` 后可能出现的配置路径下的 JSON。 |
| `data/` | 本地运行时数据（用户、聊天、用量 JSON 等），通常不提交或仅作示例。 |
