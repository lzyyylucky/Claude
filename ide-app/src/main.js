import { Compartment, EditorState } from "@codemirror/state";
import { defaultHighlightStyle, syntaxHighlighting } from "@codemirror/language";
import {
  EditorView,
  keymap,
  lineNumbers,
  highlightActiveLineGutter,
  highlightActiveLine,
} from "@codemirror/view";
import {
  defaultKeymap,
  historyKeymap,
  history,
  indentWithTab,
} from "@codemirror/commands";
import { oneDark } from "@codemirror/theme-one-dark";
import { javascript } from "@codemirror/lang-javascript";
import { json } from "@codemirror/lang-json";
import { html } from "@codemirror/lang-html";
import { css } from "@codemirror/lang-css";
import { markdown } from "@codemirror/lang-markdown";
import { python } from "@codemirror/lang-python";
import { cpp } from "@codemirror/lang-cpp";

let editorView = null;
let chatWorkspaceEditorView = null;
let currentPath = "";
let currentRel = ""; // relative to workspace root, posix-style
let workspaceRootDisplay = "";

const themeCompartment = new Compartment();

function isAppLightTheme() {
  return document.documentElement.getAttribute("data-theme") === "light";
}

function cmDocumentDarkBundle() {
  return [
    oneDark,
    EditorView.theme(
      {
        ".cm-scroller": {
          overflow: "auto",
          scrollbarWidth: "thin",
          scrollbarColor: "rgba(255,255,255,0.2) transparent",
        },
        ".cm-scroller::-webkit-scrollbar": { width: "4px", height: "4px" },
        ".cm-scroller::-webkit-scrollbar-track": { background: "transparent" },
        ".cm-scroller::-webkit-scrollbar-thumb": {
          background: "rgba(255,255,255,0.2)",
          borderRadius: "99px",
        },
        ".cm-scroller::-webkit-scrollbar-thumb:hover": {
          background: "rgba(255,255,255,0.32)",
        },
        ".cm-scroller::-webkit-scrollbar-corner": { background: "transparent" },
      },
      { dark: true }
    ),
  ];
}

function cmDocumentLightBundle() {
  return [
    syntaxHighlighting(defaultHighlightStyle, { fallback: true }),
    EditorView.theme(
      {
        "&": { height: "100%" },
        "&.cm-editor": { background: "#ffffff", color: "#1a1a1a" },
        ".cm-gutters": {
          background: "#f2f2f7",
          color: "#8e8e93",
          border: "none",
          borderRight: "1px solid #d1d1d6",
        },
        ".cm-activeLineGutter": { background: "rgba(0,113,227,0.08)" },
        ".cm-lineNumbers .cm-gutterElement": { color: "#aeaeb2" },
        ".cm-scroller": {
          background: "#ffffff",
          overflow: "auto",
          scrollbarWidth: "thin",
          scrollbarColor: "rgba(0,0,0,0.22) transparent",
        },
        ".cm-scroller::-webkit-scrollbar": { width: "4px", height: "4px" },
        ".cm-scroller::-webkit-scrollbar-track": { background: "transparent" },
        ".cm-scroller::-webkit-scrollbar-thumb": {
          background: "rgba(0,0,0,0.2)",
          borderRadius: "99px",
        },
        ".cm-scroller::-webkit-scrollbar-thumb:hover": {
          background: "rgba(0,0,0,0.32)",
        },
        ".cm-scroller::-webkit-scrollbar-corner": { background: "transparent" },
        ".cm-content": { caretColor: "#0071e3" },
        ".cm-cursor, .cm-dropCursor": { borderLeftColor: "#0071e3" },
        ".cm-activeLine": { background: "rgba(0,113,227,0.05)" },
      },
      { dark: false }
    ),
  ];
}

function buildThemeExtensions() {
  return isAppLightTheme() ? cmDocumentLightBundle() : cmDocumentDarkBundle();
}

function joinRel(parent, name) {
  if (!parent) return name;
  return parent.replace(/\/+$/, "") + "/" + name;
}

function langForFilename(name) {
  const lower = name.toLowerCase();
  if (lower.endsWith(".tsx"))
    return javascript({ jsx: true, typescript: true });
  if (lower.endsWith(".ts") || lower.endsWith(".mts") || lower.endsWith(".cts"))
    return javascript({ jsx: false, typescript: true });
  if (lower.endsWith(".jsx")) return javascript({ jsx: true, typescript: false });
  if (lower.endsWith(".js") || lower.endsWith(".mjs") || lower.endsWith(".cjs"))
    return javascript({ jsx: false, typescript: false });
  if (lower.endsWith(".json")) return json();
  if (lower.endsWith(".html") || lower.endsWith(".htm")) return html();
  if (lower.endsWith(".css")) return css();
  if (lower.endsWith(".md")) return markdown();
  if (lower.endsWith(".py")) return python();
  if (
    lower.endsWith(".cpp") ||
    lower.endsWith(".cc") ||
    lower.endsWith(".cxx") ||
    lower.endsWith(".h") ||
    lower.endsWith(".hpp") ||
    lower.endsWith(".c")
  )
    return cpp();
  return [];
}

