# CCT-CN：汉化版的Claude Code Templates代码生成系统的设计与实现

## 一、项目简介

汉化版的Claude Code Templates 是一个面向中文软件开发场景的大模型代码辅助系统。系统以 C++17 为主体实现本地 **Web 工作台**：用户通过浏览器完成登录、聊天、组件管理、Web IDE、代码检测、Git 仓库同步、Token 计费与数据分析等操作；后台负责大模型调用、会话与权限、以及与 SQL Server、本机 Git 辅助服务的对接。

本项目参考 `claude-code-templates` 的模板化思想，但并非简单封装外部工具，而是结合毕业设计需求重新实现了 Web 服务、用户认证、聊天会话、Agent/Skill 组件、代码检测、仓库同步、Token 计费和数据分析等模块。系统在 Windows 环境下以单一可执行程序 `cct-cn.exe` 启动本地 HTTP 服务，托管静态前端并实现业务 API。

项目主要解决以下问题：

1. 中文开发者使用大模型代码工具时，英文提示词门槛较高，需求表达与工程上下文难以稳定组织；
2. 普通聊天式代码生成缺少可复用的角色约束、任务流程和工程写回规范；
3. 本地项目文件、Git 仓库、代码检测、聊天历史和模型用量通常分散在多个工具中，缺少统一入口；
4. 毕业设计场景下需要一个结构清晰、可演示、可扩展且具备工程完整性的 AI 代码辅助系统。

---

## 二、主要功能

系统目前实现的主要功能（均以 **Web 工作台** 为载体）包括：

1. 支持中文需求输入，在聊天中用语言描述开发任务；
2. 支持在组件工作室中维护 **Agent / Skill / Command**，把常用角色与流程固定为 Markdown；
3. 支持接入 Claude、智谱 GLM 等模型并完成流式对话；
4. 支持通过约定格式（如回复中的 `CCT_WORKSPACE:`）将结构化结果写回授权工作区；
5. 提供登录、注册与基于 Cookie 的会话管理；
6. 提供历史会话管理与模型选择；
7. 提供代码检测功能，对选定文件给出审查意见与修改建议；
8. 提供 Web IDE，在浏览器内查看与编辑工作区文件；
9. 提供 Git 仓库同步界面（状态、差异、提交、推送、拉取、冲突处理等）；
10. 提供 SQL Server 持久化、Token 计费档位与数据分析展示。

---

## 三、系统技术路线

本系统主要由几种语言完成：**C++** 负责本地 Web 后台服务与大模型等业务逻辑；**HTML、CSS、JavaScript** 负责网页界面；**Node.js** 辅助执行与本机 Git 的交互（`git-worker`）；**SQL Server** 保存用户、会话、组件与计费等持久化数据。

| 技术或模块 | 作用 |
|---|---|
| C++17 | Web 后台服务入口、HTTP 路由、大模型调用、用户认证等业务逻辑 |
| CMake | 管理工程构建、目标链接和资源复制 |
| WinHTTP | Windows 下调用 Anthropic / 智谱等大模型 HTTP API |
| bcrypt / PBKDF2 | 用户密码哈希和认证相关密码学辅助 |
| ODBC / SQL Server | 保存用户、聊天、组件、计费与数据分析相关数据 |
| HTML / CSS / JavaScript | 实现登录页、主工作台、聊天、设置、代码检测、仓库同步和数据分析界面 |
| CodeMirror 6 | 实现浏览器内嵌代码编辑器 |
| Monaco Editor | 实现浏览器里的代码差异对比窗口，效果类似 VS Code 的文件对比 |
| Node.js / Express | 实现本机 Git 辅助服务 `git-worker` |
| simple-git | 调用本机 Git 命令，供仓库同步功能使用 |
| Markdown | 承载组件工作室中的 Agent、Skill、Command 等内容 |

系统的核心思路是：通过 Agent/Skill（及 Command）把用户需求规范化，再结合工作区上下文发起大模型请求，并在授权路径下解析结构化写回。浏览器只访问 C++ 服务暴露的同源 API；Git 操作由服务端校验会话与工作区后，转发至本机 `git-worker`。

