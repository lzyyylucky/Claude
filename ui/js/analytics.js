(function () {
  let wired = false;

  function $(id) {
    return document.getElementById(id);
  }

  function esc(s) {
    const d = document.createElement("div");
    d.textContent = s == null ? "" : String(s);
    return d.innerHTML;
  }

  function fmtLimit(n) {
    const x = Number(n);
    if (!Number.isFinite(x) || x <= 0) return "∞";
    return String(x);
  }

  function fmtNum(n) {
    const x = Number(n);
    if (!Number.isFinite(x)) return "—";
    return Math.round(x).toLocaleString("zh-CN");
  }

  function fmtBytes(n) {
    const x = Number(n);
    if (!Number.isFinite(x) || x < 0) return "—";
    if (x < 1024) return String(Math.round(x)) + " B";
    if (x < 1048576) return (x / 1024).toFixed(x < 10240 ? 1 : 0) + " KB";
    return (x / 1048576).toFixed(x < 10485760 ? 2 : 1) + " MB";
  }

  /**
   * @param {string} hostId
   * @param {Array<{day:string,n:number}>} rows
   * @param {string} gradId SVG gradient id（须唯一）
   * @param {string} cTop
   * @param {string} cBot
   */
  function renderBarChartSvg(hostId, rows, gradId, cTop, cBot) {
    const host = $(hostId);
    if (!host) return;
    const W = 720;
    const H = 168;
    const padL = 36;
    const padR = 12;
    const padT = 14;
    const padB = 36;
    const innerW = W - padL - padR;
    const innerH = H - padT - padB;
    const nums = Array.isArray(rows) ? rows.map((r) => (r && typeof r.n === "number" ? r.n : 0)) : [];
    const maxN = Math.max(1, ...nums);
    const n = nums.length || 1;
    const step = innerW / Math.max(n, 1);

    let bars = "";
    for (let i = 0; i < nums.length; i++) {
      const v = nums[i];
      const bh = (v / maxN) * innerH;
      const x = padL + i * step + step * 0.18;
      const bw = step * 0.64;
      const y = padT + innerH - bh;
      const day = rows[i] && rows[i].day ? String(rows[i].day).slice(5) : "";
      bars += `<rect x="${x.toFixed(2)}" y="${y.toFixed(2)}" width="${bw.toFixed(2)}" height="${bh.toFixed(
        2
      )}" rx="4" fill="url(#${gradId})" opacity="0.93"/>`;
      if (day && i % 2 === 0)
        bars += `<text x="${(x + bw / 2).toFixed(2)}" y="${H - 10}" text-anchor="middle" fill="currentColor" opacity="0.45" font-size="10">${esc(
          day
        )}</text>`;
    }

    host.innerHTML = `<svg width="${W}" height="${H}" viewBox="0 0 ${W} ${H}" xmlns="http://www.w3.org/2000/svg" aria-hidden="true" style="color:var(--gpt-muted)">
      <defs><linearGradient id="${gradId}" x1="0" y1="0" x2="0" y2="1"><stop offset="0%" stop-color="${cTop}"/><stop offset="100%" stop-color="${cBot}"/></linearGradient></defs>
      <text x="${padL}" y="${padT + 4}" font-size="11" fill="currentColor" opacity="0.6">${esc(String(maxN))}</text>
      ${bars}
    </svg>`;
  }

  function renderBarList(containerId, items, emptyHint, fillClass) {
    const root = $(containerId);
    if (!root) return;
    root.innerHTML = "";
    if (!items || !items.length) {
      const p = document.createElement("p");
      p.className = "analytics-empty";
      p.textContent = emptyHint || "暂无数据";
      root.appendChild(p);
      return;
    }
    const maxN = Math.max(...items.map((x) => x.n || 0), 1);
    const fill = fillClass || "analytics-bar-fill";
    const frag = document.createDocumentFragment();
    for (const it of items) {
      const row = document.createElement("div");
      row.className = "analytics-bar-row";
      const pct = ((it.n || 0) / maxN) * 100;
      row.innerHTML = `<div class="analytics-bar-label" title="${esc(it.label)}">${esc(it.label)}</div>
        <div class="analytics-bar-count">${esc(String(it.n || 0))}</div>
        <div class="analytics-bar-track"><div class="${fill}" style="width:${pct.toFixed(1)}%"></div></div>`;
      frag.appendChild(row);
    }
    root.appendChild(frag);
  }

  function renderUsageOverview(j) {
    const sec = $("analytics-usage-overview");
    if (!sec) return;
    sec.hidden = false;
    const tier = String(j.subscriptionTier || "free").toLowerCase();
    const tierLbl = $("analytics-usage-tier-lbl");
    if (tierLbl) tierLbl.textContent = tier;

    const today = Number(j.llmCallsToday) || 0;
    const lim = Number(j.llmDailyLimit);
    const barC = $("analytics-usage-bar-calls");
    const metaC = $("analytics-usage-meta-calls");
    if (barC && metaC) {
      if (!Number.isFinite(lim) || lim <= 0) {
        barC.style.width = "0%";
        barC.classList.remove("analytics-usage-bar-fill--warn", "analytics-usage-bar-fill--full");
        metaC.textContent = "已用 " + today + " 次 · 今日无单日上限（免费档且未配置限额，或当前策略不限）";
      } else {
        const pct = Math.min(100, Math.round((today / lim) * 100));
        barC.style.width = pct + "%";
        barC.classList.toggle("analytics-usage-bar-fill--warn", pct >= 85 && pct < 100);
        barC.classList.toggle("analytics-usage-bar-fill--full", pct >= 100);
        const rem = Math.max(0, lim - today);
        metaC.textContent = "已用 " + today + " / 上限 " + lim + "（剩余约 " + rem + " 次）";
      }
    }

    const tq = Number(j.tokenQuota) || 0;
    const tc = Number(j.tokensConsumed) || 0;
    const barT = $("analytics-usage-bar-tokens");
    const metaT = $("analytics-usage-meta-tokens");
    if (barT && metaT) {
      if (tq <= 0) {
        barT.style.width = "0%";
        barT.classList.remove("analytics-usage-bar-fill--warn", "analytics-usage-bar-fill--full");
        metaT.textContent = "本月 Token 配额未就绪";
      } else {
        const pct = Math.min(100, Math.round((tc / tq) * 100));
        barT.style.width = pct + "%";
        barT.classList.toggle("analytics-usage-bar-fill--warn", pct >= 85 && pct < 100);
        barT.classList.toggle("analytics-usage-bar-fill--full", pct >= 100);
        const rem = Math.max(0, tq - tc);
        metaT.textContent =
          "已消耗 " + fmtNum(tc) + " / 配额 " + fmtNum(tq) + "（剩余 " + fmtNum(rem) + "）";
      }
    }
  }

  function renderKindSplit(ks) {
    const chat = ks && typeof ks.chat === "number" ? ks.chat : 0;
    const scan = ks && typeof ks.scan === "number" ? ks.scan : 0;
    const total = chat + scan || 1;
    const pctChat = ((chat / total) * 100).toFixed(1);
    const pctScan = ((scan / total) * 100).toFixed(1);
    const row = $("analytics-kind-row");
    const leg = $("analytics-kind-legend");
    if (row) {
      row.innerHTML = `<div class="analytics-kind-seg analytics-kind-seg--chat" style="width:${pctChat}%">对话</div>
        <div class="analytics-kind-seg analytics-kind-seg--scan" style="width:${pctScan}%">检测</div>`;
    }
    if (leg) {
      leg.innerHTML = `<span>对话 <strong>${esc(String(chat))}</strong> 次（${esc(pctChat)}%）</span>
        <span>代码检测 <strong>${esc(String(scan))}</strong> 次（${esc(pctScan)}%）</span>`;
    }
  }

  async function load() {
    const msg = $("analytics-msg");
    const btn = $("analytics-refresh");
    const st = $("analytics-refresh-status");
    if (msg) msg.textContent = "";
    if (btn) {
      btn.disabled = true;
      btn.classList.add("analytics-refresh--loading");
      btn.setAttribute("aria-busy", "true");
    }
    if (st) st.textContent = "正在刷新…";
    try {
      const r = await fetch("/api/analytics/summary", { credentials: "include" });
      const j = await r.json().catch(() => ({}));
      if (!r.ok || !j.ok) {
        if (msg) msg.textContent = (j && j.error) || `加载失败（${r.status}）`;
        if (st) st.textContent = "";
        return;
      }

      renderUsageOverview(j);

      const lim = fmtLimit(j.llmDailyLimit);
      const setTxt = (id, v) => {
        const el = $(id);
        if (el) el.textContent = v;
      };
      setTxt("analytics-m-today", String(j.llmCallsToday ?? 0));
      setTxt("analytics-m-limit", lim);
      setTxt("analytics-m-total", String(j.llmTotalCalls ?? 0));
      setTxt("analytics-m-threads-store", String(j.threadsStored ?? 0));
      setTxt("analytics-m-threads-log", String(j.threadsActiveInLog ?? 0));
      setTxt("analytics-m-lines", String(j.eventsLinesRead ?? 0));

      renderBarChartSvg(
        "analytics-chart-daily",
        Array.isArray(j.dailyCalls) ? j.dailyCalls : [],
        "analyticsGradCalls",
        "#818cf8",
        "#6366f1"
      );
      renderBarChartSvg(
        "analytics-chart-tokens",
        Array.isArray(j.dailyTokens) ? j.dailyTokens : [],
        "analyticsGradTokens",
        "#5eead4",
        "#0d9488"
      );
      renderKindSplit(j.kindSplit || {});

      const rel = j.reliability || {};
      setTxt("analytics-r-chat-ok", (rel.chatOkPct != null ? String(rel.chatOkPct) : "—") + "%");
      setTxt("analytics-r-scan-ok", (rel.scanOkPct != null ? String(rel.scanOkPct) : "—") + "%");

      const lat = j.latency || {};
      setTxt("analytics-l-chat", lat.chatAvgMs != null ? String(lat.chatAvgMs) : "—");
      setTxt("analytics-l-scan", lat.scanAvgMs != null ? String(lat.scanAvgMs) : "—");
      const foot = $("analytics-latency-footnote");
      if (foot) {
        foot.textContent = `耗时样本：对话 ${lat.chatSamples ?? 0} 次 · 检测 ${lat.scanSamples ?? 0} 次（仅统计成功且日志中含 ms 的请求）。`;
      }

      const tt = j.tokenTotals || {};
      setTxt("analytics-t-prompt", fmtNum(tt.prompt));
      setTxt("analytics-t-comp", fmtNum(tt.completion));
      setTxt("analytics-t-api", fmtNum(tt.totalFromApi));
      setTxt("analytics-t-inf", fmtNum(tt.inferredTotal));

      const ws = j.workspaceWrites || {};
      setTxt("analytics-w-writes", String(ws.writes ?? 0));
      setTxt("analytics-w-mkdir", String(ws.mkdirs ?? 0));
      setTxt("analytics-w-bytes", fmtBytes(ws.bytes));

      renderBarList(
        "analytics-ws-paths",
        Array.isArray(j.workspacePathHits) ? j.workspacePathHits.map((x) => ({ label: x.label, n: x.n })) : [],
        "暂无写盘记录（在资源管理器保存文件、采纳 AI 修改或使用 IDE 写盘后出现）",
        "analytics-bar-fill analytics-bar-fill--mint"
      );

      renderBarList(
        "analytics-tools",
        Array.isArray(j.toolPickHits) ? j.toolPickHits.map((x) => ({ label: x.label, n: x.n })) : [],
        "尚无 Agent / Skill / Command 选用记录（先在对话里选用组件后再查看）"
      );
      renderBarList(
        "analytics-models",
        Array.isArray(j.modelHits) ? j.modelHits.map((x) => ({ label: x.label, n: x.n })) : [],
        "尚无模型选用记录（使用默认模型且 API 未返回 md 时可能为空）"
      );

      const t = new Date();
      const ts = t.toLocaleString("zh-CN", { hour: "2-digit", minute: "2-digit", second: "2-digit" });
      if (st) st.textContent = "已于 " + ts + " 更新";
    } catch (e) {
      if (msg) msg.textContent = "网络或解析错误";
      if (st) st.textContent = "";
    } finally {
      if (btn) {
        btn.disabled = false;
        btn.classList.remove("analytics-refresh--loading");
        btn.setAttribute("aria-busy", "false");
      }
    }
  }

  window.cctAnalyticsEnsureInit = function () {
    if (wired) return;
    wired = true;
    $("analytics-refresh")?.addEventListener("click", () => load());
  };

  window.cctAnalyticsReload = load;
})();
