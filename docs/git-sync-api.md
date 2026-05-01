# Git 仓库同步 API（浏览器 → cct-cn → git-worker）

本机场景：`cct-cn serve` 启动后尝试自动拉起 **git-worker**（Node + `simple-git`），监听 **`127.0.0.1:git_worker_port`**（默认 **47821**，见配置文件 `git_worker_port`）。浏览器只调用同源 **`/api/workspace/git/*`**；C++ 校验登录与工作区根路径后，将 **`workspaceRoot`** 注入 JSON 并转发到 git-worker。

## 前置条件

1. 已安装 **Node.js**，且 **`git.exe`** 在 PATH 中（与终端使用 Git 一致，SSH/凭证由本机环境提供）。
2. 在 **可执行文件目录** 下的 **`git-worker/`** 执行过一次 **`npm install`**（CMake `POST_BUILD` 会从源码复制 `git-worker`，但不会替你安装依赖）。
3. `config.json` 中 **`workspace_root`** 指向的本机目录是一个 **Git 仓库**（含 `.git`）。

## HTTP：浏览器可调用的路径

均为 **`POST`**，请求体为 JSON（除下方说明外字段可选）。**不要**在浏览器侧传入 `workspaceRoot`（由服务端注入）。

可选字段 **`repo_rel`**：相对于配置项 **`workspace_root`** 的子目录（使用 `/` 分隔，勿以 `/` 开头），Git 命令在该子目录执行；须为已存在的目录且仍在工作区内。留空或不传则使用工作区根目录。

| 路径 | 请求体示例 | 说明 |
|------|------------|------|
| `/api/workspace/git/status` | `{}` | 返回分支、upstream、ahead/behind、变更文件列表等 |
| `/api/workspace/git/diff-sides` | `{"path":"相对路径"}` | 返回 Monaco 两侧文本：`original`（:2 或 HEAD）、`modified`（:3 或工作区） |
| `/api/workspace/git/stage` | `{"paths":["a.cpp"]}` | `git add` |
| `/api/workspace/git/unstage` | `{"paths":["a.cpp"]}` | `git reset HEAD --` |
| `/api/workspace/git/commit` | `{"message":"说明"}` | `git commit` |
| `/api/workspace/git/push` | `{"remote":"origin","branch":"main"}` | `branch` 可省略（当前分支） |
| `/api/workspace/git/pull` | 同上 | 成功 `200`；存在冲突时 **`409`**，`conflict: true`，`conflicts` 为路径列表 |
| `/api/workspace/git/conflicts-detail` | `{}` | 每个冲突文件的 `markedContent`、`ours`、`theirs` |
| `/api/workspace/git/write-file` | `{"path":"…","content":"…"}` | 写入工作区文件（UTF-8） |
| `/api/workspace/git/add` | `{"paths":["…"]}` | 暂存（解决冲突后） |
| `/api/workspace/git/commit-merge` | `{"message":"…"}` | 完成合并提交 |
| `/api/workspace/git/merge-abort` | `{}` | `git merge --abort` |

## 内部：C++ → git-worker

- **URL**：`http://127.0.0.1:<git_worker_port>/git/<子路径>`，例如 `/git/status`。
- **方法**：`POST`，`Content-Type: application/json`。
- **体**：已含 **`workspaceRoot`**（服务端注入后的完整 UTF-8 绝对路径）。
- **超时**：push/pull **300s**，其余 **120s**。

## 前端资源

- Monaco 打包：`repo-sync-app` → **`npm run build`** → 产出 **`ui/repo-sync/`**。
- 面板脚本：**`ui/js/repo-sync/repo-sync-main.js`**，样式 **`ui/css/repo-sync.css`**。

## 配置项（`config.json`）

- **`git_worker_port`**：整数，默认 `47821`。
- **`git_worker_autostart`**：布尔，默认 `true`。