---

## 四、项目结构

项目主要目录结构如下：

```text
cct-cn-cpp/
├── CMakeLists.txt
├── README.md
├── README1.md
├── src/
│   ├── main.cpp
│   ├── context/
│   ├── generator/
│   ├── llm/
│   ├── storage/
│   ├── template/
│   ├── util/
│   └── web/
├── ui/
│   ├── app.html
│   ├── index.html
│   ├── css/
│   ├── ide/
│   ├── js/
│   └── repo-sync/
├── templates/
│   └── zh/
├── component-templates/
│   ├── agents/
│   └── skills/
├── git-worker/
├── ide-app/
├── repo-sync-app/
├── scripts/
│   └── sql/
└── docs/
```

各目录说明如下：

| 文件或目录 | 说明 |
|---|---|
| `CMakeLists.txt` | CMake 工程入口，定义 `cct-cn`、`cct-storage`、`ui-sync` 等目标，并在 Windows 下复制 UI、Git Worker 和 DLL |
| `src/main.cpp` | 程序入口：提供 **`init`**（生成配置文件）、**`serve`**（启动 Web 服务） |
| `src/util/` | 通用工具模块，包括配置读写、极简 JSON 解析、路径解析、UTF-8 字符串处理和组件名称校验 |
| `src/llm/` | 大模型调用模块，封装 Claude、智谱 GLM 和 Mock 客户端 |
| `src/context/`、`src/template/`、`src/generator/` | 服务端内部模块（与仓库中历史链路相关；**使用与说明以 Web 工作台为准**） |
| `src/web/` | Windows 下的 HTTP 服务、路由、认证、Cookie、用户存储和 Git Worker 转发 |
| `src/storage/` | 数据存储模块，主要对接 SQL Server 中的用户、聊天、组件和计费数据 |
| `ui/` | Web 前端源码与静态资源，由 C++ HTTP 服务托管 |
| `templates/zh/` | 仓库内 Markdown 模板资源（**本产品文档不介绍命令行方式使用**） |
| `component-templates/` | Web 组件工作室使用的默认 Agent / Skill 等模板 |
| `git-worker/` | Node.js Git 附属服务，负责执行本机 Git 操作 |
| `ide-app/` | CodeMirror 6 Web IDE 子工程，构建产物写入 `ui/ide/` |
| `repo-sync-app/` | Git 差异对比页面子工程，构建产物写入 `ui/repo-sync/` |
| `scripts/sql/` | SQL Server 建表和增量升级脚本 |
| `docs/` | 项目补充文档，包括工程文件索引、计费 API、Git 同步 API 等 |

---

## 五、运行环境

本项目完整功能主要面向 Windows 环境，基础运行要求如下：

| 环境 | 要求 |
|---|---|
| 操作系统 | Windows 10 / Windows 11 |
| C++ 编译器 | Visual Studio 2019+ 或其他支持 C++17 的 MSVC 工具链 |
| 构建工具 | CMake 3.16 及以上 |
| Web API | Windows WinHTTP，CMake 已配置链接 |
| Git 同步 | 本机已安装 Git，并保证 `git.exe` 位于 PATH |
| Node.js | Git 辅助服务、Web IDE 和代码对比页面构建时需要 Node.js 与 npm |
| 数据库 | 使用 SQL Server 保存系统数据，需要准备数据库并配置 ODBC 连接串 |

---

## 六、构建与部署

### 1. 构建 C++ 主程序

在项目根目录执行：

```powershell
cd d:\claudecode\cct-cn-cpp
cmake -B build -G "Visual Studio 16 2019" -A x64
cmake --build build --config Release
```

构建完成后，主程序一般位于：

```text
build\Release\cct-cn.exe
```

Windows 下 CMake 会在构建后将以下资源复制到可执行文件同级目录：

```text
build\Release\
├── cct-cn.exe
├── cct-storage.dll
├── ui/
└── git-worker/
```

### 2. 安装 Git Worker 依赖

若需要使用仓库同步功能，需要在可执行文件旁的 `git-worker` 目录安装依赖：

