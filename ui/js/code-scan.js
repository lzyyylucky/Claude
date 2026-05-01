/**
 * 「代码检测」面板：多维度 AI 审计 + 可选写盘补丁（与服务端 workspace 对齐）。
 */
(function () {
  let inited = false;
  /** @type {Array<{path:string,content:string}>|null} */
  let lastStructuredPatches = null;
  /** @type {string[]} */
  let scanPaths = [];
  const selectedDims = new Set();

  let progressTimer = null;

  const DIMS = [
    { id: "performance", title: "性能与算法", desc: "时间/空间复杂度、热点路径" },
    { id: "maintainability", title: "可读与结构", desc: "命名、重复代码、分层" },
    { id: "robustness", title: "健壮性", desc: "边界、错误处理、输入校验" },
    { id: "completeness", title: "完整度", desc: "TODO、占位、契约对齐" },
    { id: "security", title: "安全性", desc: "注入、敏感信息、危险 API" },
  ];

  let pickerScope = "";

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

  function joinRel(a, b) {
    const x = normWorkspacePath(a);
    const y = normWorkspacePath(b);
    if (!y) return x;
    return x ? x + "/" + y : y;
  }

  function parentScopePath(scope) {
    const parts = normWorkspacePath(scope).split("/").filter(Boolean);
    parts.pop();
    return parts.join("/");
  }

  function esc(s) {
    return String(s || "")
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;");
  }

  function fileIconSvg() {
    return '<svg class="code-scan-chip-ic" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" aria-hidden="true"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/></svg>';
  }

  function folderIconSvg() {
    return '<svg class="code-scan-chip-ic" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.75" aria-hidden="true"><path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/><path d="M2 10h20"/></svg>';
  }

  function guessIsDir(path) {
    const n = normWorkspacePath(path);
    return n.length > 0 && !/\.[a-zA-Z0-9]{1,12}$/.test(n.split("/").pop() || "");
  }

  function renderPathChips() {
    const host = document.getElementById("codescan-path-chips");
    if (!host) return;
    host.innerHTML = "";
    scanPaths.forEach((p, idx) => {
      const chip = document.createElement("div");
      chip.className = "code-scan-chip";
      const isDir = guessIsDir(p);
      chip.innerHTML =
        (isDir ? folderIconSvg() : fileIconSvg()) +
        '<span class="code-scan-chip-text">' +
        esc(p) +
        '</span><button type="button" class="code-scan-chip-x" data-i="' +
        idx +
        '" aria-label="移除">×</button>';
      host.appendChild(chip);
    });
    host.querySelectorAll(".code-scan-chip-x").forEach((btn) => {
      btn.addEventListener("click", () => {
        const i = Number(btn.getAttribute("data-i"));
        if (!Number.isFinite(i)) return;
        scanPaths.splice(i, 1);
        renderPathChips();
      });
    });
  }

  function addPathQuiet(raw) {
    const n = normWorkspacePath(raw);
    if (!n) return false;
    if (!scanPaths.includes(n)) scanPaths.push(n);
    renderPathChips();
    return true;
  }

  function renderDims() {
    const host = document.getElementById("codescan-dims");
    if (!host) return;
    host.innerHTML = "";
    DIMS.forEach((d) => {
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "code-scan-dim-card" + (selectedDims.has(d.id) ? " is-selected" : "");
      btn.dataset.dim = d.id;
      btn.innerHTML =
        '<span class="code-scan-dim-title">' +
        esc(d.title) +
        '</span><span class="code-scan-dim-desc">' +
        esc(d.desc) +
        "</span>";
      btn.addEventListener("click", (ev) => {
        ev.preventDefault();
        if (selectedDims.has(d.id)) selectedDims.delete(d.id);
        else selectedDims.add(d.id);
        renderDims();
      });
      host.appendChild(btn);
    });
  }

  function stopProgressAnim() {
    if (progressTimer) {
      clearInterval(progressTimer);
      progressTimer = null;
    }
    const el = document.getElementById("codescan-progress");
    if (el) {
      el.hidden = true;
      el.textContent = "";
    }
  }

  function startProgressAnim() {
    stopProgressAnim();
    const el = document.getElementById("codescan-progress");
    if (!el) return;
    const steps = [
      "正在连接服务端…",
      "正在打包源码片段…",
      "正在调用 AI 模型…",
      "等待模型输出…",
    ];
    let i = 0;
    el.hidden = false;
    el.textContent = steps[0];
    progressTimer = window.setInterval(() => {
      i = (i + 1) % steps.length;
      el.textContent = steps[i];
    }, 900);
  }

  function codescanSanitizeReplyDisplay(reply, hideBrokenFence) {
    const r = String(reply || "");
    if (!hideBrokenFence) return r;
    const idx = r.search(/```\s*json\b/i);
    if (idx < 0) return r;
    return r.slice(0, idx).trimEnd() + "\n\n---\n*（不完整 JSON 围栏已从展示中省略。）*\n";
  }

  function extractSolutionSummaryMd(text) {
    const t = String(text || "");
    const fi = t.search(/```\s*json\b/i);
    const md = fi >= 0 ? t.slice(0, fi) : t;
    const m = md.match(/##\s*解决方案概要\s*\n+([\s\S]*?)(?=\n##[^#]|\n```|$)/i);
    if (m) return m[1].trim().replace(/\n{3,}/g, "\n\n");
    return "";
  }

  /** JSON 缺失或解析失败时，从 Markdown 正文还原卡片（按所选维度关键词匹配标题） */
  function fallbackCardsFromMarkdown(reply, allowedIdsSet) {
    const raw = String(reply || "");
    const fi = raw.search(/```\s*json\b/i);
    const mdPart = fi >= 0 ? raw.slice(0, fi) : raw;
    const ids = Array.from(allowedIdsSet).map((x) => String(x).toLowerCase());
    const kw = {
      performance: [/performance/i, /性能/, /算法/, /热点/, /复杂度/],
      maintainability: [/maintainability/i, /可读/, /结构/, /维护/, /重复/, /分层/],
      robustness: [/robustness/i, /健壮/, /边界/, /错误处理/, /校验/],
      completeness: [/completeness/i, /完整/, /TODO/i, /占位/],
      security: [/security/i, /安全/, /注入/, /敏感/],
    };
    const chunks = mdPart.split(/\n(?=#{2,4}\s+)/);
    const merged = new Map();
    for (const chunk of chunks) {
      const hm = chunk.match(/^#{2,4}\s+(.+)/);
      if (!hm) continue;
      const heading = hm[1].trim().replace(/\*\*/g, "").trim();
      let matched = null;
      for (const id of ids) {
        if (heading.toLowerCase().includes(id)) {
          matched = id;
          break;
        }
      }
      if (!matched) {
        for (const id of ids) {
          const ks = kw[id];
          if (!ks) continue;
          if (ks.some((re) => re.test(heading))) {
            matched = id;
            break;
          }
        }
      }
      if (!matched) continue;
      const items = [];
      for (const line of chunk.split("\n")) {
        const lm = line.match(/^\s*[-*]\s+(.+)/);
        if (lm) items.push(lm[1].replace(/\*\*/g, "").trim());
      }
      if (!items.length) continue;
      const dimMeta = DIMS.find((d) => d.id === matched);
      const titleUse = heading.length > 56 && dimMeta ? dimMeta.title : heading;
      const prev = merged.get(matched);
      if (!prev) merged.set(matched, { dimension: matched, title: titleUse, items: items.slice(), severity: "" });
      else prev.items.push(...items);
    }
    return Array.from(merged.values()).map((b) => ({
      dimension: b.dimension,
      title: b.title,
      items: b.items.slice(0, 14),
      severity: b.severity || "",
    }));
  }

  function renderDimensionStructured(parsed, fallbackBlocks, opts) {
    opts = opts || {};
    const host = document.getElementById("codescan-dimension-out");
    const summaryEl = document.getElementById("codescan-summary");
    const legend = document.getElementById("codescan-severity-legend");
    if (legend) legend.hidden = true;
    if (!host) return;
    host.innerHTML = "";
    let dimArr = parsed && Array.isArray(parsed.dimension_results) ? parsed.dimension_results.slice() : [];
    const allowedDims = new Set(Array.from(selectedDims).map((id) => String(id).toLowerCase()));
    dimArr = dimArr.filter((block) => allowedDims.has(String(block.dimension || "").toLowerCase()));
    if (!dimArr.length && fallbackBlocks && fallbackBlocks.length) {
      dimArr = fallbackBlocks.filter((block) => allowedDims.has(String(block.dimension || "").toLowerCase()));
    }
    if (!dimArr.length) {
      host.hidden = true;
      if (parsed && parsed.summary && summaryEl) {
        summaryEl.hidden = false;
        summaryEl.textContent = String(parsed.summary);
      } else if (opts.summaryMd && summaryEl) {
        summaryEl.hidden = false;
        summaryEl.textContent = opts.summaryMd;
      }
      return;
    }
    host.hidden = false;
    let anySeverity = false;
    dimArr.forEach((block) => {
      const title = esc(block.title || block.dimension || "");
      if (block.severity) anySeverity = true;
      const sev = block.severity ? '<span class="code-scan-sev">' + esc(block.severity) + "</span>" : "";
      const items = Array.isArray(block.items) ? block.items : [];
      const ul = document.createElement("div");
      ul.className = "code-scan-dim-block";
      ul.innerHTML =
        '<div class="code-scan-dim-block-head"><span class="code-scan-dim-block-title">' +
        title +
        "</span>" +
        sev +
        "</div><ul class=\"code-scan-dim-items\"></ul>";
      const list = ul.querySelector(".code-scan-dim-items");
      items.forEach((it) => {
        const li = document.createElement("li");
        li.textContent = String(it);
        list.appendChild(li);
      });
      host.appendChild(ul);
    });
    if (legend) legend.hidden = !anySeverity;
    if (parsed && parsed.solution_summary && summaryEl) {
      summaryEl.hidden = false;
      summaryEl.textContent = String(parsed.solution_summary);
    } else if (opts.summaryMd && summaryEl) {
      summaryEl.hidden = false;
      summaryEl.textContent = opts.summaryMd;
    } else if (parsed && parsed.summary && summaryEl) {
      summaryEl.hidden = false;
      summaryEl.textContent = String(parsed.summary);
    }
  }

  async function runScan() {
    const replyEl = document.getElementById("codescan-reply");
    const summaryEl = document.getElementById("codescan-summary");
    const dimOut = document.getElementById("codescan-dimension-out");
    const busy = document.getElementById("codescan-busy");
    const applyWrap = document.getElementById("codescan-apply-wrap");
    const patchMeta = document.getElementById("codescan-patch-meta");
    lastStructuredPatches = null;
    if (applyWrap) applyWrap.hidden = true;
    if (summaryEl) {
      summaryEl.hidden = true;
      summaryEl.textContent = "";
    }
    if (dimOut) {
      dimOut.hidden = true;
      dimOut.innerHTML = "";
    }
    const leg = document.getElementById("codescan-severity-legend");
    if (leg) leg.hidden = true;
    const truncBn = document.getElementById("codescan-truncation-banner");
    if (truncBn) {
      truncBn.hidden = true;
      truncBn.textContent = "";
    }
    if (!scanPaths.length) {
      window.alert("请先通过「浏览工作区」添加文件或文件夹");
      return;
    }
    if (!selectedDims.size) {
      window.alert("请至少选择一个检测维度");
      return;
    }
    const model = typeof window.cctGetSelectedChatModel === "function" ? window.cctGetSelectedChatModel() : "";
    if (busy) {
      busy.hidden = false;
      busy.textContent = "检测进行中…";
    }
    startProgressAnim();
    if (replyEl) replyEl.innerHTML = "";
    try {
      const res = await fetch("/api/code-scan/run", {
        method: "POST",
        credentials: "include",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          paths: scanPaths.slice(),
          dimensions: Array.from(selectedDims),
          model: model || undefined,
        }),
      });
      const data = await res.json().catch(() => ({}));
      stopProgressAnim();
      if (busy) busy.hidden = true;
      if (res.status === 429 && data && (data.code === "token_exhausted" || data.code === "daily_limit_exceeded")) {
        if (typeof window.cctOpenBillingPlans === "function") window.cctOpenBillingPlans();
      }
      if (!res.ok || !data.ok) {
        window.alert((data && data.error) || "检测失败（HTTP " + res.status + "）");
        return;
      }
      const rawReply = data.reply || "";
      let parsed = null;
      if (data.structured_json) {
        try {
          parsed = JSON.parse(data.structured_json);
        } catch (_) {
          parsed = null;
        }
      }
      const truncatedHint = data.scan_truncated === true;
      const parseFailed = !!(data.structured_json && !parsed);
      const hideFence = truncatedHint || parseFailed;
      const displayReply = codescanSanitizeReplyDisplay(rawReply, hideFence);
      if (replyEl) {
        if (typeof window.cctRenderAssistantBodyHtml === "function") {
          replyEl.innerHTML = window.cctRenderAssistantBodyHtml(displayReply);
        } else {
          replyEl.textContent = displayReply;
        }
      }
      const summaryMd = extractSolutionSummaryMd(rawReply);
      const fb = fallbackCardsFromMarkdown(rawReply, selectedDims);
      const truncBanner = document.getElementById("codescan-truncation-banner");
      if (truncBanner) {
        const looksFenceOpen =
          rawReply.indexOf("```json") >= 0 || rawReply.indexOf("```JSON") >= 0;
        const showWarn = truncatedHint || parseFailed || (!parsed && looksFenceOpen);
        if (showWarn) {
          truncBanner.hidden = false;
          truncBanner.textContent =
            "检测到模型输出可能被截断，或 JSON 未能完整解析（常见于单次输出过长）。已根据 Markdown 正文尽量还原下方卡片式小结；若无「接受方案并写盘」按钮，说明本轮未返回完整补丁列表，请缩小检测文件范围或在配置中提高 max_tokens（代码检测单次请求已自动不低于 16384，可与配置取更大值）。";
        } else {
          truncBanner.hidden = true;
          truncBanner.textContent = "";
        }
      }
      renderDimensionStructured(parsed, fb, { summaryMd });
      let patches = parsed && Array.isArray(parsed.patches) ? parsed.patches : [];
      patches = patches.filter((p) => p && typeof p.path === "string" && typeof p.content === "string" && normWorkspacePath(p.path));
      lastStructuredPatches = patches.length ? patches : null;
      if (applyWrap && patchMeta) {
        if (lastStructuredPatches && lastStructuredPatches.length) {
          applyWrap.hidden = false;
          patchMeta.textContent =
            "模型返回 " + lastStructuredPatches.length + " 个文件的完整替换内容；点击「接受方案并写盘」写入服务端工作区。";
        } else {
          applyWrap.hidden = true;
          patchMeta.textContent = "";
        }
      }
      if (typeof window.cctRefreshLlmUsageFromServer === "function") window.cctRefreshLlmUsageFromServer();
    } catch (e) {
      stopProgressAnim();
      if (busy) busy.hidden = true;
      window.alert(e && e.message ? e.message : "网络错误");
    }
  }

  async function applyPatches() {
    if (!lastStructuredPatches || !lastStructuredPatches.length) return;
    if (!window.confirm("接受 AI 给出的修改并写入服务端工作区？将覆盖对应路径的文件内容。")) return;
    let okCount = 0;
    const snapshots = [];
    for (const patch of lastStructuredPatches) {
      const rawPath = patch && typeof patch.path === "string" ? patch.path : "";
      const content = patch && typeof patch.content === "string" ? patch.content : "";
      const path = normWorkspacePath(rawPath);
      if (!path) continue;
      let before = "";
      let hadFile = false;
      try {
        const gr = await fetch("/api/workspace/file?path=" + encodeURIComponent(path), { credentials: "include" });
        const gd = await gr.json().catch(() => ({}));
        if (gr.ok && gd.ok && typeof gd.content === "string") {
          before = gd.content;
          hadFile = true;
        }
      } catch (_) {}
      const res = await fetch("/api/workspace/file", {
        method: "POST",
        credentials: "include",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ path, content }),
      });
      const data = await res.json().catch(() => ({}));
      if (res.ok && data.ok) {
        okCount++;
        snapshots.push({ path, before, after: content, wasNew: !hadFile });
      } else window.alert((data && data.error) || "写入失败 " + path);
    }
    window.alert("写盘完成：" + okCount + " / " + lastStructuredPatches.length);
    if (typeof window.cctChatProjectRebuildTree === "function") window.cctChatProjectRebuildTree();
    if (snapshots.length && typeof window.cctChatProjectShowPostApplyDiff === "function") {
      if (typeof window.cctSetActiveAppView === "function") window.cctSetActiveAppView("chat");
      window.cctChatProjectShowPostApplyDiff(snapshots);
    }
  }

  function closePicker() {
    const ov = document.getElementById("codescan-explorer-overlay");
    if (ov) {
      ov.hidden = true;
      ov.setAttribute("aria-hidden", "true");
    }
  }

  async function refreshPickerList() {
    const listEl = document.getElementById("codescan-explorer-list");
    const crumbs = document.getElementById("codescan-explorer-crumbs");
    if (!listEl) return;
    const scope = pickerScope;
    if (crumbs) crumbs.textContent = scope ? "/" + scope.replace(/\//g, " / ") : "workspace 根目录";
    listEl.innerHTML = "";
    const q = scope ? "?path=" + encodeURIComponent(scope) : "";
    let data = {};
    try {
      const r = await fetch("/api/workspace/list" + q, { credentials: "include" });
      data = await r.json().catch(() => ({}));
    } catch (_) {
      data = {};
    }
    if (!data.ok) {
      listEl.innerHTML = '<div class="code-scan-explorer-err">' + esc(data.error || "列出失败") + "</div>";
      return;
    }
    const entries = (data.entries || []).slice().sort((a, b) => {
      if (a.type !== b.type) return a.type === "dir" ? -1 : 1;
      return String(a.name).localeCompare(String(b.name));
    });
    entries.forEach((ent) => {
      const row = document.createElement("div");
      row.className = "code-scan-explorer-row";
      const full = joinRel(scope, ent.name);
      if (ent.type === "dir") {
        row.innerHTML =
          folderIconSvg() +
          '<span class="code-scan-explorer-name">' +
          esc(ent.name) +
          '</span><span class="code-scan-explorer-actions"><button type="button" class="code-scan-explorer-mini" data-nav="' +
          esc(full) +
          '">进入</button><button type="button" class="code-scan-explorer-mini" data-add-dir="' +
          esc(full) +
          '">添加</button></span>';
      } else {
        row.innerHTML =
          fileIconSvg() +
          '<span class="code-scan-explorer-name">' +
          esc(ent.name) +
          '</span><span class="code-scan-explorer-actions"><button type="button" class="code-scan-explorer-mini" data-add-file="' +
          esc(full) +
          '">添加</button></span>';
      }
      listEl.appendChild(row);
    });
    listEl.querySelectorAll("[data-nav]").forEach((btn) => {
      btn.addEventListener("click", () => {
        pickerScope = btn.getAttribute("data-nav") || "";
        void refreshPickerList();
      });
    });
    listEl.querySelectorAll("[data-add-dir]").forEach((btn) => {
      btn.addEventListener("click", () => {
        addPathQuiet(btn.getAttribute("data-add-dir") || "");
      });
    });
    listEl.querySelectorAll("[data-add-file]").forEach((btn) => {
      btn.addEventListener("click", () => {
        addPathQuiet(btn.getAttribute("data-add-file") || "");
      });
    });
  }

  function openPicker() {
    const ov = document.getElementById("codescan-explorer-overlay");
    if (ov) {
      ov.hidden = false;
      ov.setAttribute("aria-hidden", "false");
    }
    pickerScope = "";
    void refreshPickerList();
  }

  window.cctCodeScanEnsureInit = function () {
    if (inited) return;
    inited = true;
    renderDims();
    renderPathChips();

    document.getElementById("codescan-open-picker")?.addEventListener("click", () => openPicker());
    document.getElementById("codescan-explorer-backdrop")?.addEventListener("click", closePicker);
    document.getElementById("codescan-explorer-close")?.addEventListener("click", closePicker);
    document.getElementById("codescan-explorer-done")?.addEventListener("click", () => closePicker());
    document.getElementById("codescan-explorer-up")?.addEventListener("click", () => {
      pickerScope = parentScopePath(pickerScope);
      void refreshPickerList();
    });

    document.getElementById("codescan-run")?.addEventListener("click", () => void runScan());
    document.getElementById("codescan-apply")?.addEventListener("click", () => void applyPatches());

    document.addEventListener("keydown", (e) => {
      if (e.key !== "Escape") return;
      const ov = document.getElementById("codescan-explorer-overlay");
      if (ov && !ov.hidden) closePicker();
    });
  };
})();
