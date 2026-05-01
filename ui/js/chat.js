/** 解析助手回复末尾的 CCT_APPLY: JSON，供写入服务端工作区 */
function cctParseCctApply(reply) {
  if (!reply || typeof reply !== "string") return null;
  const tag = "CCT_APPLY:";
  const i = reply.lastIndexOf(tag);
  if (i < 0) return null;
  let raw = reply.slice(i + tag.length).trim();
  if (raw.startsWith("```")) {
    raw = raw
      .replace(/^```(?:json)?\s*/i, "")
      .replace(/\s*```\s*$/s, "")
      .trim();
  }
  try {
    const o = JSON.parse(raw);
    if (o && typeof o.path === "string") return o;
  } catch (_) {}
  return null;
}

/** 解析助手回复末尾 CCT_WORKSPACE: JSON，用于自动写回工作区（类 Cursor 多文件补丁） */
function cctParseWorkspaceWrites(reply) {
  if (!reply || typeof reply !== "string") return [];
  const tag = "CCT_WORKSPACE:";
  const i = reply.lastIndexOf(tag);
  if (i < 0) return [];
  let raw = reply.slice(i + tag.length).trim();
  if (raw.startsWith("```")) {
    raw = raw
      .replace(/^```(?:json)?\s*/i, "")
      .replace(/\s*```\s*$/, "")
      .trim();
  }
  try {
    const o = JSON.parse(raw);
    if (o && Array.isArray(o.writes))
      return o.writes.filter((x) => x && typeof x.path === "string" && x.path && typeof x.content === "string");
  } catch (_) {}
  return [];
}

function cctTryParseWritesObject(o) {
  if (!o || !Array.isArray(o.writes)) return [];
  return o.writes.filter(
    (x) => x && typeof x.path === "string" && x.path && typeof x.content === "string"
  );
}

/** 从 ``` / ```json 等围栏内解析 `{"writes":[...]}`（不依赖子串检测；从最后一个围栏往前试） */
function cctParseWorkspaceWritesFromFenced(text) {
  if (!text || typeof text !== "string") return [];
  const re = /```(?:json|JSON|javascript|JS)?\s*\r?\n?([\s\S]*?)```/g;
  const bodies = [];
  let m;
  while ((m = re.exec(text)) !== null) bodies.push((m[1] || "").trim());
  for (let bi = bodies.length - 1; bi >= 0; bi--) {
    const raw = bodies[bi];
    if (!raw || raw.charAt(0) !== "{") continue;
    try {
      const o = JSON.parse(raw);
      const w = cctTryParseWritesObject(o);
      if (w.length) return w;
    } catch (_) {}
  }
  return [];
}

/**
 * 从正文/思考末尾提取裸 JSON：`{ "writes": [...] }`（未写 CCT_WORKSPACE、也未用围栏时）。
 * 使用括号配对，避免 content 字段内含 `}` 时被截断。
 */
function cctExtractJsonObjectAt(s, start) {
  if (!s || start < 0 || start >= s.length || s.charAt(start) !== "{") return null;
  let depth = 0;
  let inStr = false;
  let esc = false;
  for (let i = start; i < s.length; i++) {
    const c = s.charAt(i);
    if (inStr) {
      if (esc) esc = false;
      else if (c === "\\") esc = true;
      else if (c === '"') inStr = false;
      continue;
    }
    if (c === '"') {
      inStr = true;
      continue;
    }
    if (c === "{") depth++;
    else if (c === "}") {
      depth--;
      if (depth === 0) {
        const slice = s.slice(start, i + 1);
        try {
          return JSON.parse(slice);
        } catch (_) {
          return null;
        }
      }
    }
  }
  return null;
}