```powershell
cd d:\claudecode\cct-cn-cpp\build\Release\git-worker
npm install
```

安装完成后，`cct-cn serve` 会根据配置尝试自动启动 `git-worker`。也可以手动启动：

```powershell
npm start
```

### 3. 构建 Web IDE 资源

如需重新构建 CodeMirror IDE：

```powershell
cd d:\claudecode\cct-cn-cpp\ide-app
npm install
npm run build
```

该命令会生成或更新：

```text
ui\ide\ide.js
```

### 4. 构建仓库同步对比页面资源

Monaco Editor 是微软开源的浏览器代码编辑器，也是 VS Code 编辑器体验的基础。这里的“Monaco 资源”指的是仓库同步页面中用于显示 Git 文件差异对比的前端文件。

如需重新构建该页面资源：

```powershell
cd d:\claudecode\cct-cn-cpp\repo-sync-app
npm install
npm run build
```

该命令会生成或更新：

```text
ui\repo-sync\
```

### 5. 同步静态 UI 到可执行文件目录

如果只修改了 `ui/*.html`、`ui/css/*.css` 或 `ui/js/*.js`，可执行：

```powershell
cmake --build build --target ui-sync --config Release
```

---

## 七、配置说明

### 1. 初始化配置文件

首次使用前，在终端执行一次 **`init`**，仅用于生成 `.cct-cn\config.json`（不是要使用的“命令行代码生成”，而是写入 Web 与大模型所需的配置）：

```powershell
.\build\Release\cct-cn.exe init
```

默认会生成：

```text
.cct-cn\config.json
```

也可以指定配置文件路径：

```powershell
.\build\Release\cct-cn.exe init --config .\.cct-cn\config.json
```

随后请用文本编辑器打开 `config.json`，至少配置：**SQL Server ODBC 连接串**、大模型 **`api_key`**（若不用 Mock）、以及 **`workspace_root`**（或启动 `serve` 时用 `--workspace` 覆盖）。

### 2. 配置字段

配置文件主要字段如下：

| 字段 | 说明 |
|---|---|
| `llm_provider` | 大模型提供商，支持 `anthropic` 和 `zhipu` |
| `api_key` | 大模型 API Key |
| `model` | 模型名称，如 `claude-sonnet-4-20250514` 或 `glm-4` |
| `api_host` | API 主机名，如 `api.anthropic.com` 或 `open.bigmodel.cn` |
| `api_path` | API 路径，如 `/v1/messages` 或 `/api/paas/v4/chat/completions` |
| `max_context_chars` | 上下文扫描或拼接时的字符上限相关配置 |
| `max_tokens` | 单次模型输出 Token 上限 |
| `use_mock` | 是否使用 Mock 模式，开发和演示时可设为 `true` |
| `llm_daily_call_limit` | 免费档单日调用次数限制，`0` 表示不限制 |
| `workspace_root` | Web IDE 和仓库同步使用的本机工作区根目录 |
| `git_worker_port` | Git Worker 本机监听端口，默认 `47821` |
| `git_worker_autostart` | `serve` 启动时是否尝试自动拉起 Git Worker |
| `sql_odbc_connection_string` | SQL Server ODBC 连接串，Web 正常运行所必需 |

---

## 八、Web 工作台使用说明

### 1. 启动 Web 服务

在已完成 **七、配置说明**（含数据库连接与大模型密钥等）后执行：

```powershell
.\build\Release\cct-cn.exe serve --port 8787 --workspace d:\claudecode\cct-cn-cpp
```

启动后在浏览器访问：

```text
http://127.0.0.1:8787/
```

`serve` 常用参数如下：

| 参数 | 说明 |
|---|---|
| `--port` | HTTP 服务端口，默认 `8787` |
| `--workspace` | 覆盖配置文件中的 `workspace_root` |
| `-c` / `--config` | 指定配置文件路径 |

可选：查看程序版本：`.\build\Release\cct-cn.exe --version`；需要帮助：`.\build\Release\cct-cn.exe --help`（界面说明以本节 Web 为主的叙述为准）。

