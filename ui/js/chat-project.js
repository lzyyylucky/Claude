/**
 * 「新项目」面板：欢迎卡片打开文件/文件夹/服务端工作区，左侧资源管理器 + 代码预览。
 */
(function () {
  const state = {
    mode: "none",
    explorerVisible: false,
    openRelPath: "",
    openLabel: "",
    lastFileHandle: null,
    lastDirHandle: null,
    webkitTree: null,
    serverRootLabel: "",
    saveKind: "none",
    /** 服务端资源管理器当前列表作用域：""=workspace 根；否则为相对 workspace_root 的目录前缀（对话锚点） */
    serverTreeScope: "",
  };

  /** AI 本轮预览：未点「保存全部」前不写盘 */
  let aiReviewSession = null;

  /** 服务端树定时刷新（与磁盘变更双向对齐） */
  let serverTreePollTimer = null;

  function joinRel(parent, name) {
    if (!parent) return name;
    return parent.replace(/\/+$/, "") + "/" + name;
  }

  /** 工作区内相对路径规范化（与树、writes 一致）；含 .. 或绝对路径成分则返回空串 */
  function normWorkspacePath(p) {
    const s = String(p || "")
      .replace(/\\/g, "/")
      .replace(/^\.\//, "")
      .replace(/^\/+/, "");
    const segs = s.split("/").filter(Boolean);
    for (let i = 0; i < segs.length; i++) {
      if (segs[i] === "." || segs[i] === "..") return "";
    }
    return segs.join("/");
  }

  /** 对话绑定的服务端相对目录锚点（localStorage），换线程时在左侧树只聚焦该目录 */
  function anchorStorageKey(threadId) {
    return "cct_thread_workspace_anchor:" + encodeURIComponent(String(threadId || "main"));
  }

  function getStoredAnchorRelForThread(threadId) {
    try {
      const raw = localStorage.getItem(anchorStorageKey(threadId));
      if (!raw) return "";
      const o = JSON.parse(raw);
      return normWorkspacePath(o && o.rel ? String(o.rel) : "");
    } catch (_) {
      return "";
    }
  }

  function setStoredAnchorRelForThread(threadId, relDir) {
    const norm = normWorkspacePath(relDir);
    if (!norm || !threadId) return;
    try {
      localStorage.setItem(anchorStorageKey(threadId), JSON.stringify({ rel: norm, ts: Date.now() }));
    } catch (_) {}
  }

  /**
   * 从相对路径推导会话目录锚点：指向文件则取其所在目录（多级路径保留，如 test/clock/a.js → test/clock）；
   * 纯目录路径则原样返回；工作区根下单文件则返回空。
   */
  function deriveAnchorRelFromOpenPath(relPath) {
    const n = normWorkspacePath(relPath);
    if (!n) return "";
    const parts = n.split("/").filter(Boolean);
    if (!parts.length) return "";
    const last = parts[parts.length - 1];
    const looksFile = /\.[a-zA-Z0-9]{1,15}$/.test(last);
    if (looksFile) {
      if (parts.length < 2) return "";
      return parts.slice(0, -1).join("/");
    }
    return parts.join("/");
  }

  function parentScopePath(scope) {
    const parts = normWorkspacePath(scope).split("/").filter(Boolean);
    parts.pop();
    return parts.join("/");
  }

  function hideServerExplorerCtxMenu() {
    const m = document.getElementById("chat-explorer-ctx");
    if (m) m.hidden = true;
  }

  function ensureServerExplorerCtxMenu() {
    let m = document.getElementById("chat-explorer-ctx");
    if (m) return m;
    m = document.createElement("div");
    m.id = "chat-explorer-ctx";
    m.className = "chat-explorer-ctx";
    m.hidden = true;
    m.innerHTML =
      '<button type="button" class="chat-explorer-ctx-btn" data-act="file">新建文件…</button>' +
      '<button type="button" class="chat-explorer-ctx-btn" data-act="dir">新建文件夹…</button>' +
      '<button type="button" class="chat-explorer-ctx-btn chat-explorer-ctx-btn--danger" data-act="del" hidden>删除…</button>';
    document.body.appendChild(m);
    m.addEventListener("click", (e) => {
      const btn = e.target.closest("button[data-act]");
      if (!btn) return;
      const parentRel = m.dataset.parentRel || "";
      const act = btn.getAttribute("data-act");
      const delTarget = m.dataset.deleteTarget || "";
      const delKind = m.dataset.deleteKind || "";
      hideServerExplorerCtxMenu();
      if (act === "file") void serverWorkspaceCreateFile(parentRel);
      else if (act === "dir") void serverWorkspaceCreateFolder(parentRel);
      else if (act === "del") void serverWorkspaceDeleteEntry(delTarget, delKind);
    });
    return m;
  }

  function showServerExplorerCtxMenu(clientX, clientY, parentRelForCreate, deleteTarget, deleteKind) {
    const menu = ensureServerExplorerCtxMenu();
    menu.dataset.parentRel = parentRelForCreate || "";
    menu.dataset.deleteTarget = deleteTarget || "";
    menu.dataset.deleteKind = deleteKind || "";
    const delBtn = menu.querySelector('[data-act="del"]');
    if (delBtn) delBtn.hidden = !deleteTarget;
    menu.style.left = Math.min(clientX, window.innerWidth - 200) + "px";
    menu.style.top = Math.min(clientY, window.innerHeight - (!deleteTarget ? 88 : 120)) + "px";
    menu.hidden = false;
  }

  async function serverWorkspaceDeleteEntry(relPath, kind) {
    const p = normWorkspacePath(relPath || "");
    if (!p) return;
    const label = kind === "dir" ? "目录「" + p + "」及其内容" : "文件「" + p + "」";
    if (!window.confirm("确定删除 " + label + "？")) return;
    try {
      const res = await fetch("/api/workspace/file?path=" + encodeURIComponent(p), {
        method: "DELETE",
        credentials: "include",
      });
      const data = await res.json().catch(() => ({}));
      if (!res.ok || !data.ok) {
        window.alert((data && data.error) || "删除失败 HTTP " + res.status);
        return;
      }
      if (state.openRelPath && normWorkspacePath(state.openRelPath) === p) closeFilePanel();
      renderServerRoot();
    } catch (err) {
      window.alert(err && err.message ? err.message : "删除失败");
    }
  }

  /** 捕获阶段拦截原生右键菜单（整个资源管理器区域内） */
  function onChatExplorerProjectContextCapture(e) {
    if (state.mode !== "server" || !state.explorerVisible) return;
    const tree = document.getElementById("chat-explorer-tree");
    if (!tree || !tree.contains(e.target)) return;
    onChatExplorerTreeContextMenuCapture(e);
  }

  function onChatExplorerTreeContextMenuCapture(e) {
    if (state.mode !== "server" || !state.explorerVisible) return;
    const tree = document.getElementById("chat-explorer-tree");
    if (!tree || !tree.contains(e.target)) return;
    e.preventDefault();
    e.stopPropagation();
    const listedScope = normWorkspacePath(state.serverTreeScope || "");
    const row = e.target.closest(".chat-tree-row");
    let parentRel = listedScope || "";
    let deleteTarget = "";
    let deleteKind = "";
    if (!row) {
      showServerExplorerCtxMenu(e.clientX, e.clientY, parentRel, "", "");
      return;
    }
    if (row.classList.contains("chat-tree-scope-up")) {
      showServerExplorerCtxMenu(e.clientX, e.clientY, listedScope, "", "");
      return;
    }
    const kind = row.dataset.treeKind;
    const fullPath = row.dataset.treeRel || "";
    if (kind === "dir") {
      parentRel = fullPath;
      deleteTarget = fullPath;
      deleteKind = "dir";
    } else if (kind === "file") {
      parentRel = parentScopePath(fullPath);
      deleteTarget = fullPath;
      deleteKind = "file";
    }
    showServerExplorerCtxMenu(e.clientX, e.clientY, parentRel, deleteTarget, deleteKind);
  }

  async function serverWorkspaceCreateFile(parentRel) {
    const name = window.prompt("新建文件名（相对当前目录，含扩展名）", "untitled.txt");
    if (!name || !String(name).trim()) return;
    const base = normWorkspacePath(parentRel || "");
    const leaf = String(name).trim().replace(/\\/g, "/");
    const path = base ? base + "/" + leaf.replace(/^\/+/, "") : leaf.replace(/^\/+/, "");
    if (!normWorkspacePath(path)) {
      window.alert("路径不合法");
      return;
    }
    const { r, data } = await apiJson("/api/workspace/file", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      credentials: "include",
      body: JSON.stringify({ path, content: "" }),
    });
    if (!data || !data.ok) {
      window.alert((data && data.error) || "创建失败 " + (r && r.status != null ? r.status : ""));
      return;
    }
    renderServerRoot();
  }

  async function serverWorkspaceCreateFolder(parentRel) {
    const name = window.prompt("新建文件夹名", "");
    if (!name || !String(name).trim()) return;
    const base = normWorkspacePath(parentRel || "");
    const leaf = String(name).trim().replace(/\\/g, "/").replace(/\/+$/, "");
    const path = base ? base + "/" + leaf.replace(/^\/+/, "") : leaf.replace(/^\/+/, "");
    if (!normWorkspacePath(path)) {
      window.alert("路径不合法");
      return;
    }
    const { r, data } = await apiJson("/api/workspace/mkdir", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      credentials: "include",
      body: JSON.stringify({ path }),
    });
    if (!data || !data.ok) {
      window.alert((data && data.error) || "创建失败 " + (r && r.status != null ? r.status : ""));
      return;
    }
    renderServerRoot();
  }

  function attachServerTreeRowCtx(row, listedScope, kind, fullPath) {
    row.dataset.treeKind = kind;
    row.dataset.treeRel = fullPath || "";
  }

  function startServerTreePolling() {
    if (serverTreePollTimer) return;
    serverTreePollTimer = window.setInterval(() => {
      if (state.mode === "server" && state.explorerVisible) renderServerRoot();
    }, 4000);
  }

  function stopServerTreePolling() {
    if (serverTreePollTimer) {
      window.clearInterval(serverTreePollTimer);
      serverTreePollTimer = null;
    }
  }

  function setExplorerScopeFromThread(threadId) {
    state.serverTreeScope = getStoredAnchorRelForThread(threadId || "main") || "";
  }

  async function persistThreadAnchor(threadId, relDir) {
    const tid = String(threadId || "").trim() || "main";
    const norm = normWorkspacePath(relDir || "");
    if (!norm) return "";
    setStoredAnchorRelForThread(tid, norm);
    try {
      await apiJson("/api/chat/thread-anchor", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ thread_id: tid, workspace_anchor: norm }),
      });
    } catch (_) {}
    return norm;
  }

  async function enterServerScope(relDir, options) {
    const scope = normWorkspacePath(relDir || "");
    if (!scope) return;
    const q = "?path=" + encodeURIComponent(scope);
    const { r, data } = await apiJson("/api/workspace/list" + q);
    if (!data || !data.ok) {
      window.alert((data && data.error) || "列出目录失败 " + (r && r.status != null ? r.status : ""));
      return;
    }
    state.serverTreeScope = scope;
    if (!options || options.persist !== false) {
      const tid = typeof window.cctGetActiveThreadId === "function" ? window.cctGetActiveThreadId() : "main";
      await persistThreadAnchor(tid, scope);
    }
    renderServerRoot();
    const base = state.serverRootLabel ? "服务端 · " + state.serverRootLabel : "服务端工作区";
    setStatus(base + " · 本对话目录：" + scope + "（列表已限定于此文件夹）");
  }

  async function hydrateThreadAnchorFromServer(threadId) {
    const tid = String(threadId || "").trim();
    if (!tid) return "";
    const { data } = await apiJson("/api/chat/thread?id=" + encodeURIComponent(tid));
    const anch = data && data.ok ? normWorkspacePath(data.workspaceAnchor || "") : "";
    if (anch) setStoredAnchorRelForThread(tid, anch);
    return anch || getStoredAnchorRelForThread(tid);
  }

  function setStatus(text) {
    const el = document.getElementById("chat-explorer-status");
    if (el) el.textContent = text || "";
  }

  function clearTreeSelection() {
    document.querySelectorAll(".chat-explorer-tree .chat-tree-row.is-selected").forEach((n) => {
      n.classList.remove("is-selected");
    });
  }

  function selectRow(row) {
    clearTreeSelection();
    if (row) row.classList.add("is-selected");
  }

  function applyStoredSplitWidths() {
    const wrap = document.getElementById("gpt-chat-wrap");
    if (!wrap) return;
    try {
      const tw = localStorage.getItem("cct_split_tree");
      const aw = localStorage.getItem("cct_split_ai");
      if (tw) wrap.style.setProperty("--cct-tree-w", parseInt(tw, 10) + "px");
      if (aw) wrap.style.setProperty("--cct-ai-w", parseInt(aw, 10) + "px");
    } catch (_) {}
  }

  function persistSplitWidths() {
    const wrap = document.getElementById("gpt-chat-wrap");
    if (!wrap) return;
    try {
      const cs = getComputedStyle(wrap);
      const t = cs.getPropertyValue("--cct-tree-w").trim();
      const a = cs.getPropertyValue("--cct-ai-w").trim();
      const tn = parseInt(t, 10);
      const an = parseInt(a, 10);
      if (Number.isFinite(tn)) localStorage.setItem("cct_split_tree", String(tn));
      if (Number.isFinite(an)) localStorage.setItem("cct_split_ai", String(an));
    } catch (_) {}
  }

  function syncCodeEmptyVisibility() {
    const panel = document.getElementById("chat-file-panel");
    const empty = document.getElementById("chat-code-empty");
    if (!empty) return;
    if (!state.explorerVisible) {
      empty.hidden = true;
      return;
    }
    empty.hidden = !!(panel && !panel.hidden);
  }

  function setupSplitDrag() {
    const wrap = document.getElementById("gpt-chat-wrap");
    const s1 = document.getElementById("split-tree-code");
    const s2 = document.getElementById("split-code-ai");
    const explorer = document.getElementById("chat-project-explorer");
    const desk = document.getElementById("chat-desk-main");
    let drag = null;
    s1?.addEventListener("mousedown", (e) => {
      if (!wrap || !wrap.classList.contains("gpt-chat-wrap--workspace")) return;
      e.preventDefault();
      drag = {
        kind: "tree",
        startX: e.clientX,
        startTree: explorer ? explorer.getBoundingClientRect().width : 240,
      };
      s1.classList.add("is-dragging");
    });
    s2?.addEventListener("mousedown", (e) => {
      if (!wrap || !wrap.classList.contains("gpt-chat-wrap--workspace")) return;
      e.preventDefault();
      drag = {
        kind: "ai",
        startX: e.clientX,
        startAi: desk ? desk.getBoundingClientRect().width : 360,
      };
      s2.classList.add("is-dragging");
    });
    window.addEventListener("mousemove", (e) => {
      if (!drag || !wrap) return;
      const dx = e.clientX - drag.startX;
      if (drag.kind === "tree") {
        let w = drag.startTree + dx;
        w = Math.max(160, Math.min(520, w));
        wrap.style.setProperty("--cct-tree-w", w + "px");
      } else {
        let w = drag.startAi - dx;
        w = Math.max(260, Math.min(560, w));
        wrap.style.setProperty("--cct-ai-w", w + "px");
      }
    });
    window.addEventListener("mouseup", () => {
      if (!drag) return;
      persistSplitWidths();
      s1?.classList.remove("is-dragging");
      s2?.classList.remove("is-dragging");
      drag = null;
    });
  }

  function showExplorer() {
    const root = document.getElementById("app-root");
    const wrap = document.getElementById("gpt-chat-wrap");
    const aside = document.getElementById("chat-project-explorer");
    const s1 = document.getElementById("split-tree-code");
    const s2 = document.getElementById("split-code-ai");
    const codeCol = document.getElementById("chat-code-column");
    if (!wrap || !aside) return;
    applyStoredSplitWidths();
    aside.hidden = false;
    if (s1) s1.hidden = false;
    if (s2) s2.hidden = false;
    if (codeCol) codeCol.hidden = false;
    wrap.classList.add("gpt-chat-wrap--workspace");
    if (root) root.classList.add("gpt-workspace-open");
    state.explorerVisible = true;
    syncCodeEmptyVisibility();
    if (typeof cctChat !== "undefined" && cctChat.hideWelcome) cctChat.hideWelcome();
  }

  function hideExplorerUi() {
    const root = document.getElementById("app-root");
    const wrap = document.getElementById("gpt-chat-wrap");
    const aside = document.getElementById("chat-project-explorer");
    const s1 = document.getElementById("split-tree-code");
    const s2 = document.getElementById("split-code-ai");
    const codeCol = document.getElementById("chat-code-column");
    if (wrap) wrap.classList.remove("gpt-chat-wrap--workspace");
    if (root) root.classList.remove("gpt-workspace-open");
    if (aside) aside.hidden = true;
    if (s1) s1.hidden = true;
    if (s2) s2.hidden = true;
    if (codeCol) codeCol.hidden = true;
    state.explorerVisible = false;
    const tree = document.getElementById("chat-explorer-tree");
    if (tree) tree.innerHTML = "";
    setStatus("");
    const box = document.getElementById("chat-messages");
    const hasMsg = box && box.children.length > 0;
    if (!hasMsg && typeof cctChat !== "undefined" && cctChat.showWelcome) cctChat.showWelcome();
  }

  function closeFilePanel() {
    const bar = document.getElementById("chat-ai-review-bar");
    if (bar) bar.hidden = true;
    const sel = document.getElementById("chat-ai-review-file-sel");
    if (sel) {
      sel.innerHTML = "";
      sel.onchange = null;
    }
    aiReviewSession = null;
    const stack0 = document.getElementById("chat-file-editor-stack");
    if (stack0) stack0.classList.remove("chat-file-editor-stack--ai-diff");
    const panel = document.getElementById("chat-file-panel");
    const ed = document.getElementById("chat-file-editor");
    const pathEl = document.getElementById("chat-file-path");
    const stack = document.getElementById("chat-file-editor-stack");
    const host = document.getElementById("chat-file-editor-host");
    if (typeof window.cctChatWorkspaceDestroyEditor === "function") {
      window.cctChatWorkspaceDestroyEditor();
    }
    if (host) host.innerHTML = "";
    if (stack) stack.classList.remove("use-fallback");
    if (panel) panel.hidden = true;
    if (ed) ed.value = "";
    if (pathEl) pathEl.textContent = "";
    state.openRelPath = "";
    state.openLabel = "";
    state.lastFileHandle = null;
    state.saveKind = "none";
    const saveBtn = document.getElementById("chat-file-save");
    if (saveBtn) saveBtn.disabled = false;
    syncCodeEmptyVisibility();
    syncAiReviewButtons();
  }

  function openFilePanel(label, content, saveKind) {
    const panel = document.getElementById("chat-file-panel");
    const ed = document.getElementById("chat-file-editor");
    const pathEl = document.getElementById("chat-file-path");
    const stack = document.getElementById("chat-file-editor-stack");
    const host = document.getElementById("chat-file-editor-host");
    if (!panel || !ed || !pathEl) return;
    if (stack) stack.classList.remove("chat-file-editor-stack--ai-diff");
    pathEl.textContent = label;
    const text = content || "";
    const pathLabel = (label || "").split(/[/\\]/).pop() || "untitled";
    let usedCm = false;
    if (host && typeof window.cctChatWorkspaceMountEditor === "function") {
      try {
        usedCm = !!window.cctChatWorkspaceMountEditor(host, text, pathLabel);
      } catch (err) {
        usedCm = false;
      }
    }
    if (stack) stack.classList.toggle("use-fallback", !usedCm);
    ed.value = text;
    panel.hidden = false;
    state.openLabel = label;
    state.saveKind = saveKind;
    const saveBtn = document.getElementById("chat-file-save");
    if (saveBtn) saveBtn.disabled = saveKind === "none";
    syncCodeEmptyVisibility();
  }

  async function apiJson(url, opts) {
    const r = await fetch(url, { credentials: "include", ...opts });
    const data = await r.json().catch(() => ({}));
    return { r, data };
  }

  const AI_BUNDLE_SKIP_DIRS = new Set([
    "node_modules",
    ".git",
    "dist",
    "build",
    ".vs",
    "out",
    "target",
    "cmake-build-debug",
    "cmake-build-release",
    "__pycache__",
    ".idea",
  ]);
  /** 单次对话打包进模型的最大文件数，避免大仓库数千次请求导致「长时间无反应」 */
  const AI_BUNDLE_MAX_FILES = 72;

  const AI_BUNDLE_TEXT_EXT =
    /\.(c|cc|cpp|cxx|h|hh|hpp|hxx|inl|inc|idl|def|rc|cmake|txt|md|json|js|cjs|mjs|ts|tsx|jsx|css|scss|sass|less|html|htm|xhtml|vue|svelte|xml|plist|yaml|yml|toml|ini|cfg|conf|properties|gradle|kts|java|kt|rs|go|py|pyi|cs|vb|fs|fsx|sql|sh|bash|zsh|ps1|bat|cmd|mak|mk|am|ac|m4|pod|rb|php|pl|pm|swift|mm|m)$/i;

  function isAiBundleTextFile(baseName) {
    const n = (baseName || "").toLowerCase();
    if (n === "makefile" || n === "cmakelists.txt" || n === "dockerfile" || n === "rakefile") return true;
    if (n.endsWith(".gitignore") || n.endsWith(".dockerignore") || n.endsWith(".editorconfig")) return true;
    return AI_BUNDLE_TEXT_EXT.test(n);
  }

  /** 枚举服务端工作区内文本路径（与打包扫描一致），用于整仓检查点 */
  async function enumerateServerWorkspaceTextPaths() {
    const paths = [];
    let stopped = false;
    async function walk(rel) {
      if (stopped) return;
      const q = rel ? "?path=" + encodeURIComponent(String(rel).replace(/\\/g, "/")) : "";
      const { data } = await apiJson("/api/workspace/list" + q);
      if (!data.ok) return;
      const entries = (data.entries || []).slice().sort(sortEntries);
      for (const ent of entries) {
        if (stopped) return;
        const sub = rel ? joinRel(rel, ent.name) : ent.name;
        const norm = String(sub).replace(/\\/g, "/");
        if (ent.type === "dir") {
          if (AI_BUNDLE_SKIP_DIRS.has(ent.name)) continue;
          await walk(norm);
        } else {
          if (!isAiBundleTextFile(ent.name)) continue;
          paths.push(norm);
          if (paths.length >= AI_BUNDLE_MAX_FILES) {
            stopped = true;
            return;
          }
        }
      }
    }
    await walk(normWorkspacePath(state.serverTreeScope || ""));
    return paths;
  }

  async function collectServerFilesForAiBundle(maxJsonChars) {
    const files = [];
    let stopped = false;
    setStatus("正在扫描工作区…");
    async function walk(rel) {
      if (stopped) return;
      const q = rel ? "?path=" + encodeURIComponent(String(rel).replace(/\\/g, "/")) : "";
      const { data } = await apiJson("/api/workspace/list" + q);
      if (!data.ok) return;
      const entries = (data.entries || []).slice().sort(sortEntries);
      for (const ent of entries) {
        if (stopped) return;
        const sub = rel ? joinRel(rel, ent.name) : ent.name;
        const norm = String(sub).replace(/\\/g, "/");
        if (ent.type === "dir") {
          if (AI_BUNDLE_SKIP_DIRS.has(ent.name)) continue;
          await walk(norm);
        } else {
          if (!isAiBundleTextFile(ent.name)) continue;
          setStatus("读取 " + norm + " …");
          const { data: fd } = await apiJson("/api/workspace/file?path=" + encodeURIComponent(norm));
          if (!fd.ok || typeof fd.content !== "string") continue;
          const content = fd.content;
          if (content.length > 1_800_000) continue;
          const trial = files.concat([{ path: norm, content }]);
          if (JSON.stringify({ files: trial }).length > maxJsonChars) {
            stopped = true;
            return;
          }
          files.push({ path: norm, content });
          if (files.length >= AI_BUNDLE_MAX_FILES) {
            stopped = true;
            return;
          }
        }
      }
    }
    await walk(normWorkspacePath(state.serverTreeScope || ""));
    setStatus(files.length ? "已读取 " + files.length + " 个文本文件（打包中）" : "");
    return files;
  }

  const AI_BUNDLE_MAX_FILE_BYTES = 2 * 1024 * 1024;

  async function collectHandleFilesForAiBundle(dirHandle, relPrefix, files, maxJsonChars) {
    let stopped = false;
    setStatus("正在扫描本机文件夹…");
    async function walk(dh, rel) {
      if (stopped) return;
      const entries = [];
      for await (const [n, h] of dh.entries()) {
        entries.push({ name: n, handle: h });
      }
      entries.sort((a, b) => {
        if (a.handle.kind !== b.handle.kind) return a.handle.kind === "directory" ? -1 : 1;
        return a.name.localeCompare(b.name);
      });
      for (const { name: n, handle: h } of entries) {
        if (stopped) return;
        const sub = rel ? joinRel(rel, n) : n;
        const norm = normWorkspacePath(sub);
        if (h.kind === "directory") {
          if (AI_BUNDLE_SKIP_DIRS.has(n)) continue;
          await walk(h, norm);
        } else {
          if (!isAiBundleTextFile(n)) continue;
          setStatus("读取 " + norm + " …");
          let content = "";
          try {
            const file = await h.getFile();
            if (file.size > AI_BUNDLE_MAX_FILE_BYTES) continue;
            content = await file.text();
          } catch (_) {
            continue;
          }
          const trial = files.concat([{ path: norm, content }]);
          if (JSON.stringify({ files: trial }).length > maxJsonChars) {
            stopped = true;
            return;
          }
          files.push({ path: norm, content });
          if (files.length >= AI_BUNDLE_MAX_FILES) {
            stopped = true;
            return;
          }
        }
      }
    }
    await walk(dirHandle, relPrefix ? normWorkspacePath(relPrefix) : "");
    setStatus(files.length ? "已读取 " + files.length + " 个文本文件（打包中）" : "");
  }

  /** 枚举本机授权目录内文本路径（与打包扫描一致），用于检查点 */
  async function enumerateHandleTextPaths(dirHandle, relPrefix) {
    const paths = [];
    let stopped = false;
    async function walk(dh, rel) {
      if (stopped) return;
      const entries = [];
      for await (const [n, h] of dh.entries()) {
        entries.push({ name: n, handle: h });
      }
      entries.sort((a, b) => {
        if (a.handle.kind !== b.handle.kind) return a.handle.kind === "directory" ? -1 : 1;
        return a.name.localeCompare(b.name);
      });
      for (const { name: n, handle: h } of entries) {
        if (stopped) return;
        const sub = rel ? joinRel(rel, n) : n;
        const norm = normWorkspacePath(sub);
        if (h.kind === "directory") {
          if (AI_BUNDLE_SKIP_DIRS.has(n)) continue;
          await walk(h, norm);
        } else {
          if (!isAiBundleTextFile(n)) continue;
          paths.push(norm);
          if (paths.length >= AI_BUNDLE_MAX_FILES) {
            stopped = true;
            return;
          }
        }
      }
    }
    await walk(dirHandle, relPrefix ? normWorkspacePath(relPrefix) : "");
    return paths;
  }

  async function writeRelPathToDirHandle(rootHandle, relPath, content) {
    const parts = normWorkspacePath(relPath)
      .split("/")
      .filter(Boolean);
    if (!parts.length) throw new Error("空路径");
    let dir = rootHandle;
    for (let i = 0; i < parts.length - 1; i++) {
      dir = await dir.getDirectoryHandle(parts[i], { create: true });
    }
    const fh = await dir.getFileHandle(parts[parts.length - 1], { create: true });
    const writable = await fh.createWritable();
    await writable.write(content);
    await writable.close();
  }

  async function getFileHandleByRelPath(rootHandle, relPath) {
    const parts = normWorkspacePath(relPath)
      .split("/")
      .filter(Boolean);
    if (!parts.length) throw new Error("空路径");
    let dir = rootHandle;
    for (let i = 0; i < parts.length - 1; i++) {
      dir = await dir.getDirectoryHandle(parts[i], { create: false });
    }
    return await dir.getFileHandle(parts[parts.length - 1], { create: false });
  }

  async function deleteRelPathFromDirHandle(rootHandle, relPath) {
    const parts = normWorkspacePath(relPath)
      .split("/")
      .filter(Boolean);
    if (!parts.length) throw new Error("空路径");
    const baseName = parts.pop();
    let dir = rootHandle;
    for (const p of parts) {
      dir = await dir.getDirectoryHandle(p, { create: false });
    }
    await dir.removeEntry(baseName);
  }

  async function snapshotServerWorkspaceForRollback(paths) {
    const items = [];
    for (const path of paths) {
      if (!path) continue;
      const { data } = await apiJson("/api/workspace/file?path=" + encodeURIComponent(path));
      if (data && data.ok && typeof data.content === "string") {
        items.push({ path, existed: true, content: data.content });
      } else {
        items.push({ path, existed: false, content: "" });
      }
    }
    return items;
  }

  async function snapshotLocalWorkspaceForRollback(paths) {
    const items = [];
    if (!state.lastDirHandle) return items;
    for (const path of paths) {
      if (!path) continue;
      try {
        const fh = await getFileHandleByRelPath(state.lastDirHandle, path);
        const file = await fh.getFile();
        let content = await file.text();
        if (content.length > AI_BUNDLE_MAX_FILE_BYTES) content = content.slice(0, AI_BUNDLE_MAX_FILE_BYTES);
        items.push({ path, existed: true, content });
      } catch (_) {
        items.push({ path, existed: false, content: "" });
      }
    }
    return items;
  }

  /** 供聊天请求体 workspace_bundle：服务端或本机(File System Access)工作区下的文本文件快照 JSON */
  window.cctChatProjectBuildWorkspaceAiBundle = async function () {
    if (!state.explorerVisible) return "";
    try {
      const maxJson = 200000;
      if (state.mode === "server") {
        const collected = await collectServerFilesForAiBundle(maxJson);
        if (!collected.length) return "";
        if (collected.length >= AI_BUNDLE_MAX_FILES) {
          const es = document.getElementById("chat-explorer-status");
          if (es)
            es.textContent =
              "AI 快照：已打包前 " +
              AI_BUNDLE_MAX_FILES +
              " 个文本类文件（大仓库限制；未列入的文件仍可让模型通过 CCT_WORKSPACE 修改）";
        }
        return JSON.stringify({ files: collected });
      }
      if (state.mode === "local-handle" && state.lastDirHandle) {
        try {
          if (typeof state.lastDirHandle.requestPermission === "function") {
            const st = await state.lastDirHandle.requestPermission({ mode: "read" });
            if (st !== "granted") return "";
          }
        } catch (_) {
          return "";
        }
        const collected = [];
        await collectHandleFilesForAiBundle(state.lastDirHandle, "", collected, maxJson);
        if (!collected.length) return "";
        if (collected.length >= AI_BUNDLE_MAX_FILES) {
          const es = document.getElementById("chat-explorer-status");
          if (es)
            es.textContent =
              "AI 快照：已打包前 " + AI_BUNDLE_MAX_FILES + " 个文本类文件（本机大目录限制；其余路径仍可写回）";
        }
        return JSON.stringify({ files: collected });
      }
      return "";
    } catch (_) {
      return "";
    }
  };

  /**
   * 解析助手返回的 writes：服务端 PUT；本机目录则写入已授权的 FileSystemDirectoryHandle。
   * 兼容旧名 cctChatProjectApplyServerWorkspaceWrites。
   */
  window.cctChatProjectApplyWorkspaceWrites = async function (writes) {
    if (!Array.isArray(writes) || !writes.length) return { ok: true, count: 0 };
    if (!state.explorerVisible) {
      return { ok: false, error: "请先打开左侧工作区（资源管理器）。" };
    }
    let count = 0;
    const errs = [];

    if (state.mode === "server") {
      const uniqPaths = [...new Set(writes.map((w) => normWorkspacePath(w.path)).filter(Boolean))];
      let rollbackSnap = null;
      if (uniqPaths.length) {
        rollbackSnap = { mode: "server", items: await snapshotServerWorkspaceForRollback(uniqPaths) };
      }
      for (const w of writes) {
        if (!w || typeof w.path !== "string" || typeof w.content !== "string") continue;
        const path = normWorkspacePath(w.path);
        if (!path) continue;
        const { r, data } = await apiJson("/api/workspace/file", {
          method: "PUT",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ path, content: w.content }),
        });
        if (data && data.ok) count++;
        else errs.push(path + ": " + ((data && data.error) || String(r.status)));
      }
      const openNorm = normWorkspacePath(state.openRelPath);
      rebuildServerTreeRoot();
      if (openNorm) {
        for (const w of writes) {
          const p = normWorkspacePath(w.path);
          if (p && p === openNorm) {
            await openServerFile(openNorm, null);
            break;
          }
        }
      }
      if (!count && errs.length) return { ok: false, error: errs[0] };
      if (errs.length) return { ok: true, count, rollbackSnap, error: "部分失败：" + errs.slice(0, 3).join("；") };
      return { ok: true, count, rollbackSnap };
    }

    if (state.mode === "local-handle" && state.lastDirHandle) {
      try {
        if (typeof state.lastDirHandle.requestPermission === "function") {
          const st = await state.lastDirHandle.requestPermission({ mode: "readwrite" });
          if (st !== "granted") {
            return { ok: false, error: "需要浏览器对本机文件夹的读写权限才能自动写回（请在权限提示中选择允许）。" };
          }
        }
      } catch (e) {
        return { ok: false, error: (e && e.message) || "无法获取本机文件夹写入权限。" };
      }
      const uniqPathsLocal = [...new Set(writes.map((w) => normWorkspacePath(w.path)).filter(Boolean))];
      let rollbackSnap = null;
      if (uniqPathsLocal.length) {
        rollbackSnap = { mode: "local-handle", items: await snapshotLocalWorkspaceForRollback(uniqPathsLocal) };
      }
      for (const w of writes) {
        if (!w || typeof w.path !== "string" || typeof w.content !== "string") continue;
        const path = normWorkspacePath(w.path);
        if (!path) continue;
        try {
          await writeRelPathToDirHandle(state.lastDirHandle, path, w.content);
          count++;
        } catch (e) {
          errs.push(path + ": " + (e && e.message ? e.message : "写入失败"));
        }
      }
      const label = (window.cctWorkspace && window.cctWorkspace.folderLabel) || "folder";
      renderHandleRoot(state.lastDirHandle, label);
      const openNorm = normWorkspacePath(state.openRelPath);
      if (openNorm && writes.some((w) => w && normWorkspacePath(w.path) === openNorm)) {
        try {
          const fh = await getFileHandleByRelPath(state.lastDirHandle, openNorm);
          await openHandleFile(fh, openNorm, null);
        } catch (_) {}
      }
      if (!count && errs.length) return { ok: false, error: errs[0] };
      if (errs.length) return { ok: true, count, rollbackSnap, error: "部分失败：" + errs.slice(0, 3).join("；") };
      return { ok: true, count, rollbackSnap };
    }

    return {
      ok: false,
      error:
        "多文件自动写回需要「服务端工作区」或通过「打开文件夹」授予的本机目录（File System Access）。传统文件选择器打开的文件夹无法写回多个路径。",
    };
  };

  window.cctChatProjectRollbackWorkspaceSnapshot = async function (snap) {
    if (!snap || !snap.mode || !Array.isArray(snap.items) || !snap.items.length) {
      return { ok: false, error: "无效快照" };
    }
    if (snap.mode === "server") {
      for (const it of snap.items) {
        if (it.existed) {
          const { data } = await apiJson("/api/workspace/file", {
            method: "PUT",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ path: it.path, content: it.content }),
          });
          if (!data || !data.ok) return { ok: false, error: (data && data.error) || it.path };
        } else {
          const { data } = await apiJson("/api/workspace/file?path=" + encodeURIComponent(it.path), {
            method: "DELETE",
          });
          if (!data || !data.ok) return { ok: false, error: (data && data.error) || it.path };
        }
      }
      rebuildServerTreeRoot();
      const openNorm = normWorkspacePath(state.openRelPath);
      if (openNorm) {
        try {
          await openServerFile(openNorm, null);
        } catch (_) {}
      }
      return { ok: true };
    }
    if (snap.mode === "local-handle" && state.lastDirHandle) {
      try {
        if (typeof state.lastDirHandle.requestPermission === "function") {
          const st = await state.lastDirHandle.requestPermission({ mode: "readwrite" });
          if (st !== "granted") return { ok: false, error: "需要读写权限才能撤回" };
        }
      } catch (e) {
        return { ok: false, error: (e && e.message) || "权限失败" };
      }
      for (const it of snap.items) {
        try {
          if (it.existed) {
            await writeRelPathToDirHandle(state.lastDirHandle, it.path, it.content);
          } else {
            await deleteRelPathFromDirHandle(state.lastDirHandle, it.path);
          }
        } catch (e) {
          return { ok: false, error: (e && e.message) || String(it.path) };
        }
      }
      const label = (window.cctWorkspace && window.cctWorkspace.folderLabel) || "folder";
      renderHandleRoot(state.lastDirHandle, label);
      return { ok: true };
    }
    return { ok: false, error: "当前模式不支持撤回" };
  };

  /**
   * 在本轮用户提问发出前，对当前已打开工作区打快照（整仓文本文件枚举上限与 AI 打包一致）。
   * @returns {Promise<{mode:string,items:any[]}|null>}
   */
  window.cctChatProjectCheckpointBeforeUserTurn = async function () {
    if (!state.explorerVisible) return null;
    try {
      if (state.mode === "server") {
        const paths = await enumerateServerWorkspaceTextPaths();
        if (!paths.length) return null;
        const items = await snapshotServerWorkspaceForRollback(paths);
        return { mode: "server", items };
      }
      if (state.mode === "local-handle" && state.lastDirHandle) {
        try {
          if (typeof state.lastDirHandle.requestPermission === "function") {
            const st = await state.lastDirHandle.requestPermission({ mode: "read" });
            if (st !== "granted") return null;
          }
        } catch (_) {
          return null;
        }
        const paths = await enumerateHandleTextPaths(state.lastDirHandle, "");
        if (!paths.length) return null;
        const items = await snapshotLocalWorkspaceForRollback(paths);
        return { mode: "local-handle", items };
      }
    } catch (_) {}
    return null;
  };

  window.cctChatProjectApplyServerWorkspaceWrites = window.cctChatProjectApplyWorkspaceWrites;

  function lineDiffLcsRows(aText, bText) {
    const norm = (t) => String(t || "").replace(/\r\n/g, "\n");
    let a = norm(aText).split("\n");
    let b = norm(bText).split("\n");
    const MAXL = 1000;
    let truncated = false;
    if (a.length > MAXL || b.length > MAXL) {
      truncated = true;
      if (a.length > MAXL) a = a.slice(0, MAXL);
      if (b.length > MAXL) b = b.slice(0, MAXL);
    }
    const m = a.length;
    const n = b.length;
    const dp = [];
    for (let i = 0; i <= m; i++) {
      dp[i] = new Array(n + 1).fill(0);
    }
    for (let i = m - 1; i >= 0; i--) {
      for (let j = n - 1; j >= 0; j--) {
        if (a[i] === b[j]) dp[i][j] = 1 + dp[i + 1][j + 1];
        else dp[i][j] = Math.max(dp[i + 1][j], dp[i][j + 1]);
      }
    }
    const rows = [];
    let i = 0;
    let j = 0;
    while (i < m && j < n) {
      if (a[i] === b[j]) {
        rows.push({ t: "eq", line: a[i] });
        i++;
        j++;
      } else if (dp[i + 1][j] >= dp[i][j + 1]) {
        rows.push({ t: "del", line: a[i] });
        i++;
      } else {
        rows.push({ t: "add", line: b[j] });
        j++;
      }
    }
    while (i < m) rows.push({ t: "del", line: a[i++] });
    while (j < n) rows.push({ t: "add", line: b[j++] });
    if (truncated) rows.push({ t: "eq", line: "…（diff 已截断，仅显示前 " + MAXL + " 行）" });
    return rows;
  }

  function renderAiDiffHost(host, rows) {
    if (!host) return;
    host.innerHTML = "";
    const wrap = document.createElement("div");
    wrap.className = "cct-ai-diff-scroll";
    for (let r = 0; r < rows.length; r++) {
      const row = rows[r];
      const div = document.createElement("div");
      div.className = "cct-ai-diff-row cct-ai-diff-" + row.t;
      div.textContent = row.line != null && row.line !== "" ? row.line : " ";
      wrap.appendChild(div);
    }
    host.appendChild(wrap);
  }

  function syncAiReviewButtons() {
    const keepBtn = document.getElementById("chat-ai-review-keep");
    const doneBtn = document.getElementById("chat-ai-review-done");
    const post = !!(aiReviewSession && aiReviewSession.kind === "post");
    if (keepBtn) keepBtn.hidden = post;
    if (doneBtn) doneBtn.hidden = !post;
  }

  function updateAiReviewUi() {
    const bar = document.getElementById("chat-ai-review-bar");
    const meta = document.getElementById("chat-ai-review-meta");
    if (!aiReviewSession || !bar || !meta) return;
    meta.textContent =
      aiReviewSession.kind === "post"
        ? "代码检测 · 已写盘 · " + aiReviewSession.items.length + " 个文件（红删绿增）"
        : "本轮 AI 修改 · " + aiReviewSession.items.length + " 个文件（预览，尚未写入磁盘）";
    bar.hidden = false;
    syncAiReviewButtons();
  }

  function aiReviewRenderIndex(idx) {
    if (!aiReviewSession || !aiReviewSession.items[idx]) return;
    aiReviewSession.idx = idx;
    const it = aiReviewSession.items[idx];
    const host = document.getElementById("chat-file-editor-host");
    const pathEl = document.getElementById("chat-file-path");
    const stack = document.getElementById("chat-file-editor-stack");
    const ed = document.getElementById("chat-file-editor");
    if (typeof window.cctChatWorkspaceDestroyEditor === "function") {
      window.cctChatWorkspaceDestroyEditor();
    }
    if (host) host.innerHTML = "";
    if (stack) stack.classList.add("chat-file-editor-stack--ai-diff");
    if (ed) ed.value = "";
    if (pathEl) pathEl.textContent = it.path + "（预览）";
    state.openRelPath = it.path;
    state.openLabel = it.path;
    state.saveKind = "ai-review";
    const saveBtn = document.getElementById("chat-file-save");
    if (saveBtn) saveBtn.disabled = true;
    renderAiDiffHost(host, lineDiffLcsRows(it.before, it.after));
    syncCodeEmptyVisibility();
  }

  window.cctChatProjectStartAiReview = async function (writes) {
    if (!Array.isArray(writes) || !writes.length) return { skipped: true };
    if (state.mode !== "server" && state.mode !== "local-handle") {
      return { skipped: true };
    }
    if (!state.explorerVisible) return { skipped: true };
    const items = [];
    for (const w of writes) {
      if (!w || typeof w.path !== "string" || typeof w.content !== "string") continue;
      const path = normWorkspacePath(w.path);
      if (!path) continue;
      let before = "";
      let wasNew = false;
      if (state.mode === "server") {
        const { r, data } = await apiJson("/api/workspace/file?path=" + encodeURIComponent(path));
        if (data.ok && typeof data.content === "string") {
          before = data.content;
          wasNew = false;
        } else {
          before = "";
          wasNew = r.status === 404 || !!(data.error && String(data.error).indexOf("不存在") >= 0);
        }
      } else {
        try {
          const fh = await getFileHandleByRelPath(state.lastDirHandle, path);
          const file = await fh.getFile();
          before = await file.text();
          wasNew = false;
        } catch (_) {
          before = "";
          wasNew = true;
        }
      }
      items.push({ path, before, after: w.content, wasNew });
    }
    if (!items.length) return { skipped: true };
    aiReviewSession = { items, idx: 0, kind: "pre" };
    const panel = document.getElementById("chat-file-panel");
    const codeCol = document.getElementById("chat-code-column");
    if (codeCol) codeCol.hidden = false;
    if (panel) panel.hidden = false;
    const sel = document.getElementById("chat-ai-review-file-sel");
    if (sel) {
      sel.innerHTML = "";
      for (let i = 0; i < items.length; i++) {
        const o = document.createElement("option");
        o.value = String(i);
        o.textContent = items[i].path + (items[i].wasNew ? " （新建）" : "");
        sel.appendChild(o);
      }
      sel.onchange = () => {
        const j = parseInt(sel.value, 10);
        if (!Number.isNaN(j)) aiReviewRenderIndex(j);
      };
    }
    updateAiReviewUi();
    aiReviewRenderIndex(0);
    setStatus("AI 预览：确认前不会修改磁盘");
    return { ok: true };
  };

  window.cctChatProjectShowPostApplyDiff = function (items) {
    if (!Array.isArray(items) || !items.length) return;
    showExplorer();
    aiReviewSession = { items, idx: 0, kind: "post" };
    const panel = document.getElementById("chat-file-panel");
    const codeCol = document.getElementById("chat-code-column");
    if (codeCol) codeCol.hidden = false;
    if (panel) panel.hidden = false;
    const sel = document.getElementById("chat-ai-review-file-sel");
    if (sel) {
      sel.innerHTML = "";
      for (let i = 0; i < items.length; i++) {
        const o = document.createElement("option");
        o.value = String(i);
        o.textContent = items[i].path + (items[i].wasNew ? " （新建）" : "");
        sel.appendChild(o);
      }
      sel.onchange = () => {
        const j = parseInt(sel.value, 10);
        if (!Number.isNaN(j)) aiReviewRenderIndex(j);
      };
    }
    updateAiReviewUi();
    aiReviewRenderIndex(0);
    setStatus("代码检测：写盘完成，以下为修改差异预览");
  };

  window.cctChatProjectCommitAiReview = async function () {
    if (!aiReviewSession || !aiReviewSession.items.length) return { ok: false };
    const writes = aiReviewSession.items.map((it) => ({ path: it.path, content: it.after }));
    const first = aiReviewSession.items[0].path;
    aiReviewSession = null;
    const bar = document.getElementById("chat-ai-review-bar");
    if (bar) bar.hidden = true;
    const sel = document.getElementById("chat-ai-review-file-sel");
    if (sel) {
      sel.innerHTML = "";
      sel.onchange = null;
    }
    const stack = document.getElementById("chat-file-editor-stack");
    if (stack) stack.classList.remove("chat-file-editor-stack--ai-diff");
    const ar = await window.cctChatProjectApplyWorkspaceWrites(writes);
    setStatus(ar && ar.ok ? "已保存本轮 AI 修改" : "");
    if (state.mode === "server") await openServerFile(first, null);
    else if (state.mode === "local-handle" && state.lastDirHandle) {
      try {
        const fh = await getFileHandleByRelPath(state.lastDirHandle, first);
        await openHandleFile(fh, first, null);
      } catch (_) {}
    }
    return ar || { ok: true };
  };

  window.cctChatProjectDiscardAiReview = async function () {
    const wasPost = !!(aiReviewSession && aiReviewSession.kind === "post");
    if (!aiReviewSession) {
      closeFilePanel();
      return { ok: true };
    }
    aiReviewSession = null;
    const bar = document.getElementById("chat-ai-review-bar");
    if (bar) bar.hidden = true;
    const sel = document.getElementById("chat-ai-review-file-sel");
    if (sel) {
      sel.innerHTML = "";
      sel.onchange = null;
    }
    const stack = document.getElementById("chat-file-editor-stack");
    if (stack) stack.classList.remove("chat-file-editor-stack--ai-diff");
    closeFilePanel();
    setStatus(wasPost ? "" : "已撤销本轮 AI 预览（磁盘未被修改）");
    return { ok: true };
  };

  /** 探测文件可读（不弹窗）；用于「定位到对话路径」时尝试 CCT_Export 前缀 */
  async function peekServerFile(relPath) {
    const q = encodeURIComponent(relPath);
    const { data } = await apiJson("/api/workspace/file?path=" + q);
    return data && data.ok && typeof data.content === "string" ? data : null;
  }

  /** 尝试 popup/foo.js、CCT_Export/…/popup/foo.js，以及在「对话目录」下的同类前缀 */
  async function expandExportFolderCandidates(relNorm) {
    const norm = normWorkspacePath(relNorm);
    const out = [];
    if (norm) out.push(norm);
    const scope = normWorkspacePath(state.serverTreeScope || "");

    async function collectFromList(listPath) {
      const q = listPath ? "?path=" + encodeURIComponent(listPath) : "";
      const { data } = await apiJson("/api/workspace/list" + q);
      if (!data || !data.ok || !data.entries) return;
      for (const ent of data.entries) {
        if (ent.type !== "dir") continue;
        const n = ent.name || "";
        if (/^CCT_Export/i.test(n) && norm) {
          const base = listPath ? joinRel(listPath, n) : n;
          out.push(joinRel(base, norm));
        }
      }
    }

    await collectFromList("");
    if (scope) await collectFromList(scope);
    return [...new Set(out)];
  }

  async function openServerFile(relPath, row) {
    if (aiReviewSession) {
      window.alert("当前有未确认的 AI 修改预览，请先点击「保存全部」或关闭预览。");
      return;
    }
    const q = encodeURIComponent(relPath);
    const { r, data } = await apiJson("/api/workspace/file?path=" + q);
    if (!data.ok) {
      window.alert(data.error || "读取失败 " + r.status);
      return;
    }
    if (row) selectRow(row);
    state.openRelPath = relPath;
    openFilePanel(relPath, data.content || "", "server");
  }

  /** 合并会话锚点与本条路径，生成候选相对路径列表（与服务端 list/file 一致） */
  function buildUniqAssistantPaths(pathsIn) {
    const raw = (pathsIn || []).map((p) => normWorkspacePath(String(p))).filter(Boolean);
    let uniq = [...new Set(raw)];
    if (!uniq.length) return [];
    uniq.sort((a, b) => a.length - b.length);
    const anchor =
      typeof window.cctChatProjectGetThreadWorkspaceAnchor === "function"
        ? window.cctChatProjectGetThreadWorkspaceAnchor()
        : "";
    if (anchor) {
      const prefixed = uniq.map((u) => joinRel(anchor, u)).map(normWorkspacePath).filter(Boolean);
      uniq = [...new Set(uniq.concat(prefixed))];
      uniq.sort((a, b) => a.length - b.length);
    }
    return uniq;
  }

  /** 在已启用服务端工作区的前提下，尝试打开本条助手涉及的磁盘文件（不改变会话锚点） */
  async function tryPeekServerWorkspaceByAssistantPaths(pathsIn) {
    const uniq = buildUniqAssistantPaths(pathsIn);
    if (!uniq.length) return false;
    for (const target of uniq) {
      const cands = await expandExportFolderCandidates(target);
      for (const c of cands) {
        const peek = await peekServerFile(c);
        if (peek) {
          state.openRelPath = c;
          openFilePanel(c, peek.content || "", "server");
          return true;
        }
      }
    }
    return false;
  }

  /**
   * 进入三栏聊天工作区并尽量打开首个路径（优先服务端 workspace_root）。
   * @param {string[]} pathsIn 相对路径列表，如 writes 中的 path
   */
  window.cctChatProjectFocusWorkspaceForPaths = async function (pathsIn) {
    const uniq = buildUniqAssistantPaths(pathsIn);
    if (!uniq.length) {
      window.alert("未解析到有效路径。");
      return;
    }
    const firstTarget = uniq[0];

    if (typeof window.cctSetActiveAppView === "function") window.cctSetActiveAppView("chat");
    if (typeof window.cctChatProjectResumeIfWorkspace === "function") window.cctChatProjectResumeIfWorkspace();
    document.getElementById("app-root")?.classList.add("gpt-workspace-open");

    if (typeof window.cctChatProjectDiscardAiReview === "function") {
      await window.cctChatProjectDiscardAiReview();
    }

    const { data: st } = await apiJson("/api/workspace/status");
    if (st && st.ok && st.enabled) {
      await probeServerWorkspace();
      onServerWorkspace();
      await new Promise((res) => setTimeout(res, 80));
      const opened = await tryPeekServerWorkspaceByAssistantPaths(pathsIn);
      if (!opened) {
        window.alert("服务端工作区中未找到对应文件（可能尚未通过「保存」写入，或路径与当前 workspace_root 不一致）。");
      }
      return;
    }

    if (state.mode === "local-handle" && state.lastDirHandle) {
      try {
        if (typeof state.lastDirHandle.requestPermission === "function") {
          const perm = await state.lastDirHandle.requestPermission({ mode: "read" });
          if (perm !== "granted") throw new Error("需要读取权限");
        }
        const fh = await getFileHandleByRelPath(state.lastDirHandle, firstTarget);
        await openHandleFile(fh, firstTarget, null);
      } catch (e) {
        window.alert((e && e.message) || "无法在本机工作区打开该路径；请先启用服务端工作区。");
      }
      return;
    }

    window.alert("请先在左侧打开「服务端工作区」（需在配置中设置 workspace_root），或使用「打开文件夹」授权本机目录。");
  };

  /**
   * 与「历史对话列表选择会话」一致：根据 /api/chat/thread 返回的 workspaceAnchor + messages 更新锚点。
   * @param {string} listFallbackAnchor 列表接口可能携带的 workspaceAnchor（详情缺失时兜底）
   */
  window.cctChatProjectSyncThreadWorkspaceAnchorFromApi = function (threadId, apiPayload, listFallbackAnchor) {
    const tid = String(threadId || "").trim() || "main";
    const payload = apiPayload && typeof apiPayload === "object" ? apiPayload : {};
    const msgs = Array.isArray(payload.messages) ? payload.messages : [];
    const serverAnchor = String(payload.workspaceAnchor || listFallbackAnchor || "").trim();
    if (serverAnchor && typeof window.cctChatProjectSetThreadWorkspaceAnchor === "function") {
      window.cctChatProjectSetThreadWorkspaceAnchor(tid, serverAnchor);
    } else if (typeof window.cctChatProjectInferAnchorFromMessages === "function") {
      window.cctChatProjectInferAnchorFromMessages(tid, msgs);
    }
  };

  /**
   * 「气泡下 IDE 按钮」：与历史会话跳转同一套——拉取 thread → 同步锚点 → EnsureServerWorkspace → 再尝试打开本条路径。
   */
  window.cctChatProjectActivateIdeWorkspace = async function (pathsFromBubble) {
    const paths = pathsFromBubble || [];
    if (typeof window.cctChatProjectDiscardAiReview === "function") {
      await window.cctChatProjectDiscardAiReview();
    }
    if (typeof window.cctSetActiveAppView === "function") window.cctSetActiveAppView("chat");
    if (typeof window.cctChatProjectResumeIfWorkspace === "function") window.cctChatProjectResumeIfWorkspace();
    document.getElementById("app-root")?.classList.add("gpt-workspace-open");

    const tid = typeof window.cctGetActiveThreadId === "function" ? window.cctGetActiveThreadId() : "main";
    try {
      const r = await fetch("/api/chat/thread?id=" + encodeURIComponent(tid), { credentials: "include" });
      const d = await r.json().catch(() => ({}));
      if (d && d.ok) window.cctChatProjectSyncThreadWorkspaceAnchorFromApi(tid, d, "");
    } catch (_) {}

    if (typeof window.cctChatProjectEnsureServerWorkspaceForThread === "function") {
      await window.cctChatProjectEnsureServerWorkspaceForThread(tid);
    }

    if (!paths.length) return;

    const opened = await tryPeekServerWorkspaceByAssistantPaths(paths);
    if (!opened) {
      window.alert("服务端工作区中未找到本条涉及的文件（可能尚未写入）。左侧已进入本会话目录。");
    }
  };

  function sortEntries(a, b) {
    if (a.type !== b.type) return a.type === "dir" ? -1 : 1;
    return a.name.localeCompare(b.name);
  }

  function createServerFileRow(name, fullPath, listedScope) {
    const li = document.createElement("li");
    li.className = "chat-tree-node";
    const row = document.createElement("div");
    row.className = "ide-tree-item ide-tree-file chat-tree-row";
    row.textContent = name;
    attachServerTreeRowCtx(row, listedScope, "file", fullPath);
    row.addEventListener("click", async (e) => {
      e.stopPropagation();
      await openServerFile(fullPath, row);
    });
    li.appendChild(row);
    return li;
  }

  function createServerDirRow(name, fullPath, listedScope) {
    const li = document.createElement("li");
    li.className = "chat-tree-node";
    const row = document.createElement("div");
    row.className = "ide-tree-item ide-tree-dir chat-tree-row";
    row.innerHTML =
      '<span class="chat-tree-chev" aria-hidden="true">›</span> <span class="chat-tree-name"></span>';
    row.querySelector(".chat-tree-name").textContent = name;
    li.appendChild(row);
    attachServerTreeRowCtx(row, listedScope, "dir", fullPath);
    row.addEventListener("click", async (e) => {
      e.stopPropagation();
      await enterServerScope(fullPath);
    });
    return li;
  }

  function renderServerRoot() {
    const tree = document.getElementById("chat-explorer-tree");
    if (!tree) return;
    tree.innerHTML = "";
    const rootUl = document.createElement("ul");
    rootUl.className = "ide-tree-list chat-tree-root";
    tree.appendChild(rootUl);

    (async () => {
      const scope = normWorkspacePath(state.serverTreeScope || "");
      const q = scope ? "?path=" + encodeURIComponent(scope) : "";
      let r = null;
      let data = null;
      /** @type {{ ok?: boolean; entries?: unknown[]; error?: string }} */
      let listPayload = null;
      ({ r, data } = await apiJson("/api/workspace/list" + q));
      listPayload = data;
      let listedScope = scope;
      let anchorFallbackNote = "";
      if (!listPayload.ok && scope) {
        const fallback = await apiJson("/api/workspace/list");
        if (fallback.data && fallback.data.ok) {
          listPayload = fallback.data;
          listedScope = "";
          anchorFallbackNote = scope;
        }
      }
      if (!listPayload.ok) {
        rootUl.innerHTML = "";
        const err = document.createElement("div");
        err.className = "chat-tree-error";
        err.textContent =
          listPayload.error ||
          (scope ? "无法列出该目录 " : "无法列出根目录 ") +
            (r && r.status != null ? String(r.status) : "");
        tree.appendChild(err);
        return;
      }
      data = listPayload;
      if (anchorFallbackNote) {
        const base = state.serverRootLabel ? "服务端 · " + state.serverRootLabel : "服务端工作区";
        setStatus(
          base +
            " · 锚点目录「" +
            anchorFallbackNote +
            "」尚不可用，已显示 workspace 根目录；在该路径创建文件夹后可刷新树。"
        );
      }
      if (listedScope) {
        const upLi = document.createElement("li");
        upLi.className = "chat-tree-node";
        const parentSc = parentScopePath(listedScope);
        const upRow = document.createElement("div");
        upRow.className = "ide-tree-item ide-tree-file chat-tree-row chat-tree-scope-up";
        upRow.textContent = parentSc ? "↩ 上级目录（" + parentSc + "）" : "↩ 返回 workspace 根目录";
        upRow.title = "返回上一级";
        upRow.addEventListener("click", (e) => {
          e.stopPropagation();
          state.serverTreeScope = parentSc;
          renderServerRoot();
          const tid = typeof window.cctGetActiveThreadId === "function" ? window.cctGetActiveThreadId() : "main";
          if (parentSc) persistThreadAnchor(tid, parentSc);
          const base = state.serverRootLabel ? "服务端 · " + state.serverRootLabel : "服务端工作区";
          setStatus(parentSc ? base + " · 当前：" + parentSc : base);
        });
        upLi.appendChild(upRow);
        rootUl.appendChild(upLi);
      }
      const entries = (data.entries || []).slice().sort(sortEntries);
      for (const ent of entries) {
        const sub = listedScope ? joinRel(listedScope, ent.name) : ent.name;
        if (ent.type === "dir") rootUl.appendChild(createServerDirRow(ent.name, sub, listedScope));
        else rootUl.appendChild(createServerFileRow(ent.name, sub, listedScope));
      }
    })();
  }

  function rebuildServerTreeRoot() {
    if (state.mode === "server") renderServerRoot();
  }

  async function openHandleFile(fileHandle, displayPath, row) {
    if (aiReviewSession) {
      window.alert("当前有未确认的 AI 修改预览，请先点击「保存全部」或关闭预览。");
      return;
    }
    try {
      const file = await fileHandle.getFile();
      const text = await file.text();
      selectRow(row);
      state.lastFileHandle = fileHandle;
      state.openRelPath = displayPath;
      openFilePanel(displayPath, text, "handle");
    } catch (err) {
      window.alert(err && err.message ? err.message : "读取文件失败");
    }
  }

  function createHandleFileRow(name, fileHandle, displayPath) {
    const li = document.createElement("li");
    li.className = "chat-tree-node";
    const row = document.createElement("div");
    row.className = "ide-tree-item ide-tree-file chat-tree-row";
    row.textContent = name;
    row.addEventListener("click", (e) => {
      e.stopPropagation();
      openHandleFile(fileHandle, displayPath, row);
    });
    li.appendChild(row);
    return li;
  }

  function createHandleDirRow(name, dirHandle, basePath) {
    const fullPath = joinRel(basePath, name);
    const li = document.createElement("li");
    li.className = "chat-tree-node";
    const row = document.createElement("div");
    row.className = "ide-tree-item ide-tree-dir chat-tree-row";
    row.innerHTML =
      '<span class="chat-tree-chev" aria-hidden="true">▸</span> <span class="chat-tree-name"></span>';
    row.querySelector(".chat-tree-name").textContent = name;
    const childUl = document.createElement("ul");
    childUl.className = "ide-tree-list chat-tree-children";
    childUl.hidden = true;
    li.appendChild(row);
    li.appendChild(childUl);
    let loaded = false;
    row.addEventListener("click", async (e) => {
      e.stopPropagation();
      const opening = childUl.hidden;
      if (opening) {
        childUl.hidden = false;
        row.classList.add("is-open");
        if (!loaded) {
          try {
            const entries = [];
            for await (const [n, h] of dirHandle.entries()) {
              entries.push({ name: n, handle: h });
            }
            entries.sort((a, b) => {
              if (a.handle.kind !== b.handle.kind) return a.handle.kind === "directory" ? -1 : 1;
              return a.name.localeCompare(b.name);
            });
            for (const { name: n, handle: h } of entries) {
              const sub = joinRel(fullPath, n);
              if (h.kind === "directory") childUl.appendChild(createHandleDirRow(n, h, fullPath));
              else childUl.appendChild(createHandleFileRow(n, h, sub));
            }
            loaded = true;
          } catch (err) {
            window.alert(err && err.message ? err.message : "列出目录失败");
            childUl.hidden = true;
            row.classList.remove("is-open");
          }
        }
      } else {
        childUl.hidden = true;
        row.classList.remove("is-open");
      }
    });
    return li;
  }

  function renderHandleRoot(dirHandle, label) {
    const tree = document.getElementById("chat-explorer-tree");
    if (!tree) return;
    tree.innerHTML = "";
    const rootUl = document.createElement("ul");
    rootUl.className = "ide-tree-list chat-tree-root";
    tree.appendChild(rootUl);
    (async () => {
      try {
        const entries = [];
        for await (const [n, h] of dirHandle.entries()) {
          entries.push({ name: n, handle: h });
        }
        entries.sort((a, b) => {
          if (a.handle.kind !== b.handle.kind) return a.handle.kind === "directory" ? -1 : 1;
          return a.name.localeCompare(b.name);
        });
        for (const { name: n, handle: h } of entries) {
          if (h.kind === "directory") rootUl.appendChild(createHandleDirRow(n, h, ""));
          else rootUl.appendChild(createHandleFileRow(n, h, n));
        }
      } catch (err) {
        tree.textContent = err && err.message ? err.message : "无法读取目录";
      }
    })();
    setStatus("本地文件夹 · " + label);
  }

  function buildWebkitTree(files) {
    const root = { dirs: {}, files: {} };
    for (let i = 0; i < files.length; i++) {
      const f = files[i];
      const parts = (f.webkitRelativePath || f.name).split("/").filter(Boolean);
      let cur = root;
      for (let j = 0; j < parts.length; j++) {
        const seg = parts[j];
        if (j === parts.length - 1) {
          cur.files[seg] = f;
        } else {
          cur.dirs[seg] = cur.dirs[seg] || { dirs: {}, files: {} };
          cur = cur.dirs[seg];
        }
      }
    }
    return root;
  }

  function renderWebkitNode(node, basePath, containerUl) {
    const dirNames = Object.keys(node.dirs).sort();
    const fileNames = Object.keys(node.files).sort();
    for (let d = 0; d < dirNames.length; d++) {
      const name = dirNames[d];
      const subPath = joinRel(basePath, name);
      const li = document.createElement("li");
      li.className = "chat-tree-node";
      const row = document.createElement("div");
      row.className = "ide-tree-item ide-tree-dir chat-tree-row";
      row.innerHTML =
        '<span class="chat-tree-chev" aria-hidden="true">▸</span> <span class="chat-tree-name"></span>';
      row.querySelector(".chat-tree-name").textContent = name;
      const childUl = document.createElement("ul");
      childUl.className = "ide-tree-list chat-tree-children";
      childUl.hidden = true;
      li.appendChild(row);
      li.appendChild(childUl);
      renderWebkitNode(node.dirs[name], subPath, childUl);
      row.addEventListener("click", (e) => {
        e.stopPropagation();
        const opening = childUl.hidden;
        childUl.hidden = !opening;
        row.classList.toggle("is-open", opening);
      });
      containerUl.appendChild(li);
    }
    for (let f = 0; f < fileNames.length; f++) {
      const name = fileNames[f];
      const file = node.files[name];
      const fullPath = joinRel(basePath, name);
      const li = document.createElement("li");
      li.className = "chat-tree-node";
      const row = document.createElement("div");
      row.className = "ide-tree-item ide-tree-file chat-tree-row";
      row.textContent = name;
      row.addEventListener("click", async (e) => {
        e.stopPropagation();
        try {
          const text = await file.text();
          selectRow(row);
          state.lastFileHandle = null;
          state.openRelPath = fullPath;
          openFilePanel(fullPath, text, "none");
        } catch (err) {
          window.alert(err && err.message ? err.message : "读取失败");
        }
      });
      li.appendChild(row);
      containerUl.appendChild(li);
    }
  }

  function renderWebkitRoot(files, folderName) {
    const tree = document.getElementById("chat-explorer-tree");
    if (!tree) return;
    tree.innerHTML = "";
    const rootUl = document.createElement("ul");
    rootUl.className = "ide-tree-list chat-tree-root";
    tree.appendChild(rootUl);
    const node = buildWebkitTree(files);
    renderWebkitNode(node, "", rootUl);
    setStatus("本地文件夹 · " + folderName + "（共 " + files.length + " 项）· 通过文件夹选择时无法在浏览器内写回磁盘");
  }

  function renderSingleFileTree(fileName) {
    const tree = document.getElementById("chat-explorer-tree");
    if (!tree) return;
    tree.innerHTML = "";
    const rootUl = document.createElement("ul");
    rootUl.className = "ide-tree-list chat-tree-root";
    const li = document.createElement("li");
    li.className = "chat-tree-node";
    const row = document.createElement("div");
    row.className = "ide-tree-item ide-tree-file chat-tree-row";
    row.textContent = fileName;
    rootUl.appendChild(li);
    li.appendChild(row);
    tree.appendChild(rootUl);
  }

  function getOpenFileContent() {
    if (typeof window.cctChatWorkspaceGetDoc === "function") {
      const t = window.cctChatWorkspaceGetDoc();
      if (t !== null) return t;
    }
    const ed = document.getElementById("chat-file-editor");
    return ed ? ed.value : "";
  }

  async function saveCurrentFile() {
    if (state.saveKind === "ai-review") {
      window.alert("当前为 AI 修改预览，请使用上方「保存全部」写入磁盘，或关闭中间栏预览。");
      return;
    }
    const content = getOpenFileContent();
    if (state.saveKind === "server" && state.openRelPath) {
      const { r, data } = await apiJson("/api/workspace/file", {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ path: state.openRelPath, content }),
      });
      if (!data.ok) {
        window.alert(data.error || "保存失败 " + r.status);
        return;
      }
      setStatus("已保存 · " + state.openRelPath);
      return;
    }
    if (state.saveKind === "handle" && state.lastFileHandle) {
      try {
        const w = await state.lastFileHandle.createWritable();
        await w.write(content);
        await w.close();
        setStatus("已写入本机文件");
      } catch (err) {
        window.alert(err && err.message ? err.message : "保存失败");
      }
      return;
    }
    window.alert("当前来源不支持保存（请使用 File System Access 打开文件，或服务端工作区）");
  }

  function resetWorkspaceState() {
    stopServerTreePolling();
    hideServerExplorerCtxMenu();
    state.mode = "none";
    state.serverTreeScope = "";
    state.lastDirHandle = null;
    state.webkitTree = null;
    state.serverRootLabel = "";
    window.cctWorkspace.lastFile = null;
    window.cctWorkspace.dirHandle = null;
    window.cctWorkspace.folderLabel = "";
    window.cctWorkspace.folderFileCount = 0;
    closeFilePanel();
  }

  function closeWorkspace() {
    resetWorkspaceState();
    hideExplorerUi();
  }

  async function onPickFolder() {
    const wdir = document.getElementById("workspace-folder-input");
    if (typeof window.showDirectoryPicker === "function") {
      try {
        const dir = await window.showDirectoryPicker();
        resetWorkspaceState();
        state.mode = "local-handle";
        state.lastDirHandle = dir;
        window.cctWorkspace.dirHandle = dir;
        window.cctWorkspace.folderLabel = dir.name;
        window.cctWorkspace.folderFileCount = 0;
        showExplorer();
        renderHandleRoot(dir, dir.name);
        return;
      } catch (err) {
        if (err && err.name === "AbortError") return;
      }
    }
    wdir && wdir.click();
  }

  function onWebkitFolderChange(list) {
    if (!list || !list.length) return;
    const first = list[0];
    const rel = first.webkitRelativePath || "";
    const folderName = rel ? rel.split("/")[0] : "文件夹";
    resetWorkspaceState();
    state.mode = "local-webkit";
    window.cctWorkspace.dirHandle = null;
    window.cctWorkspace.folderLabel = folderName;
    window.cctWorkspace.folderFileCount = list.length;
    showExplorer();
    renderWebkitRoot(list, folderName);
  }

  async function onPickFile() {
    if (typeof window.showOpenFilePicker === "function") {
      try {
        const [handle] = await window.showOpenFilePicker({ multiple: false });
        const file = await handle.getFile();
        const text = await file.text();
        resetWorkspaceState();
        state.mode = "local-file";
        window.cctWorkspace.lastFile = file;
        window.cctWorkspace.dirHandle = null;
        showExplorer();
        renderSingleFileTree(file.name);
        state.lastFileHandle = handle;
        state.openRelPath = file.name;
        openFilePanel(file.name, text, "handle");
        setStatus("本地文件 · 可保存到原路径（若浏览器授予写入权限）");
        return;
      } catch (err) {
        if (err && err.name === "AbortError") return;
      }
    }
    document.getElementById("workspace-file-input")?.click();
  }

  function onLegacyFileInput(file) {
    if (!file) return;
    resetWorkspaceState();
    state.mode = "local-file";
    window.cctWorkspace.lastFile = file;
    window.cctWorkspace.dirHandle = null;
    showExplorer();
    renderSingleFileTree(file.name);
    file.text().then(
      (text) => {
        state.lastFileHandle = null;
        state.openRelPath = file.name;
        openFilePanel(file.name, text, "none");
        setStatus("本地文件 · 传统选择器无法写回，请改用支持 File System Access 的浏览器或使用服务端工作区");
      },
      () => window.alert("无法读取文件")
    );
  }

  function onServerWorkspace() {
    resetWorkspaceState();
    state.mode = "server";
    const tid = typeof window.cctGetActiveThreadId === "function" ? window.cctGetActiveThreadId() : "main";
    setExplorerScopeFromThread(tid);
    showExplorer();
    renderServerRoot();
    startServerTreePolling();
    const base = state.serverRootLabel ? "服务端 · " + state.serverRootLabel : "服务端工作区";
    const sc = state.serverTreeScope;
    setStatus(sc ? base + " · 本对话目录：" + sc + "（列表已限定于此文件夹）" : base);
  }

  window.cctChatProjectServerPeek = function () {
    if (state.mode !== "server") return null;
    return {
      scope: state.serverTreeScope || "",
      openRelPath: state.openRelPath || "",
      rootLabel: state.serverRootLabel || "",
      saveKind: state.saveKind || "",
    };
  };

  async function probeServerWorkspace() {
    const card = document.getElementById("card-server-ws");
    if (!card) return;
    const { data } = await apiJson("/api/workspace/status");
    if (data.ok && data.enabled) {
      card.hidden = false;
      state.serverRootLabel = data.root || "";
    } else {
      card.hidden = true;
    }
  }

  function onExternalWrite(path) {
    if (state.mode !== "server" || !state.explorerVisible) return;
    const norm = typeof path === "string" ? path.replace(/\\/g, "/") : "";
    const openNorm = (state.openRelPath || "").replace(/\\/g, "/");
    if (norm && openNorm === norm) {
      openServerFile(norm, null);
    }
    rebuildServerTreeRoot();
  }

  window.cctChatProject = {
    isExplorerOpen() {
      return state.explorerVisible;
    },
  };

  /** 供聊天接口附带「当前打开文件」全文，便于模型结合代码回答 */
  window.cctChatProjectGetAiContext = function () {
    if (!state.explorerVisible) return null;
    const rel = (state.openRelPath || state.openLabel || "").trim();
    if (!rel) return null;
    const content = getOpenFileContent();
    if (content == null || !String(content).trim()) return null;
    return {
      relPath: state.openRelPath || "",
      label: state.openLabel || state.openRelPath || "",
      content: String(content),
    };
  };

  window.cctChatProjectOnExternalWrite = onExternalWrite;

  /**
   * 永久删除服务端工作区内文件（需用户确认）。paths 为相对 workspace_root 的路径。
   */
  window.cctChatProjectConfirmDeletePaths = async function (pathsIn) {
    const raw = (pathsIn || [])
      .map((p) => normWorkspacePath(String(p)))
      .filter(Boolean);
    const uniq = [...new Set(raw)];
    if (!uniq.length) return { ok: true, count: 0 };
    if (state.mode !== "server" || !state.explorerVisible) {
      window.alert("删除文件需先在左侧打开「服务端工作区」。");
      return { ok: false, error: "需要服务端工作区" };
    }
    const preview =
      uniq.slice(0, 18).join("\n") + (uniq.length > 18 ? "\n… 共 " + uniq.length + " 个路径" : "");
    if (!window.confirm("即将从服务端工作区永久删除以下文件：\n\n" + preview + "\n\n此操作不可撤销，确定删除？")) {
      return { ok: false, cancelled: true };
    }
    let okCount = 0;
    const errs = [];
    for (const p of uniq) {
      const { data } = await apiJson("/api/workspace/file?path=" + encodeURIComponent(p), {
        method: "DELETE",
      });
      if (data && data.ok) okCount++;
      else errs.push(p + ": " + ((data && data.error) || "失败"));
    }
    rebuildServerTreeRoot();
    setStatus("已删除 " + okCount + " 个文件");
    if (errs.length) window.alert("部分删除失败：\n" + errs.slice(0, 8).join("\n"));
    return { ok: true, count: okCount, errors: errs };
  };

  /**
   * 发送前根据 payload 中的 editor_path / workspace_bundle.files[] 推导会话目录锚点，
   * 解决「从未在左侧打开文件、仅靠快照路径」时历史会话无法定位子目录的问题。
   */
  window.cctChatProjectApplyAnchorFromPayload = function (payload) {
    if (!payload) return;
    const tid = String(payload.thread_id || "").trim() || "main";
    let rel = "";
    if (payload.editor_path && String(payload.editor_path).trim()) {
      rel = deriveAnchorRelFromOpenPath(String(payload.editor_path));
    }
    if (!rel && payload.workspace_bundle && String(payload.workspace_bundle).length > 12) {
      try {
        const o = JSON.parse(payload.workspace_bundle);
        const fs = o && Array.isArray(o.files) ? o.files : [];
        for (let i = 0; i < fs.length; i++) {
          const p = fs[i] && (fs[i].path || fs[i].relPath);
          if (p) {
            rel = deriveAnchorRelFromOpenPath(String(p));
            if (rel) break;
          }
        }
      } catch (_) {}
    }
    if (rel) setStoredAnchorRelForThread(tid, rel);
  };

  /**
   * 根据已加载的历史消息推断会话目录锚点（优先最近的助手消息），写入 localStorage。
   * 用于从未发过「带快照的请求」时仅靠历史正文仍能定位 calculate/ 等子目录。
   */
  window.cctChatProjectInferAnchorFromMessages = function (threadId, messages) {
    const tid = String(threadId || "").trim() || "main";
    if (!messages || !messages.length) return getStoredAnchorRelForThread(tid);
    const collect =
      typeof window.cctCollectPathsForWorkspaceInference === "function"
        ? window.cctCollectPathsForWorkspaceInference
        : null;
    for (let mi = messages.length - 1; mi >= 0; mi--) {
      const m = messages[mi];
      if (!m || m.role !== "assistant") continue;
      const content = m.content || "";
      const paths = collect ? collect(content) : [];
      for (let pi = 0; pi < paths.length; pi++) {
        const rel = deriveAnchorRelFromOpenPath(String(paths[pi]));
        if (rel) {
          setStoredAnchorRelForThread(tid, rel);
          return rel;
        }
      }
    }
    return getStoredAnchorRelForThread(tid);
  };

  window.cctChatProjectRememberThreadWorkspaceAnchor = function (explicitThreadId) {
    const tid =
      explicitThreadId != null && String(explicitThreadId).trim() !== ""
        ? String(explicitThreadId).trim()
        : typeof window.cctGetActiveThreadId === "function"
          ? window.cctGetActiveThreadId()
          : "main";
    if (state.mode !== "server" || !state.explorerVisible) return;
    const scoped = normWorkspacePath(state.serverTreeScope || "");
    if (scoped) {
      setStoredAnchorRelForThread(tid, scoped);
      return;
    }
    const rel = deriveAnchorRelFromOpenPath(state.openRelPath || "");
    if (!rel) return;
    setStoredAnchorRelForThread(tid, rel);
  };

  window.cctChatProjectGetThreadWorkspaceAnchor = function (threadId) {
    return getStoredAnchorRelForThread(
      threadId || (typeof window.cctGetActiveThreadId === "function" ? window.cctGetActiveThreadId() : "main")
    );
  };

  window.cctChatProjectSetThreadWorkspaceAnchor = function (threadId, relDir) {
    const tid = String(threadId || "").trim() || "main";
    const norm = normWorkspacePath(relDir || "");
    if (!norm) return "";
    persistThreadAnchor(tid, norm);
    return norm;
  };

  window.cctChatProjectEnsureServerWorkspaceForThread = async function (threadId) {
    const tid = threadId || "main";
    const { data } = await apiJson("/api/workspace/status");
    if (!data || !data.ok || !data.enabled) return;
    await hydrateThreadAnchorFromServer(tid);
    await probeServerWorkspace();
    resetWorkspaceState();
    state.mode = "server";
    setExplorerScopeFromThread(tid);
    showExplorer();
    renderServerRoot();
    document.getElementById("app-root")?.classList.add("gpt-workspace-open");
    const base = state.serverRootLabel ? "服务端 · " + state.serverRootLabel : "服务端工作区";
    const sc = state.serverTreeScope;
    setStatus(sc ? base + " · 本对话目录：" + sc + "（仅在此文件夹操作）" : base);
  };

  /**
   * 将 CCT_APPLY 落盘到工作区根下以 label 命名的子文件夹（根目录：已配置 workspace_root 则用其值，否则为 D:\\），
   * 并进入三栏「服务端工作区」且打开刚写入的主文件。
   */
  window.cctChatProjectMaterializeIde = async function (payloadOrList, label) {
    const list = Array.isArray(payloadOrList)
      ? payloadOrList.filter((x) => x && typeof x.path === "string" && x.path && typeof x.content === "string")
      : payloadOrList && typeof payloadOrList.path === "string" && payloadOrList.path
        ? [payloadOrList]
        : [];
    if (!list.length) {
      window.alert("没有可导出的文件信息。");
      return;
    }
    const first = list[0];
    const content = typeof first.content === "string" ? first.content : "";
    const bodyObj = {
      label: (label && String(label).trim()) || "CCT_Export",
      path: first.path,
      content,
    };
    if (list.length >= 2) {
      bodyObj.path2 = list[1].path;
      bodyObj.content2 = typeof list[1].content === "string" ? list[1].content : "";
    }
    const body = JSON.stringify(bodyObj);
    try {
      const r = await fetch("/api/chat/materialize-ide", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        credentials: "include",
        body,
      });
      const data = await r.json().catch(() => ({}));
      if (!data.ok) {
        window.alert(data.error || "导出失败 " + r.status);
        return;
      }
      const tid = typeof window.cctGetActiveThreadId === "function" ? window.cctGetActiveThreadId() : "main";
      const rel = (data.relPath || "").replace(/\\/g, "/");
      const anch = deriveAnchorRelFromOpenPath(rel);
      if (anch) setStoredAnchorRelForThread(tid, anch);
      await probeServerWorkspace();
      onServerWorkspace();
      await new Promise((res) => setTimeout(res, 80));
      if (rel) await openServerFile(rel, null);
      else window.alert("已导出，但未返回相对路径。");
    } catch (e) {
      window.alert(e && e.message ? e.message : "网络错误");
    }
  };

  window.cctChatProjectRebuildTree = function () {
    rebuildServerTreeRoot();
  };

  window.cctChatProjectPauseForOtherPanels = function () {
    document.getElementById("app-root")?.classList.remove("gpt-workspace-open");
  };

  window.cctChatProjectResumeIfWorkspace = function () {
    if (!state.explorerVisible) return;
    document.getElementById("app-root")?.classList.add("gpt-workspace-open");
  };

  window.cctChatProjectBoot = function () {
    document.getElementById("card-open-folder")?.addEventListener("click", onPickFolder);
    document.getElementById("card-open-file")?.addEventListener("click", onPickFile);
    document.getElementById("card-server-ws")?.addEventListener("click", onServerWorkspace);
    document.getElementById("chat-explorer-close")?.addEventListener("click", closeWorkspace);
    document.getElementById("chat-project-explorer")?.addEventListener("contextmenu", onChatExplorerProjectContextCapture, true);
    document.getElementById("chat-file-save")?.addEventListener("click", () => saveCurrentFile());
    document.getElementById("chat-file-close")?.addEventListener("click", closeFilePanel);
    document.getElementById("chat-ai-review-keep")?.addEventListener("click", async () => {
      await window.cctChatProjectCommitAiReview();
    });
    document.getElementById("chat-ai-review-done")?.addEventListener("click", () => {
      closeFilePanel();
      setStatus("");
    });

    const wf = document.getElementById("workspace-file-input");
    wf?.addEventListener("change", (e) => {
      const file = e.target.files && e.target.files[0];
      e.target.value = "";
      if (file) onLegacyFileInput(file);
    });

    const wdir = document.getElementById("workspace-folder-input");
    wdir?.addEventListener("change", (e) => {
      const list = e.target.files;
      e.target.value = "";
      if (list && list.length) onWebkitFolderChange(list);
    });

    window.cctOnActiveThreadChanged = function (tid) {
      if (!state.explorerVisible || state.mode !== "server") return;
      setExplorerScopeFromThread(tid || "main");
      renderServerRoot();
      const base = state.serverRootLabel ? "服务端 · " + state.serverRootLabel : "服务端工作区";
      const sc = state.serverTreeScope;
      setStatus(sc ? base + " · 本对话目录：" + sc : base);
    };

    probeServerWorkspace();
    setupSplitDrag();

    document.addEventListener("click", (e) => {
      if (e.target.closest("#chat-explorer-ctx")) return;
      hideServerExplorerCtxMenu();
    });
    document.addEventListener("keydown", (e) => {
      if (e.key === "Escape") hideServerExplorerCtxMenu();
    });
  };
})();