async function apiJson(url, opts) {
  const r = await fetch(url, { credentials: "include", ...opts });
  const data = await r.json().catch(() => ({}));
  return { r, data };
}

function destroyEditor() {
  if (editorView) {
    editorView.destroy();
    editorView = null;
  }
}

/** 与对话区一致：细滚动条 + 随 html[data-theme] 深/浅切换 */
function cmBaseExtensions(pathLabel) {
  const ext = langForFilename(pathLabel);
  return [
    history(),
    themeCompartment.of(buildThemeExtensions()),
    lineNumbers(),
    highlightActiveLineGutter(),
    highlightActiveLine(),
    keymap.of([...defaultKeymap, ...historyKeymap, indentWithTab]),
    ext,
    EditorView.lineWrapping,
    EditorView.theme({
      "&": { height: "100%" },
      ".cm-editor": {
        fontFamily: "var(--cct-code-font)",
        fontSize: "var(--cct-code-size)",
        lineHeight: 1.65,
        fontWeight: "400",
        WebkitFontSmoothing: "antialiased",
        MozOsxFontSmoothing: "grayscale",
        textRendering: "optimizeLegibility",
        fontFeatureSettings: '"calt" 1, "liga" 1',
      },
      ".cm-content": { fontFamily: "inherit" },
      ".cm-gutters": {
        fontFamily: "var(--cct-code-font)",
        fontWeight: "400",
      },
    }),
  ];
}

function reapplyCmTheme() {
  const eff = themeCompartment.reconfigure(buildThemeExtensions());
  if (editorView) editorView.dispatch({ effects: eff });
  if (chatWorkspaceEditorView) chatWorkspaceEditorView.dispatch({ effects: eff });
}

window.cctIdeNotifyColorScheme = reapplyCmTheme;

function mountEditor(parent, text, pathLabel) {
  destroyEditor();
  const state = EditorState.create({
    doc: text,
    extensions: cmBaseExtensions(pathLabel),
  });
  editorView = new EditorView({ state, parent });
}

function destroyChatWorkspaceEditor() {
  if (chatWorkspaceEditorView) {
    chatWorkspaceEditorView.destroy();
    chatWorkspaceEditorView = null;
  }
}

/** 供「新项目」中间栏挂载：语法高亮 + 与 IDE 相同的语言推断 */
function mountChatWorkspaceEditor(parent, text, pathLabel) {
  if (!parent) return false;
  destroyChatWorkspaceEditor();
  parent.innerHTML = "";
  const label = pathLabel || "untitled";
  const state = EditorState.create({
    doc: text || "",
    extensions: cmBaseExtensions(label),
  });
  chatWorkspaceEditorView = new EditorView({ state, parent });
  return true;
}

window.cctChatWorkspaceMountEditor = mountChatWorkspaceEditor;
window.cctChatWorkspaceGetDoc = () =>
  chatWorkspaceEditorView ? chatWorkspaceEditorView.state.doc.toString() : null;
window.cctChatWorkspaceDestroyEditor = destroyChatWorkspaceEditor;

async function openFile(relPath) {
  const q = encodeURIComponent(relPath);
  const { r, data } = await apiJson("/api/workspace/file?path=" + q);
  if (!data.ok) {
    alert(data.error || "读取失败 " + r.status);
    return;
  }
  currentRel = relPath;
  currentPath = relPath;
  const edHost = document.getElementById("ide-editor-host");
  if (!edHost) return;
  mountEditor(edHost, data.content || "", relPath.split("/").pop() || relPath);
  const cap = document.getElementById("ide-open-file-label");
  if (cap) cap.textContent = relPath;
}

async function saveCurrent() {
  if (!currentRel || !editorView) {
    alert("请先打开文件");
    return;
  }
  const content = editorView.state.doc.toString();
  const { r, data } = await apiJson("/api/workspace/file", {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ path: currentRel, content }),
  });
  if (!data.ok) {
    alert(data.error || "保存失败 " + r.status);
    return;
  }
  const hint = document.getElementById("ide-status-hint");
  if (hint) {
    hint.textContent = "已保存 " + currentRel;
    setTimeout(() => {
      if (hint.textContent.startsWith("已保存")) hint.textContent = "";
    }, 2500);
  }
}