### 2. Web 主要页面

Web 工作台主要包括：

| 功能区 | 说明 |
|---|---|
| 登录 / 注册 | 创建本地用户并建立会话 Cookie |
| 聊天页面 | 发送中文需求、选择模型、查看历史会话和流式响应 |
| 组件工作室 | 编辑用户自己的 Agent、Skill、Command 等 Markdown 组件 |
| Web IDE | 在浏览器中查看和编辑工作区文件 |
| 代码检测 | 选择文件和检测维度，调用模型生成审查意见和修复建议 |
| 仓库同步 | 查看 Git 状态、差异对比、暂存、提交、推送、拉取和处理冲突 |
| 数据分析 | 展示调用次数、Token 消耗、额度剩余和相关使用数据 |
| 设置 / 订阅 | 管理主题、个人信息、套餐和模拟支付 |

### 3. Git 仓库同步前置条件

仓库同步功能需要满足：

1. 本机已安装 Git，且 `git.exe` 位于 PATH；
2. `workspace_root` 或 `--workspace` 指向一个包含 `.git` 的仓库；
3. `build\Release\git-worker` 中已经执行过 `npm install`；
4. `git_worker_port` 没有被其他程序占用。

浏览器不会直接执行 Git 命令，而是调用同源 `/api/workspace/git/*` 接口。C++ 服务校验登录状态和工作区路径后，将请求转发到本机 `git-worker`。

---

## 九、核心流程说明

### 1. Web 聊天流程

```text
用户在浏览器输入中文需求
↓
前端携带会话、模型、Agent、Skill 等信息请求后端
↓
后端校验登录状态和 Token 额度
↓
读取用户组件与历史消息
↓
将 Agent / Skill（及 Command）注入本轮模型上下文
↓
调用大模型并返回流式响应
↓
保存聊天记录和本轮 Token 消耗
```

### 2. Agent / Skill / Command 组件流程

```text
用户在组件工作室创建或编辑 Markdown 组件
↓
组件内容保存到 SQL Server 数据库
↓
聊天请求指定对应组件字段
↓
服务端在调用模型前读取组件内容并注入提示词
```

### 3. 工作区写回流程

Web 路径下，多文件写回可依赖模型在回复末尾输出的 **`CCT_WORKSPACE:`** JSON。系统据此将多个文件写入授权工作区，并校验路径必须位于 `workspace_root` 内。

```text
模型输出代码修改结果
↓
回复末尾包含 CCT_WORKSPACE JSON
↓
服务端解析目标文件路径与内容
↓
校验路径必须位于 workspace_root 内
↓
写入或更新工作区文件
```

### 4. Git 仓库同步流程

```text
浏览器调用 /api/workspace/git/status 等接口
↓
C++ 服务校验用户登录与 workspace_root
↓
C++ 服务注入 workspaceRoot 并转发给 git-worker
↓
git-worker 使用 simple-git 执行本机 Git 命令
↓
结果返回前端并展示状态、差异或冲突信息
```

### 5. Token 计费流程

```text
用户发起聊天或代码检测
↓
后端读取用户套餐和当月剩余额度
↓
额度不足时返回 429
↓
额度充足时调用大模型
↓
调用成功后累计本轮 Token 消耗
↓
数据分析和订阅页面展示最新额度状态
```

---

## 十、数据库设计

系统使用 SQL Server 保存用户、聊天、组件、计费和使用记录等数据。运行前需要先执行建表脚本，并在配置文件中填写 `sql_odbc_connection_string`。

主要持久化对象如下：

| 数据对象 | SQL Server 表 | 说明 |
|---|---|---|
| 用户账号 | `Users` | 保存用户名、密码哈希、注册时间等信息 |
| 用户偏好 | `UserPreferences` | 保存主题、个人设置等信息 |
| 聊天线程 | `ChatThreads` | 保存每个用户的聊天会话 |
| 聊天消息 | `ChatMessages` | 保存每个会话中的具体消息 |
| Agent / Skill 组件 | `Components` | 保存用户自定义的角色模板和任务流程 |
| 订阅与 Token | `UserBilling` | 保存套餐、额度、已消耗 Token 和计费周期 |