function cctParseWorkspaceWritesBare(merged) {
  if (!merged || typeof merged !== "string") return [];
  const re = /\{\s*"writes"\s*:/g;
  let hit;
  let last = -1;
  while ((hit = re.exec(merged)) !== null) last = hit.index;
  if (last < 0) return [];
  const o = cctExtractJsonObjectAt(merged, last);
  const w = cctTryParseWritesObject(o);
  return w.length ? w : [];
}

/**
 * 合并「正文」与「思考」再解析：智谱流式里 CCT_WORKSPACE 可能只在 reasoning 通道，此前会导致磁盘不写。
 */
function cctCollectWorkspaceWrites(content, thinking) {
  const blobs = [];
  if (content) blobs.push(content);
  if (thinking) blobs.push(thinking);
  if (content && thinking) blobs.push(content + "\n" + thinking);
  for (let b = 0; b < blobs.length; b++) {
    const w = cctParseWorkspaceWrites(blobs[b]);
    if (w.length) return w;
  }
  const merged = (content || "") + "\n" + (thinking || "");
  const w2 = cctParseWorkspaceWritesFromFenced(merged);
  if (w2.length) return w2;
  return cctParseWorkspaceWritesBare(merged);
}

/** 聊天区展示用：去掉末尾机器可读块（CCT_APPLY / CCT_WORKSPACE），避免把 JSON 当正文渲染 */
function cctStripAgentMarkersForDisplay(reply) {
  if (!reply || typeof reply !== "string") return "";
  let s = reply;
  for (;;) {
    const ia = s.lastIndexOf("CCT_APPLY:");
    const iw = s.lastIndexOf("CCT_WORKSPACE:");
    const i = ia > iw ? ia : iw;
    if (i < 0) break;
    s = s.slice(0, i).replace(/\s+$/, "");
  }
  return s;
}

/** HTML 属性转义（围栏语言标记） */
function cctEscapeHtmlAttr(s) {
  const m = { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" };
  return String(s).replace(/[&<>"]/g, (c) => m[c] || c);
}

/**
 * 助手正文：按 ``` 围栏拆成「段落」与「代码块」，代码块用 pre+横向滚动，避免 pre-wrap+break-word 把长行切碎导致版面错乱。
 */
function cctRenderAssistantBodyHtml(raw) {
  const stripped = cctStripAgentMarkersForDisplay(String(raw || ""));
  const esc = (t) => {
    const m = { "&": "&amp;", "<": "&lt;", ">": "&gt;", "\"": "&quot;" };
    return String(t).replace(/[&<>"]/g, (c) => m[c] || c);
  };
  const parts = stripped.split("```");
  let html = "";
  for (let i = 0; i < parts.length; i++) {
    if (i % 2 === 0) {
      html += '<div class="bubble-prose">' + esc(parts[i]) + "</div>";
      continue;
    }
    let body = parts[i];
    if (body.startsWith("\r\n")) body = body.slice(2);
    else if (body.startsWith("\n")) body = body.slice(1);
    const nl = body.indexOf("\n");
    let lang = "";
    if (nl >= 0) {
      const firstLine = body.slice(0, nl).trim();
      if (/^[a-zA-Z0-9+#.\-]{1,48}$/.test(firstLine)) {
        lang = firstLine;
        body = body.slice(nl + 1);
      }
    } else if (body.length <= 48 && /^[a-zA-Z0-9+#.\-]+$/.test(body.trim())) {
      lang = body.trim();
      body = "";
    }
    html +=
      '<pre class="bubble-code-fence"' +
      (lang ? ' data-lang="' + cctEscapeHtmlAttr(lang) + '"' : "") +
      "><code>" +
      esc(body) +
      "</code></pre>";
  }
  return html;
}
window.cctRenderAssistantBodyHtml = cctRenderAssistantBodyHtml;

function cctGuessExportLabelFromDom() {
  const box = typeof cctChat !== "undefined" && cctChat.box ? cctChat.box : null;
  if (!box) return "CCT_Export";
  const mains = box.querySelectorAll(".bubble.user .bubble-main");
  const last = mains[mains.length - 1];
  const t = last ? String(last.textContent || "").trim() : "";
  if (t) return t.length > 48 ? t.slice(0, 48) : t;
  return "CCT_Export";
}

/** 从 fenced code 猜主文件名（无 CCT_APPLY 时把代码落盘到工作区） */
function cctGuessFilenameFromFence(lang, content) {
  const L = (lang || "").toLowerCase().trim();
  const head = (content || "").slice(0, 800);
  if (L.includes("html") || /<!DOCTYPE\s+html|<\s*html[\s>]/i.test(head)) return "index.html";
  if (L === "css" || L === "scss") return "styles.css";
  if (L === "json") return "data.json";
  if (L === "py" || L === "python") return "main.py";
  if (L === "ts" || L === "typescript") return "main.ts";
  if (L.includes("cpp") || L.includes("c++") || L === "cxx" || L === "cc") return "main.cpp";
  if (L === "h" || L.includes("hpp") || L.includes("header")) return "main.h";
  if (L === "md" || L.includes("markdown")) return "README.md";
  if (L === "js" || L === "javascript" || L === "jsx" || L === "tsx") return "script.js";
  return "snippet.txt";
}

/** 工作区相对路径规范化（与 chat-project 一致） */
function cctNormRelPath(p) {
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

function cctJoinUnderAnchor(anchor, fileName) {
  const f = cctNormRelPath(fileName);
  const a = cctNormRelPath(anchor);
  if (!f) return "";
  if (!a) return f;
  return a + "/" + f;
}

function cctExtractPathHintFromFenceContent(content) {
  const lines = String(content).split(/\r?\n/).slice(0, 18);
  for (let li = 0; li < lines.length; li++) {
    const line = lines[li];
    let m = line.match(/^\s*(?:\/\/|\/\*|\*|#|<!--)\s*(?:path|file|filepath)\s*[:：]\s*([\w.\/\\\-]+)/i);
    if (!m) m = line.match(/^\s*(?:file|path)\s*[:：]\s*([\w.\/\\\-]+)/i);
    if (m && m[1]) return m[1].trim().replace(/\\/g, "/");
  }
  return "";
}

/** 提取全部 ``` 围栏（排除机器可读 writes JSON） */
function cctExtractAllCodeFences(text) {
  if (!text || typeof text !== "string") return [];
  const re = /```([\w+#.-]*)\s*\r?\n([\s\S]*?)```/g;
  const out = [];
  let m;
  while ((m = re.exec(text)) !== null) {
    const lang = (m[1] || "").trim();
    const body = (m[2] || "").replace(/\s+$/, "").trim();
    if (body.length < 28) continue;
    const L = lang.toLowerCase();
    if (L === "json" || L === "jsonc") {
      const t = body.trimStart().slice(0, 1200);
      if (t.startsWith("{") && /"writes"\s*:/.test(t)) continue;
    }
    if ((L === "text" || L === "plaintext") && body.length < 120) continue;
    out.push({ lang, content: body });
  }
  return out.slice(0, 14);
}

/**
 * 无 CCT_WORKSPACE 时：从代码围栏合成 writes，路径优先读围栏首行 path:/file: 提示，否则落在「对话锚点目录」下并按语言猜测文件名。
 */
function cctBuildSyntheticWritesFromAssistantReply(fullReply, fullThink) {
  const merged = (fullReply || "") + "\n" + (fullThink || "");
  const stripped = cctStripAgentMarkersForDisplay(merged);
  const fences = cctExtractAllCodeFences(stripped);
  const anchor =
    typeof window.cctChatProjectGetThreadWorkspaceAnchor === "function"
      ? window.cctChatProjectGetThreadWorkspaceAnchor()
      : "";
  const stemUsed = new Map();
  const writes = [];
  for (let i = 0; i < fences.length; i++) {
    const f = fences[i];
    let path = "";
    const hint = cctExtractPathHintFromFenceContent(f.content);
    if (hint) {
      let hp = cctNormRelPath(hint.replace(/\\/g, "/"));
      if (hp.indexOf("/") < 0 && anchor) hp = cctJoinUnderAnchor(anchor, hp);
      path = hp;
    } else {
      let base = cctGuessFilenameFromFence(f.lang, f.content);
      const key = base;
      const n = stemUsed.get(key) || 0;
      stemUsed.set(key, n + 1);
      if (n > 0) {
        const mm = base.match(/^(.+)(\.[^.]+)$/);
        if (mm) base = mm[1] + "_" + (n + 1) + mm[2];
        else base = base + "_" + (n + 1);
      }
      path = anchor ? cctJoinUnderAnchor(anchor, base) : cctNormRelPath(base);
    }
    path = cctNormRelPath(path);
    if (!path) continue;
    writes.push({ path, content: f.content });
  }
  return writes;
}

/**
 * 从历史会话里的助手原文推断「涉及文件的相对路径」（不依赖当前锚点），供左侧树打开到对应子目录。
 */
window.cctCollectPathsForWorkspaceInference = function (assistantMarkdown) {
  const paths = [];
  const s = String(assistantMarkdown || "");
  if (typeof cctCollectWorkspaceWrites === "function") {
    cctCollectWorkspaceWrites(s, "").forEach((w) => {
      if (w && typeof w.path === "string" && w.path) paths.push(w.path);
    });
  }
  if (typeof cctParseCctApply === "function") {
    const a = cctParseCctApply(s);
    if (a && typeof a.path === "string" && a.path) paths.push(a.path);
  }
  const stripped = typeof cctStripAgentMarkersForDisplay === "function" ? cctStripAgentMarkersForDisplay(s) : s;
  if (typeof cctExtractAllCodeFences === "function" && typeof cctExtractPathHintFromFenceContent === "function") {
    const fences = cctExtractAllCodeFences(stripped);
    for (let fi = 0; fi < fences.length; fi++) {
      const hint = cctExtractPathHintFromFenceContent(fences[fi].content);
      if (hint) paths.push(cctNormRelPath(String(hint).replace(/\\/g, "/")));
    }
  }
  const seen = new Set();
  const out = [];
  for (let i = 0; i < paths.length; i++) {
    const n = cctNormRelPath(paths[i]);
    if (!n || seen.has(n)) continue;
    seen.add(n);
    out.push(n);
  }
  return out;
};

/** 解析助手末尾 CCT_DELETE: JSON 数组或 {"paths":[...]}，用于确认后删盘 */
function cctParseWorkspaceDeletes(reply) {
  if (!reply || typeof reply !== "string") return [];
  const tag = "CCT_DELETE:";
  const i = reply.lastIndexOf(tag);
  if (i < 0) return [];
  let raw = reply.slice(i + tag.length).trim();
  if (raw.startsWith("```")) {
    raw = raw
      .replace(/^```(?:json)?\s*/i, "")
      .replace(/\s*```\s*$/s, "")
      .trim();
  }
  try {
    const o = JSON.parse(raw);
    const arr = Array.isArray(o) ? o : o && Array.isArray(o.paths) ? o.paths : null;
    if (arr) return arr.map((x) => String(x).trim()).filter(Boolean);
  } catch (_) {}
  return [];
}

/** 取正文中最大的 ``` 代码块（兼容无换行、无语言标签等模型输出） */
function cctExtractLargestCodeFence(text) {
  if (!text || typeof text !== "string") return null;
  const re = /```([\w+#.-]*)\s*\r?\n([\s\S]*?)```/g;
  let best = "";
  let bestLang = "";
  let m;
  while ((m = re.exec(text)) !== null) {
    const lang = (m[1] || "").trim();
    const body = (m[2] || "").replace(/\s+$/, "").trim();
    if (body.length > best.length) {
      best = body;
      bestLang = lang;
    }
  }
  if (!best) {
    const parts = text.split(/```+/);
    for (let i = 1; i < parts.length; i += 2) {
      let chunk = parts[i] || "";
      chunk = chunk.replace(/^\r?\n/, "");
      const lines = chunk.split(/\r?\n/);
      let lang = "";
      let bodyStart = 0;
      if (lines.length && /^[\w+#.-]{1,40}$/.test(lines[0].trim())) {
        lang = lines[0].trim();
        bodyStart = 1;
      }
      const body = lines.slice(bodyStart).join("\n").replace(/\s+$/, "").trim();
      if (body.length > best.length) {
        best = body;
        bestLang = lang;
      }
    }
  }
  if (!best && text.trim().length > 80) {
    const st = text.trim();
    if (/^(function|const|let|var|import|class|export)\b/m.test(st) || /\bfunction\s*\(/.test(st))
      return { lang: "javascript", content: st };
  }
  if (!best) return null;
  return { lang: bestLang, content: best };
}

/**
 * 供「放入 IDE」：优先 CCT_APPLY；否则用去掉 CCT 后的正文里最大代码块。
 * @returns {{ path: string, content: string } | null}
 */
function cctBuildIdePayloadFromAssistantReply(rawReply) {
  if (!rawReply || typeof rawReply !== "string") return null;
  const ap = cctParseCctApply(rawReply);
  if (ap && typeof ap.path === "string" && ap.path && typeof ap.content === "string") {
    return { path: ap.path, content: ap.content };
  }
  const stripped = cctStripAgentMarkersForDisplay(rawReply);
  const ex = cctExtractLargestCodeFence(stripped);
  if (!ex || !ex.content) return null;
  return { path: cctGuessFilenameFromFence(ex.lang, ex.content), content: ex.content };
}

/** 线框双矩形复制图标（与全局白底按钮样式解耦后由 currentColor 着色） */
const CCT_COPY_ICON_SVG =
  '<svg class="bubble-copy-icon" width="18" height="18" viewBox="0 0 24 24" fill="none" xmlns="http://www.w3.org/2000/svg" aria-hidden="true"><rect x="8.5" y="8.5" width="12" height="12" rx="2.25" stroke="currentColor" stroke-width="1.85" stroke-linecap="round" stroke-linejoin="round"/><path d="M5.5 15.5v-9a2 2 0 0 1 2-2h9" stroke="currentColor" stroke-width="1.85" stroke-linecap="round" stroke-linejoin="round"/></svg>';

/** 与复制按钮同排：左栏+编辑区线框图标（放入工作区） */
const CCT_IDE_ICON_SVG =
  '<svg class="bubble-copy-icon" width="18" height="18" viewBox="0 0 24 24" fill="none" xmlns="http://www.w3.org/2000/svg" aria-hidden="true"><rect x="3" y="4" width="8" height="16" rx="1.5" stroke="currentColor" stroke-width="1.85"/><rect x="13" y="4" width="8" height="10" rx="1.5" stroke="currentColor" stroke-width="1.85"/><rect x="13" y="15" width="8" height="5" rx="1.5" stroke="currentColor" stroke-width="1.85"/></svg>';

/** 类 Cursor：用户气泡右下角手绘风「回退到此提问前」 */
const CCT_USER_ROLLBACK_SKETCH_SVG =
  '<svg class="bubble-copy-icon bubble-sketch-rollback" width="20" height="20" viewBox="0 0 24 24" fill="none" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">' +
  '<path d="M8.2 16.4c-.2.35-.35.72-.45 1.1-.85-.55-1.55-1.32-2-2.25-.95-1.9-.75-4.15.55-5.85.85-1.1 2.05-1.85 3.35-2.1" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" fill="none"/>' +
  '<path d="M5.5 7.6 8 5l2.6 2.55M8 5.05v4.2" stroke="currentColor" stroke-width="1.75" stroke-linecap="round" stroke-linejoin="round"/>' +
  '<path d="M17.2 18.3c1.45-.95 2.4-2.6 2.4-4.45 0-2.2-1.35-4.05-3.25-4.85" stroke="currentColor" stroke-width="1.55" stroke-linecap="round" fill="none" opacity="0.85"/>' +
  "</svg>";

function cctGetPreviousAssistantBubble(fromEl) {
  const box = document.getElementById("chat-messages");
  if (!box || !fromEl) return null;
  const list = [...box.querySelectorAll(":scope > .bubble.assistant")];
  const i = list.indexOf(fromEl);
  return i > 0 ? list[i - 1] : null;
}

function cctGetRawAssistantTextFromBubble(b) {
  if (!b) return "";
  if (b._cctAssistantRaw != null) return String(b._cctAssistantRaw);
  const main = b.querySelector(".bubble-main");
  if (main) return String(main.textContent || "");
  const rep = b.querySelector(".bubble-reply");
  return rep ? String(rep.textContent || "") : "";
}

/** 当前条 + 上一条助手（若有可解析代码）合并为 1～2 个落盘项，路径冲突时给上一条加前缀 */
function cctBuildIdeExportListForBubble(bubbleEl) {
  const rawCur = cctGetRawAssistantTextFromBubble(bubbleEl);
  const prevEl = cctGetPreviousAssistantBubble(bubbleEl);
  const rawPrev = cctGetRawAssistantTextFromBubble(prevEl);
  const pCur = rawCur ? cctBuildIdePayloadFromAssistantReply(rawCur) : null;
  const pPrev = rawPrev ? cctBuildIdePayloadFromAssistantReply(rawPrev) : null;
  const list = [];
  if (pCur) {
    if (pPrev) list.push({ path: pPrev.path, content: pPrev.content });
    list.push({ path: pCur.path, content: pCur.content });
  } else if (pPrev) {
    list.push({ path: pPrev.path, content: pPrev.content });
  }
  if (list.length === 2 && list[0].path === list[1].path) {
    const ext = (list[0].path.match(/\.\w+$/) || [""])[0] || ".txt";
    const base = list[0].path.replace(/\.\w+$/, "").replace(/[/\\]/g, "_") || "prev";
    list[0].path = base + "_上一条" + ext;
  }
  return list;
}

/** 从助手正文解析 writes/CCT_APPLY/裸 JSON 中的路径，用于打开左中右工作区 */
function cctExtractAssistantPathsForIde(bubbleEl) {
  const raw = cctGetRawAssistantTextFromBubble(bubbleEl);
  const paths = [];
  const w1 = cctCollectWorkspaceWrites(raw, "");
  for (const w of w1) {
    if (w && typeof w.path === "string" && w.path) paths.push(w.path.replace(/\\/g, "/"));
  }
  const bare = cctParseWorkspaceWritesBare(raw);
  for (const w of bare) {
    if (w && typeof w.path === "string" && w.path) paths.push(w.path.replace(/\\/g, "/"));
  }
  const ap = cctParseCctApply(raw);
  if (ap && typeof ap.path === "string" && ap.path) paths.push(ap.path.replace(/\\/g, "/"));
  return [...new Set(paths.map((p) => p.replace(/^\/+/, "").trim()).filter(Boolean))];
}

async function cctUserBubbleRollback(userBubble, snap) {
  if (
    !userBubble ||
    typeof window.cctChatProjectRollbackWorkspaceSnapshot !== "function" ||
    !snap ||
    !snap.mode ||
    !Array.isArray(snap.items) ||
    !snap.items.length
  )
    return;
  if (
    !window.confirm(
      "回退到此提问之前？将按快照恢复左侧工作区磁盘内容，并移除本条之后的对话气泡（刷新页面后历史记录仍以服务端为准）。"
    )
  )
    return;
  const rb = userBubble.querySelector('[data-cct-user-rollback="1"]');
  if (rb) rb.disabled = true;
  try {
    const rr = await window.cctChatProjectRollbackWorkspaceSnapshot(snap);
    if (!rr || !rr.ok) {
      cctChat.append("system", (rr && rr.error) || "工作区回退失败。");
      if (rb) rb.disabled = false;
      return;
    }
    if (typeof window.cctChatProjectDiscardAiReview === "function") {
      await window.cctChatProjectDiscardAiReview();
    }
    let n = userBubble.nextSibling;
    while (n) {
      const nx = n.nextSibling;
      n.remove();
      n = nx;
    }
    cctChat.append("system", "已回退工作区到该提问前的快照；后续气泡已从界面移除。");
  } catch (e) {
    cctChat.append("system", "回退异常: " + (e && e.message ? e.message : String(e)));
    if (rb) rb.disabled = false;
  }
}

function cctWireIdeButton(ideBtn, bubbleEl) {
  if (!ideBtn) return;
  ideBtn.removeAttribute("hidden");
  ideBtn.hidden = false;
  ideBtn.title = "进入左中右工作区并定位到本条涉及的文件路径";
  ideBtn.setAttribute("aria-label", "打开工作区");
  ideBtn.onclick = async () => {
    const paths = cctExtractAssistantPathsForIde(bubbleEl);
    if (typeof window.cctChatProjectActivateIdeWorkspace === "function") {
      await window.cctChatProjectActivateIdeWorkspace(paths);
      return;
    }
    if (paths.length && typeof window.cctChatProjectFocusWorkspaceForPaths === "function") {
      await window.cctChatProjectFocusWorkspaceForPaths(paths);
    }
  };
}

function cctWireCopyButton(btn, getText) {
  if (!btn) return;
  btn.addEventListener("click", async () => {
    const t = typeof getText === "function" ? getText() : "";
    if (!t) return;
    try {
      await navigator.clipboard.writeText(t);
      btn.classList.add("is-done");
      setTimeout(() => btn.classList.remove("is-done"), 1400);
    } catch (_) {
      window.alert("复制失败，请检查浏览器权限。");
    }
  });
}

const cctChat = {
  box: null,
  input: null,
  scrollEl: null,
  resizeInput() {
    const ta = this.input;
    if (!ta) return;
    ta.style.height = "auto";
    const max = 200;
    const next = Math.min(ta.scrollHeight, max);
    ta.style.height = next + "px";
  },
  scrollToBottom() {
    const sc = this.scrollEl || this.box?.closest(".chat-scroll");
    if (sc) sc.scrollTop = sc.scrollHeight;
  },
  hideWelcome() {
    const w = document.getElementById("chat-empty");
    if (w) w.style.display = "none";
  },
  showWelcome() {
    const w = document.getElementById("chat-empty");
    if (!w) return;
    if (
      window.cctChatProject &&
      typeof window.cctChatProject.isExplorerOpen === "function" &&
      window.cctChatProject.isExplorerOpen()
    ) {
      return;
    }
    w.style.display = "";
  },
  append(role, text, opts) {
    opts = opts || {};
    if (!this.box) return;
    this.hideWelcome();
    const d = document.createElement("div");
    d.className = "bubble " + role;
    const roleLabel = role === "user" ? "你" : role === "assistant" ? "助手" : "系统";
    const showText = role === "assistant" ? cctStripAgentMarkersForDisplay(text) : text;
    const bodyHtml =
      "<div class=\"role\">" +
      roleLabel +
      "</div>" +
      "<div class=\"bubble-main\">" +
      (role === "assistant" ? cctRenderAssistantBodyHtml(text) : this.escapeHtml(showText)) +
      "</div>";
    if (role === "assistant") {
      d._cctAssistantRaw = text;
      const ideBtnHtml =
        "<button type=\"button\" class=\"bubble-action-btn bubble-ide-toggle\" data-cct-ide=\"1\" " +
        "title=\"进入左中右工作区并定位文件\" aria-label=\"打开工作区\">" +
        CCT_IDE_ICON_SVG +
        "</button>";
      d.innerHTML = bodyHtml + "<div class=\"bubble-actions\">" +
        "<button type=\"button\" class=\"bubble-action-btn\" title=\"复制（不含思考与 CCT_APPLY）\" aria-label=\"复制\">" +
        CCT_COPY_ICON_SVG +
        "</button>" +
        ideBtnHtml +
        "</div>";
      const btns = d.querySelectorAll(".bubble-action-btn");
      const btn = btns[0];
      cctWireCopyButton(btn, () => cctStripAgentMarkersForDisplay(text));
      const ide = d.querySelector(".bubble-action-btn[data-cct-ide=\"1\"]");
      cctWireIdeButton(ide, d);
    } else if (role === "user") {
      const ck = opts.checkpointSnap;
      const hasCk = ck && ck.mode && Array.isArray(ck.items) && ck.items.length;
      let html = bodyHtml;
      if (hasCk) {
        html +=
          "<div class=\"bubble-actions bubble-actions--user\">" +
          "<button type=\"button\" class=\"bubble-action-btn bubble-user-rollback\" data-cct-user-rollback=\"1\" " +
          "title=\"回退到此提问之前（恢复工作区快照）\" aria-label=\"回退\">" +
          CCT_USER_ROLLBACK_SKETCH_SVG +
          "</button></div>";
      }
      d.innerHTML = html;
      if (hasCk) {
        const rb = d.querySelector('[data-cct-user-rollback="1"]');
        rb?.addEventListener("click", async (ev) => {
          ev.preventDefault();
          ev.stopPropagation();
          await cctUserBubbleRollback(d, ck);
        });
      }
    } else {
      d.innerHTML = bodyHtml;
    }
    this.box.appendChild(d);
    this.scrollToBottom();
  },
  escapeHtml(s) {
    const m = { "&": "&amp;", "<": "&lt;", ">": "&gt;" };
    return s.replace(/[&<>]/g, (c) => m[c] || c);
  },
  appendLoading() {
    if (!this.box) return;
    this.hideWelcome();
    document.getElementById("cct-assistant-loading-bubble")?.remove();
    const d = document.createElement("div");
    d.id = "cct-assistant-loading-bubble";
    d.className = "bubble assistant bubble--loading";
    d.setAttribute("aria-busy", "true");
    d.setAttribute("aria-label", "正在生成回复");
    d.innerHTML =
      '<div class="role">助手</div>' +
      '<div class="bubble-loading-bar" aria-hidden="true"></div>' +
      '<div class="bubble-loading-dots" aria-hidden="true"><span></span><span></span><span></span></div>';
    this.box.appendChild(d);
    this.scrollToBottom();
  },
  removeLoading() {
    document.getElementById("cct-assistant-loading-bubble")?.remove();
  },
  buildStreamAssistantBubble() {
    const d = document.createElement("div");
    d.className = "bubble assistant bubble--streaming";
    d.innerHTML =
      '<div class="role">助手</div>' +
      '<details class="bubble-think" hidden>' +
      '<summary class="bubble-think-summary">思考过程</summary>' +
      '<div class="bubble-think-body"></div>' +
      "</details>" +
      '<div class="bubble-reply"></div>' +
      '<div class="bubble-actions">' +
      '<button type="button" class="bubble-action-btn" title="复制正文（不含思考与 CCT_APPLY）" aria-label="复制">' +
      CCT_COPY_ICON_SVG +
      '</button>' +
      '<button type="button" class="bubble-action-btn bubble-ide-toggle" data-cct-ide="1" ' +
      'title=\"进入左中右工作区并定位文件\" aria-label=\"打开工作区\">' +
      CCT_IDE_ICON_SVG +
      "</button>" +
      "</div>";
    const thinkBody = d.querySelector(".bubble-think-body");
    const replyEl = d.querySelector(".bubble-reply");
    const thinkDetails = d.querySelector(".bubble-think");
    const copyBtn = d.querySelector(".bubble-action-btn");
    cctWireCopyButton(copyBtn, () => cctStripAgentMarkersForDisplay(replyEl ? replyEl.textContent || "" : ""));
    return { root: d, thinkBody, replyEl, thinkDetails };
  },
  parseSseCarry(carry, onObj) {
    let rest = carry;
    for (;;) {
      const sep = rest.indexOf("\n\n");
      if (sep < 0) break;
      const block = rest.slice(0, sep);
      rest = rest.slice(sep + 2);
      const lines = block.split("\n");
      for (const line of lines) {
        if (!line.startsWith("data:")) continue;
        let payload = line.slice(5).trim();
        if (!payload || payload === "[DONE]") continue;
        try {
          const o = JSON.parse(payload);
          onObj(o);
        } catch (_) {}
      }
    }
    return rest;
  },
  async readSseStream(response, onObj) {
    const reader = response.body && response.body.getReader ? response.body.getReader() : null;
    if (!reader) return;
    const dec = new TextDecoder();
    let carry = "";
    for (;;) {
      const step = await reader.read();
      if (step.done) break;
      carry += dec.decode(step.value, { stream: true });
      carry = this.parseSseCarry(carry, onObj);
    }
    carry += dec.decode();
    carry = this.parseSseCarry(carry, onObj);
  },
  async buildPayload(msg, threadIdFrozen) {
    const tid =
      threadIdFrozen != null && String(threadIdFrozen).trim() !== ""
        ? String(threadIdFrozen).trim()
        : typeof window.cctGetActiveThreadId === "function"
          ? window.cctGetActiveThreadId()
          : "main";
    const payload = { message: msg, thread_id: tid };
    if (typeof window.cctGetSelectedChatModel === "function") {
      const mid = window.cctGetSelectedChatModel();
      if (mid) payload.model = mid;
    }
    if (typeof window.cctChatProjectGetAiContext === "function") {
      const ctx = window.cctChatProjectGetAiContext();
      if (ctx && ctx.content && String(ctx.content).trim()) {
        let c = String(ctx.content);
        if (c.length > 48000) {
          c = c.slice(0, 48000) + "\n…（前端已截断，服务端仍会按 max_context_chars 截断）";
        }
        payload.editor_path = ctx.relPath || ctx.label || "";
        payload.editor_content = c;
      }
    }
    if (typeof window.cctChatProjectBuildWorkspaceAiBundle === "function") {
      try {
        const kBundleMs = 18000;
        const bundle = await Promise.race([
          window.cctChatProjectBuildWorkspaceAiBundle(),
          new Promise((_, rej) => setTimeout(() => rej(new Error("bundle_timeout")), kBundleMs)),
        ]).catch((e) => {
          const es = document.getElementById("chat-explorer-status");
          if (e && e.message === "bundle_timeout" && es) {
            es.textContent =
              "工作区快照超时（已跳过整包）；仍会发送当前打开文件与您的输入。大目录请稍候再试或缩小工作区。";
          }
          return "";
        });
        if (bundle && String(bundle).length > 12) payload.workspace_bundle = bundle;
      } catch (_) {}
    }
    const selA = document.getElementById("chat-agent-select");
    const selS = document.getElementById("chat-skill-select");
    const selC = document.getElementById("chat-command-select");
    if (selA && selA.value) payload.agent = selA.value;
    if (selS && selS.value) payload.skill = selS.value;
    if (selC && selC.value) payload.command = selC.value;
    return payload;
  },
  async send() {
    const msg = this.input.value.trim();
    if (!msg) return;
    const threadAtSend =
      typeof window.cctGetActiveThreadId === "function" ? window.cctGetActiveThreadId() : "main";
    const stillSendingThisThread = () =>
      threadAtSend === (typeof window.cctGetActiveThreadId === "function" ? window.cctGetActiveThreadId() : "main");
    this.input.value = "";
    this.resizeInput();
    if (typeof window.cctChatProjectRememberThreadWorkspaceAnchor === "function") {
      window.cctChatProjectRememberThreadWorkspaceAnchor(threadAtSend);
    }
    let checkpointSnap = null;
    if (typeof window.cctChatProjectCheckpointBeforeUserTurn === "function") {
      try {
        checkpointSnap = await window.cctChatProjectCheckpointBeforeUserTurn();
      } catch (_) {}
    }
    this.append("user", msg, { checkpointSnap });
    this.appendLoading();
    const sendBtn = document.getElementById("chat-send");
    if (sendBtn) sendBtn.disabled = true;
    try {
      let payload;
      try {
        payload = await this.buildPayload(msg, threadAtSend);
      } catch (prepErr) {
        if (stillSendingThisThread()) {
          this.append("assistant", "准备发送失败: " + (prepErr && prepErr.message ? prepErr.message : String(prepErr)));
        }
        return;
      }
      if (typeof window.cctChatProjectApplyAnchorFromPayload === "function") {
        window.cctChatProjectApplyAnchorFromPayload(payload);
      }
      if (typeof window.cctChatProjectGetThreadWorkspaceAnchor === "function") {
        const anch = window.cctChatProjectGetThreadWorkspaceAnchor(payload.thread_id);
        if (anch) payload.workspace_anchor = anch;
      }
      const res = await fetch("/api/chat/stream", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        credentials: "include",
        body: JSON.stringify(payload),
      });
      if (res.status === 401) {
        window.location.href = "/index.html#login";
        return;
      }
      if (res.status === 429) {
        this.removeLoading();
        if (!stillSendingThisThread()) return;
        const data429 = await res.json().catch(() => ({}));
        const code = data429 && data429.code;
        if (code === "token_exhausted" || code === "daily_limit_exceeded") {
          if (typeof window.cctOpenBillingPlans === "function") window.cctOpenBillingPlans();
        }
        this.append(
          "assistant",
          "调用受限：" + ((data429 && data429.error) || "今日模型调用次数已达上限")
        );
        return;
      }
      const ct = (res.headers.get("content-type") || "").toLowerCase();
      if (!res.ok || ct.indexOf("text/event-stream") < 0) {
        if (!stillSendingThisThread()) {
          this.removeLoading();
          return;
        }
        const data = await res.json().catch(() => ({}));
        this.append("assistant", "错误: " + (data.error || res.status));
        return;
      }
      if (!stillSendingThisThread()) {
        this.removeLoading();
        return;
      }
      this.removeLoading();
      const { root, thinkBody, replyEl, thinkDetails } = this.buildStreamAssistantBubble();
      this.box.appendChild(root);
      let fullReply = "";
      let fullThink = "";
      await this.readSseStream(res, (o) => {
        if (!stillSendingThisThread()) return;
        if (!o || !o.e) return;
        if (o.e === "h" && thinkBody && typeof o.d === "string") {
          fullThink += o.d;
          thinkBody.textContent = fullThink;
          if (thinkDetails && fullThink) {
            thinkDetails.hidden = false;
            thinkDetails.open = true;
          }
          this.scrollToBottom();
        }
        if (o.e === "c" && replyEl && typeof o.d === "string") {
          fullReply += o.d;
          replyEl.textContent = cctStripAgentMarkersForDisplay(fullReply);
          this.scrollToBottom();
        }
        if (o.e === "err" && replyEl) {
          replyEl.textContent = "错误: " + (o.m || "未知");
        }
        if (o.e === "done") {
          root.classList.remove("bubble--streaming");
        }
      });
      if (!stillSendingThisThread()) return;
      root.classList.remove("bubble--streaming");
      if (!fullThink && thinkDetails) {
        thinkDetails.removeAttribute("open");
        thinkDetails.hidden = true;
      }
      const replyText = (replyEl && replyEl.textContent) || "";
      if (!String(fullReply).trim() && !String(fullThink).trim() && replyText.indexOf("错误") < 0) {
        if (replyEl) {
          replyEl.textContent =
            "（未收到模型正文与思考：常见原因是工作区快照过大导致上游超时。已限制单次打包文件数；请重试或暂时关闭工作区仅用当前文件对话。）";
        }
      } else if (replyEl && String(fullReply).trim()) {
        replyEl.innerHTML = cctRenderAssistantBodyHtml(fullReply);
      }
      root._cctAssistantRaw = fullReply + (fullThink && String(fullThink).trim() ? "\n\n" + fullThink : "");
      const ideBtn = root.querySelector('.bubble-action-btn[data-cct-ide="1"]');
      cctWireIdeButton(ideBtn, root);

      const mergedRawForDeletes = fullReply + "\n" + (fullThink || "");
      const deletePaths = cctParseWorkspaceDeletes(mergedRawForDeletes);
      if (deletePaths.length && typeof window.cctChatProjectConfirmDeletePaths === "function") {
        try {
          await window.cctChatProjectConfirmDeletePaths(deletePaths);
        } catch (_) {}
      }
      if (!stillSendingThisThread()) return;

      const structuredWrites = cctCollectWorkspaceWrites(fullReply, fullThink);
      let writes = structuredWrites;
      if (!writes.length) {
        writes = cctBuildSyntheticWritesFromAssistantReply(fullReply, fullThink);
      }
      const synthUsed = !structuredWrites.length && writes.length > 0;

      if (writes.length) {
        if (typeof window.cctChatProjectStartAiReview === "function") {
          const pr = await window.cctChatProjectStartAiReview(writes);
          if (!stillSendingThisThread()) return;
          if (pr && pr.skipped) {
            if (
              typeof window.cctChatProjectApplyWorkspaceWrites === "function" ||
              typeof window.cctChatProjectApplyServerWorkspaceWrites === "function"
            ) {
              const apply =
                typeof window.cctChatProjectApplyWorkspaceWrites === "function"
                  ? window.cctChatProjectApplyWorkspaceWrites
                  : window.cctChatProjectApplyServerWorkspaceWrites;
              const ar = await apply(writes);
              if (!stillSendingThisThread()) return;
              if (ar && ar.ok && (ar.count || 0) > 0) {
                let sys =
                  "已将助手返回的 " +
                  (ar.count || writes.length) +
                  " 个文件写入当前工作区（服务端或本机授权目录），左侧树与编辑器已刷新。";
                if (ar.error) sys += " " + ar.error;
                if (synthUsed) sys += "（本轮由代码块自动生成路径）";
                this.append("system", sys);
              } else if (ar && !ar.ok && ar.error) {
                this.append("system", ar.error);
              }
            } else {
              this.append("system", "检测到多文件工作区补丁（CCT_WORKSPACE），但当前页面未加载工作区模块，无法自动写回。");
            }
          } else if (pr && pr.ok) {
            let sys =
              "已在中间栏打开 diff 预览（红删绿增）。确认前不会写盘：请点「保存全部」写入，或关闭预览放弃。";
            if (synthUsed) {
              sys +=
                " 已从代码围栏推断路径（无需模型输出 JSON）；建议在围栏首行用 // path: 相对路径 指定文件名。";
            }
            this.append("system", sys);
          }
        } else if (
          typeof window.cctChatProjectApplyWorkspaceWrites === "function" ||
          typeof window.cctChatProjectApplyServerWorkspaceWrites === "function"
        ) {
          const apply =
            typeof window.cctChatProjectApplyWorkspaceWrites === "function"
              ? window.cctChatProjectApplyWorkspaceWrites
              : window.cctChatProjectApplyServerWorkspaceWrites;
          const ar = await apply(writes);
          if (!stillSendingThisThread()) return;
          if (ar && ar.ok && (ar.count || 0) > 0) {
            let sys =
              "已将助手返回的 " +
              (ar.count || writes.length) +
              " 个文件写入当前工作区（服务端或本机授权目录），左侧树与编辑器已刷新。";
            if (ar.error) sys += " " + ar.error;
            if (synthUsed) sys += "（本轮由代码块自动生成路径）";
            this.append("system", sys);
          } else if (ar && !ar.ok && ar.error) {
            this.append("system", ar.error);
          }
        } else {
          this.append("system", "检测到工作区写入内容，但当前页面未加载工作区模块，无法写回。");
        }
      } else if (payload.workspace_bundle && String(payload.workspace_bundle).length > 12) {
        if (stillSendingThisThread()) {
          this.append(
            "system",
            "本轮未生成写盘预览：模型未输出可解析的结构化补丁，且回复中未检测到足够的代码围栏；请打开左侧工作区并在对话目录下聚焦文件后再试。"
          );
        }
      }
      if (stillSendingThisThread() && typeof window.cctRefreshLlmUsageFromServer === "function") {
        void window.cctRefreshLlmUsageFromServer();
      }
    } catch (netErr) {
      if (stillSendingThisThread()) {
        this.append("assistant", "请求异常: " + (netErr && netErr.message ? netErr.message : String(netErr)));
      }
    } finally {
      this.removeLoading();
      if (sendBtn) sendBtn.disabled = false;
    }
  },
  async clear() {
    await fetch("/api/chat/clear", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      credentials: "include",
      body: JSON.stringify({
        thread_id: typeof window.cctGetActiveThreadId === "function" ? window.cctGetActiveThreadId() : "main",
      }),
    });
    if (this.box) this.box.innerHTML = "";
    this.showWelcome();
  },
};

window.cctOpenChatHistoryPanel = async function () {
  const ov = document.getElementById("chat-history-overlay");
  const list = document.getElementById("chat-history-list");
  const inp = document.getElementById("chat-history-search");
  if (!ov || !list) return;
  ov.hidden = false;
  ov.setAttribute("aria-hidden", "false");
  if (inp) inp.value = "";
  if (typeof window.cctBindChatHistorySearchOnce === "function") {
    window.cctBindChatHistorySearchOnce();
  }
  if (typeof window.cctLoadChatHistoryPanelContents === "function") {
    await window.cctLoadChatHistoryPanelContents("");
  }
};

let cctChatHistorySearchTimer = null;
let cctChatHistorySearchBound = false;

const CCT_CHAT_HISTORY_ICON_EDIT =
  '<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M17 3a2.828 2.828 0 1 1 4 4L7.5 20.5 2 22l1.5-5.5L17 3z"/></svg>';
const CCT_CHAT_HISTORY_ICON_TRASH =
  '<svg xmlns="http://www.w3.org/2000/svg" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><polyline points="3 6 5 6 21 6"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"/><line x1="10" x2="10" y1="11" y2="17"/><line x1="14" x2="14" y1="11" y2="17"/></svg>';

window.cctBindChatHistorySearchOnce = function () {
  if (cctChatHistorySearchBound) return;
  const inp = document.getElementById("chat-history-search");
  if (!inp) return;
  cctChatHistorySearchBound = true;
  inp.addEventListener("input", () => {
    if (cctChatHistorySearchTimer) clearTimeout(cctChatHistorySearchTimer);
    cctChatHistorySearchTimer = setTimeout(() => {
      if (typeof window.cctLoadChatHistoryPanelContents === "function") {
        window.cctLoadChatHistoryPanelContents(inp.value);
      }
    }, 200);
  });
};

async function cctOpenHistoryThread(id, threadWorkspaceAnchor) {
  const ov = document.getElementById("chat-history-overlay");
  if (!id) return;
  if (typeof window.cctSetActiveAppView === "function") window.cctSetActiveAppView("chat");
  window.cctSetActiveThreadId(id);
  const r2 = await fetch("/api/chat/thread?id=" + encodeURIComponent(id), { credentials: "include" });
  const d2 = await r2.json().catch(() => ({}));
  if (ov) {
    ov.hidden = true;
    ov.setAttribute("aria-hidden", "true");
  }
  if (!d2.ok || !cctChat.box) {
    window.alert((d2 && d2.error) || (!cctChat.box ? "聊天区域未就绪，请刷新页面。" : "加载历史会话失败"));
    return;
  }
  cctChat.box.innerHTML = "";
  cctChat.hideWelcome();
  const msgs = d2.messages || [];
  const serverAnchor = d2.workspaceAnchor || threadWorkspaceAnchor || "";
  if (typeof window.cctChatProjectSyncThreadWorkspaceAnchorFromApi === "function") {
    window.cctChatProjectSyncThreadWorkspaceAnchorFromApi(id, d2, threadWorkspaceAnchor);
  } else {
    if (serverAnchor && typeof window.cctChatProjectSetThreadWorkspaceAnchor === "function") {
      window.cctChatProjectSetThreadWorkspaceAnchor(id, serverAnchor);
    } else if (typeof window.cctChatProjectInferAnchorFromMessages === "function") {
      window.cctChatProjectInferAnchorFromMessages(id, msgs);
    }
  }
  for (const m of msgs) {
    if (m.role === "user") cctChat.append("user", m.content || "");
    else if (m.role === "assistant") cctChat.append("assistant", m.content || "");
  }
  if (!msgs.length) cctChat.showWelcome();
  cctChat.scrollToBottom();
  if (typeof window.cctChatProjectEnsureServerWorkspaceForThread === "function") {
    await window.cctChatProjectEnsureServerWorkspaceForThread(id);
  }
}

window.cctLoadChatHistoryPanelContents = async function (query) {
  const list = document.getElementById("chat-history-list");
  if (!list) return;
  list.innerHTML = "";
  const q = typeof query === "string" ? query.trim() : "";
  let url = "/api/chat/threads";
  if (q) url += "?q=" + encodeURIComponent(q);
  try {
    const r = await fetch(url, { credentials: "include" });
    const d = await r.json().catch(() => ({}));
    if (!r.ok) {
      list.innerHTML =
        "<li class=\"chat-history-error\">加载失败（HTTP " +
        r.status +
        "）。请使用与控制台一致的地址访问（例如 http://127.0.0.1:端口/app.html）。</li>";
      return;
    }
    if (!d.ok) {
      list.innerHTML = "<li class=\"chat-history-error\">" + (d.error || "加载失败") + "</li>";
      return;
    }
    const threads = d.threads || [];
    for (const t of threads) {
      const li = document.createElement("li");
      li.className = "chat-history-item";
      const id = t.id || "";
      const title = t.title || id;
      const threadWorkspaceAnchor = t.workspaceAnchor || "";

      const mainBtn = document.createElement("button");
      mainBtn.type = "button";
      mainBtn.className = "chat-history-item-main";
      mainBtn.textContent = title;
      mainBtn.addEventListener("click", () => cctOpenHistoryThread(id, threadWorkspaceAnchor));

      const actions = document.createElement("div");
      actions.className = "chat-history-item-actions";

      if (id && id !== "main") {
        const ren = document.createElement("button");
        ren.type = "button";
        ren.className = "chat-history-btn chat-history-btn-icon";
        ren.setAttribute("aria-label", "重命名");
        ren.title = "重命名";
        ren.innerHTML = CCT_CHAT_HISTORY_ICON_EDIT;
        ren.addEventListener("click", async (e) => {
          e.stopPropagation();
          const nt = window.prompt("会话标题", title);
          if (nt == null) return;
          const trimmed = String(nt).trim();
          if (!trimmed) {
            window.alert("标题不能为空");
            return;
          }
          try {
            const rr = await fetch("/api/chat/thread", {
              method: "PUT",
              credentials: "include",
              headers: { "Content-Type": "application/json" },
              body: JSON.stringify({ thread_id: id, title: trimmed }),
            });
            const dj = await rr.json().catch(() => ({}));
            if (!rr.ok || !dj.ok) {
              window.alert((dj && dj.error) || "重命名失败");
              return;
            }
            if (typeof window.cctReloadChatHistoryList === "function") window.cctReloadChatHistoryList();
          } catch (err) {
            window.alert("网络错误：" + (err && err.message ? String(err.message) : String(err)));
          }
        });

        const del = document.createElement("button");
        del.type = "button";
        del.className = "chat-history-btn chat-history-btn-icon chat-history-btn-danger";
        del.setAttribute("aria-label", "删除会话");
        del.title = "删除会话";
        del.innerHTML = CCT_CHAT_HISTORY_ICON_TRASH;
        del.addEventListener("click", async (e) => {
          e.stopPropagation();
          if (!window.confirm("确定删除此会话？删除后无法恢复。")) return;
          try {
            const rr = await fetch("/api/chat/thread?id=" + encodeURIComponent(id), {
              method: "DELETE",
              credentials: "include",
            });
            const dj = await rr.json().catch(() => ({}));
            if (!rr.ok || !dj.ok) {
              window.alert((dj && dj.error) || "删除失败");
              return;
            }
            const active =
              typeof window.cctGetActiveThreadId === "function" ? window.cctGetActiveThreadId() : "";
            if (active === id && typeof window.cctSetActiveThreadId === "function") {
              window.cctSetActiveThreadId("main");
            }
            if (typeof window.cctReloadChatHistoryList === "function") await window.cctReloadChatHistoryList();
          } catch (err) {
            window.alert("网络错误：" + (err && err.message ? String(err.message) : String(err)));
          }
        });
        actions.appendChild(ren);
        actions.appendChild(del);
      }

      li.appendChild(mainBtn);
      li.appendChild(actions);
      list.appendChild(li);
    }
    if (!threads.length) {
      list.innerHTML = "<li class=\"chat-history-empty\">没有匹配的会话</li>";
    }
  } catch (e) {
    list.innerHTML =
      "<li class=\"chat-history-error\">网络错误：" +
      (e && e.message ? String(e.message) : String(e)) +
      "</li>";
  }
};

window.cctReloadChatHistoryList = function () {
  const inp = document.getElementById("chat-history-search");
  const q = inp ? inp.value : "";
  return window.cctLoadChatHistoryPanelContents(q);
};

/** 从 /api/agents、/api/skills 填充聊天区下拉框（登录后或保存组件后调用） */
async function cctChatRefreshAgentSkillOptions() {
  const selA = document.getElementById("chat-agent-select");
  const selS = document.getElementById("chat-skill-select");
  const selC = document.getElementById("chat-command-select");
  if (!selA || !selS) return;
  const prevA = selA.value;
  const prevS = selS.value;
  const prevC = selC ? selC.value : "";
  async function fill(kind, sel) {
    const data = await fetch("/api/" + kind, { credentials: "include" }).then((r) => r.json()).catch(() => ({}));
    sel.innerHTML = "<option value=\"\">（无）</option>";
    if (!data.ok || !Array.isArray(data.items)) return;
    for (const n of data.items) {
      if (!n) continue;
      const o = document.createElement("option");
      o.value = n;
      o.textContent = n;
      sel.appendChild(o);
    }
  }
  await fill("agents", selA);
  await fill("skills", selS);
  if (selC) await fill("commands", selC);
  if ([...selA.options].some((op) => op.value === prevA)) selA.value = prevA;
  if ([...selS.options].some((op) => op.value === prevS)) selS.value = prevS;
  if (selC && [...selC.options].some((op) => op.value === prevC)) selC.value = prevC;
}
window.cctChatRefreshAgentSkillOptions = cctChatRefreshAgentSkillOptions;