function parentRelPath(relDir) {
  if (!relDir) return "";
  const i = relDir.lastIndexOf("/");
  return i <= 0 ? "" : relDir.slice(0, i);
}

async function refreshTree(relDir) {
  const tree = document.getElementById("ide-file-tree");
  if (!tree) return;
  tree.innerHTML = "";
  if (relDir) {
    const up = document.createElement("div");
    up.className = "ide-tree-up";
    up.textContent = "⬆ 上级目录";
    up.onclick = () => refreshTree(parentRelPath(relDir));
    tree.appendChild(up);
  }
  const q = relDir ? "?path=" + encodeURIComponent(relDir) : "";
  const { r, data } = await apiJson("/api/workspace/list" + q);
  if (!data.ok) {
    tree.textContent = data.error || "无法列出目录";
    return;
  }
  const ul = document.createElement("ul");
  ul.className = "ide-tree-list";
  const entries = data.entries || [];
  entries.sort((a, b) => {
    if (a.type !== b.type) return a.type === "dir" ? -1 : 1;
    return a.name.localeCompare(b.name);
  });
  for (const e of entries) {
    const li = document.createElement("li");
    li.className = "ide-tree-item ide-tree-" + e.type;
    const full = joinRel(relDir, e.name);
    if (e.type === "dir") {
      li.textContent = "▸ " + e.name;
      li.onclick = (ev) => {
        ev.stopPropagation();
        refreshTree(full);
      };
    } else {
      li.textContent = e.name;
      li.onclick = (ev) => {
        ev.stopPropagation();
        openFile(full);
      };
    }
    ul.appendChild(li);
  }
  tree.appendChild(ul);
}

async function bootIde() {
  const root = document.getElementById("ide-root");
  if (!root || root.dataset.ideReady === "1") return;
  root.dataset.ideReady = "1";
  root.innerHTML = `
    <div class="ide-shell">
      <div class="ide-toolbar">
        <span class="ide-toolbar-title">代码工作区</span>
        <span id="ide-ws-root" class="ide-ws-root"></span>
        <span class="ide-toolbar-actions">
          <button type="button" class="ide-btn ide-btn-ghost" id="ide-btn-refresh-tree">刷新树</button>
          <button type="button" class="ide-btn" id="ide-btn-save">保存</button>
        </span>
      </div>
      <p id="ide-status-hint" class="ide-status-hint"></p>
      <div class="ide-split">
        <aside class="ide-sidebar" id="ide-file-tree" aria-label="文件树"></aside>
        <div class="ide-editor-panel">
          <div class="ide-editor-caption" id="ide-open-file-label">未打开文件</div>
          <div class="ide-editor-host" id="ide-editor-host"></div>
        </div>
      </div>
      <p class="ide-ai-hint">AI 协议：助手可在回复末尾附加独立一行 <code>CCT_APPLY:</code> 紧接 JSON（单行或换行后）：<code>{"path":"相对路径","content":"全文"}</code>，将自动写入工作区并打开。</p>
    </div>
  `;

  const { data } = await apiJson("/api/workspace/status");
  if (!data.ok) {
    root.querySelector(".ide-shell").innerHTML =
      `<p class="ide-disabled">${data.error || "工作区不可用"}。请在 <code>.cct-cn/config.json</code> 配置 <code>workspace_root</code> 指向本机项目目录，或使用 <code>cct-cn serve --workspace &lt;目录&gt;</code> 后刷新。</p>`;
    return;
  }
  workspaceRootDisplay = data.root || "";
  const wr = document.getElementById("ide-ws-root");
  if (wr) wr.textContent = workspaceRootDisplay;

  document.getElementById("ide-btn-save")?.addEventListener("click", saveCurrent);
  document.getElementById("ide-btn-refresh-tree")?.addEventListener("click", () => refreshTree(""));

  await refreshTree("");
  mountEditor(document.getElementById("ide-editor-host"), "// 从左侧选择文件\n", "untitled");
  currentRel = "";
}

window.cctIdeApplyFromAssistant = async function (obj) {
  if (!obj || typeof obj.path !== "string" || !obj.path) return false;
  const content = typeof obj.content === "string" ? obj.content : "";
  const { data } = await apiJson("/api/workspace/apply", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ path: obj.path, content }),
  });
  if (!data.ok) {
    console.warn("cctIdeApplyFromAssistant", data.error);
    return false;
  }
  await openFile(obj.path);
  return true;
};

window.cctIdeGetCurrentPath = () => currentRel;
window.cctIdeBoot = bootIde;

function tryBoot() {
  if (document.getElementById("ide-root")) bootIde();
}

if (document.readyState === "loading") {
  document.addEventListener("DOMContentLoaded", tryBoot);
} else {
  tryBoot();
}