SQL Server 脚本位于：

```text
scripts\sql\schema.sql
scripts\sql\schema_incremental_userbilling.sql
```

---

## 十一、系统测试与验证

当前项目以手动功能验证和演示验证为主，主要测试内容如下：

| 测试功能 | 测试方式 | 预期结果 |
|---|---|---|
| 工程构建 | `cmake --build build --config Release` | 成功生成 `cct-cn.exe` 和 `cct-storage.dll` |
| 配置文件生成 | `cct-cn.exe init` | 成功生成 `.cct-cn\config.json` |
| Web 启动 | `cct-cn.exe serve --port 8787`（需有效 SQL 连接串） | 浏览器可访问本地页面 |
| 用户注册登录 | 在浏览器注册并登录 | 成功进入主工作台 |
| 聊天会话 | 发送中文需求 | 成功返回模型回复并保存历史 |
| 组件工作室 | 创建 Agent / Skill / Command | 组件内容可保存并在聊天中使用 |
| 代码检测 | 选择文件并运行检测 | 返回审查意见或修复建议 |
| Git 状态 | 打开仓库同步页面 | 正确显示分支、变更和差异 |
| Git 提交 | 暂存文件并提交 | 本地仓库生成新提交 |
| Token 额度 | 发起模型调用或开通套餐 | 正确更新额度和消耗记录 |
| 数据库存储 | 配置 ODBC 并执行 SQL 脚本 | 用户、聊天、组件、计费数据写入 SQL Server |

---

## 十二、系统特点

1. **中文本地化交互**  
   Web 界面与组件内容以中文为主，降低使用门槛。

2. **提示词组件化**  
   Agent、Skill、Command 以 Markdown 维护，便于复用角色与流程。

3. **一体化工作台**  
   在同一本地服务中集成聊天、编辑、检测、Git 同步与用量分析，减少工具切换。

4. **支持国内外模型接入**  
   系统支持 Anthropic Claude 与智谱 GLM，并可使用 Mock 便于演示。

5. **与工作区结合的写回**  
   Web 可通过 `CCT_WORKSPACE:` 等约定，在授权目录内结构化落地修改。

6. **本机 Git 工作流集成**  
   仓库同步界面配合 `git-worker` 封装本机 Git 操作，服务端统一鉴权与路径约束。

7. **使用 SQL Server 统一保存数据**  
   用户账号、聊天记录、组件内容与 Token 额度等核心数据保存在 SQL Server，便于管理与扩展。

8. **具备计费与数据分析雏形**  
   订阅档位、Token 配额、消耗记录与数据分析页面，支撑毕设演示与后续扩展话题。

---

## 十三、后续改进方向

后续可以从以下方面继续完善系统：

1. 完善多模型配置管理，支持更多国产大模型和 OpenAI 兼容接口；
2. 增强工作区上下文组织策略，细化文件选择与 Token 预算控制；
3. 完善 `CCT_WORKSPACE:` 写回协议，增加写入预览、冲突检测和回滚机制；
4. 将模拟支付替换为真实微信或支付宝支付回调；
5. 增加管理员后台，用于查看用户、套餐、调用量和系统日志；
6. 优化 Git 冲突处理体验，提供更完整的三方合并视图；
7. 增加插件机制，使 Agent、Skill、Command 与代码检测规则可按项目分发；
8. 增强安全控制，例如工作区访问白名单、API 限流和操作审计。

---

## 十四、项目总结

汉化版的 Claude Code Templates 代码生成系统是一个面向中文开发场景的大模型代码辅助系统。项目以 C++17 实现本地 Web 服务与核心业务逻辑，在浏览器工作台中提供聊天、组件、代码检测、仓库同步、计费和数据分析等能力，形成较完整的毕业设计工程原型。

通过本项目，可以系统性地理解大模型代码工具在中文场景下的产品与工程要点，包括提示词组织、会话与持久化、模型 API 调用、结构化输出与工作区集成、以及与本地 Git 工作流的结合。
