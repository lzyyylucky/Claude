/**
 * 仓库同步：调用 /api/workspace/git/*，Monaco Diff 见 window.cctRepoSyncMonaco。
 */
(function () {
  const STORAGE_REPO_REL = "cct_repo_sync_repo_rel";
  let inited = false;
  let statusSnapshot = null;
  let diffUi = null;
  let conflictFiles = [];
  let conflictIndex = 0;
  let lastOurs = "";
  let lastTheirs = "";
  let pickerCurRel = "";

  function $(id) {
    return document.getElementById(id);
  }

  function normRepoRel(v) {
    return String(v || "")
      .trim()
      .replace(/\\/g, "/")
      .replace(/^\/+/, "")
      .replace(/\/+$/, "");
  }

  function loadRepoRelFromStorage() {
    try {
      const v = sessionStorage.getItem(STORAGE_REPO_REL);
      const inp = $("repo-sync-repo-rel");
      if (inp && v != null) inp.value = v;
    } catch (_) {}
  }

  function saveRepoRelToStorage() {
    try {
      const inp = $("repo-sync-repo-rel");
      sessionStorage.setItem(STORAGE_REPO_REL, inp ? inp.value.trim() : "");
    } catch (_) {}
  }

  function gitBasePayload(extra) {
    const o = Object.assign({}, extra || {});
    const rel = normRepoRel($("repo-sync-repo-rel") && $("repo-sync-repo-rel").value);
    if (rel) o.repo_rel = rel;
    return o;
  }

  function setBtnBusy(btn, busy) {
    if (!btn) return;
    btn.disabled = !!busy;
    btn.setAttribute("aria-busy", busy ? "true" : "false");
  }

  function setMsg(text, isErr) {
    const el = $("repo-sync-msg");
    if (!el) return;
    el.textContent = text || "";
    el.classList.toggle("repo-sync-msg--err", !!isErr);
  }

  async function gitApi(tail, jsonBody) {
    const body = gitBasePayload(jsonBody);
    const res = await fetch("/api/workspace/git/" + tail, {
      method: "POST",
      credentials: "include",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    });
    const text = await res.text();
    let data = {};
    try {
      data = text ? JSON.parse(text) : {};
    } catch {
      data = { ok: false, error: text.slice(0, 400) };
    }
    return { res, data };
  }

  function rb() {
    const r = ($("repo-sync-remote") && $("repo-sync-remote").value.trim()) || "origin";
    const b = $("repo-sync-branch") && $("repo-sync-branch").value.trim();
    const o = { remote: r };
    if (b) o.branch = b;
    return o;
  }

  function disposeDiff() {
    if (diffUi) {
      try {
        diffUi.dispose();
      } catch (_) {}
      diffUi = null;
    }
    const host = $("repo-sync-diff-host");
    if (host) host.innerHTML = "";
  }

  function firstGitColChar(s) {
    const t = String(s == null ? "" : s).trim();
    return t.length ? t[0] : "";
  }

  function gitColMeaning(c) {
    const map = {
      M: "已修改",
      A: "新增",
      D: "删除",
      R: "重命名",
      C: "复制",
      U: "冲突",
      "?": "未跟踪",
      "!": "忽略",
    };
    return map[c] || "";
  }

  /** 将 simple-git 两列状态转成可读中文（避免直接显示 ? · ?） */
  function readableGitLine(f) {
    const ix = firstGitColChar(f.index);
    const wd = firstGitColChar(f.working_dir);
    if (ix === "?" && wd === "?") return "未跟踪";
    const parts = [];
    if (ix && ix !== "?") parts.push("暂存区：" + (gitColMeaning(ix) || ix));
    if (wd && wd !== "?") parts.push("未暂存：" + (gitColMeaning(wd) || wd));
    return parts.length ? parts.join(" · ") : "—";
  }

  function hasStagedIndex(f) {
    const ix = firstGitColChar(f.index);
    return !!(ix && ix !== "?");
  }

  function hasWorkingTreeChange(f) {
    const ix = firstGitColChar(f.index);
    const wd = firstGitColChar(f.working_dir);
    if (ix === "?" && wd === "?") return true;
    return !!(wd && wd !== "?");
  }

  function buildFileRow(f) {
    const row = document.createElement("div");
    row.className = "repo-sync-file-row";
    const cb = document.createElement("input");
    cb.type = "checkbox";
    cb.className = "repo-sync-file-cb";
    cb.dataset.path = f.path;
    cb.setAttribute("aria-label", "选择 " + f.path);
    const pathSpan = document.createElement("span");
    pathSpan.className = "repo-sync-file-path";
    pathSpan.textContent = f.path;
    pathSpan.title = "点击切换勾选";
    const meta = document.createElement("span");
    meta.className = "repo-sync-file-meta";
    meta.textContent = readableGitLine(f);
    row.appendChild(cb);
    row.appendChild(pathSpan);
    row.appendChild(meta);
    pathSpan.addEventListener("click", () => {
      cb.checked = !cb.checked;
    });
    return row;
  }

  function appendFileSection(container, titleText, badgeModifierClass, files, emptyHint) {
    const sec = document.createElement("div");
    sec.className = "repo-sync-file-section";
    const head = document.createElement("div");
    head.className = "repo-sync-file-section-title";
    head.appendChild(document.createTextNode(titleText + " "));
    const badge = document.createElement("span");
    badge.className =
      "repo-sync-file-section-badge" + (badgeModifierClass ? " " + badgeModifierClass : "");
    badge.textContent = files.length ? String(files.length) + " 项" : "0 项";
    head.appendChild(badge);
    sec.appendChild(head);
    if (!files.length) {
      const empty = document.createElement("p");
      empty.className = "repo-sync-file-empty";
      empty.textContent = emptyHint;
      sec.appendChild(empty);
    } else {
      for (let i = 0; i < files.length; i++) sec.appendChild(buildFileRow(files[i]));
    }
    container.appendChild(sec);
  }

  async function refreshStatus() {
    const btn = $("repo-sync-refresh");
    setBtnBusy(btn, true);
    setMsg("");
    try {
      const { res, data } = await gitApi("status", {});
      const sum = $("repo-sync-summary");
      const list = $("repo-sync-file-list");
      if (!list || !sum) return;

      if (!res.ok) {
        sum.textContent = "";
        list.innerHTML = "";
        setMsg((data && data.error) || "请求失败 " + res.status, true);
        return;
      }
      statusSnapshot = data;
      const relHint = normRepoRel($("repo-sync-repo-rel") && $("repo-sync-repo-rel").value);
      const locHint = relHint ? "子目录「" + relHint + "」" : "工作区根目录";

      if (!data.isRepo) {
        sum.textContent = data.message || "非 Git 仓库";
        list.innerHTML =
          '<p class="repo-sync-hint">' +
          locHint +
          '下不是 Git 仓库（缺少 <code>.git</code>）。请填写上方正确的<strong>相对路径</strong>指向已 clone 的仓库，或使用「浏览」选择文件夹。</p>';
        return;
      }

      sum.textContent =
        (relHint ? "仓库: " + relHint + " · " : "") +
        "分支 " +
        (data.branch || "?") +
        (data.tracking ? " → " + data.tracking : "") +
        " · ahead " +
        (data.ahead != null ? data.ahead : "?") +
        " · behind " +
        (data.behind != null ? data.behind : "?") +
        (data.clean ? " · 工作区干净" : "");

      list.innerHTML = "";
      const files = Array.isArray(data.files) ? data.files : [];
      if (files.length === 0) {
        list.innerHTML = '<p class="repo-sync-hint">暂无变更。</p>';
        return;
      }
      const stagedFiles = files.filter(hasStagedIndex);
      const workingFiles = files.filter(hasWorkingTreeChange);
      appendFileSection(
        list,
        "已暂存（将包含在下次提交）",
        "",
        stagedFiles,
        "暂无已暂存文件。在下方「工作区」勾选文件后点「暂存所选」。",
      );
      appendFileSection(
        list,
        "工作区（勾选后「暂存所选」）",
        "repo-sync-file-section-badge--amber",
        workingFiles,
        "工作区干净：当前没有需要暂存的变更。",
      );
    } finally {
      setBtnBusy(btn, false);
    }
  }

  function selectedPaths() {
    const list = $("repo-sync-file-list");
    if (!list) return [];
    return Array.from(list.querySelectorAll('input[type="checkbox"]:checked'))
      .map((c) => c.dataset.path)
      .filter(Boolean);
  }

  async function loadConflictUi() {
    const wrap = $("repo-sync-conflict-wrap");
    const sel = $("repo-sync-conflict-sel");
    if (!wrap || !sel) return;
    const { res, data } = await gitApi("conflicts-detail", {});
    if (!res.ok || !data.ok) {
      wrap.hidden = true;
      return;
    }
    conflictFiles = Array.isArray(data.files) ? data.files : [];
    if (conflictFiles.length === 0) {
      wrap.hidden = true;
      disposeDiff();
      return;
    }
    wrap.hidden = false;
    sel.innerHTML = "";
    conflictFiles.forEach((f, i) => {
      const opt = document.createElement("option");
      opt.value = String(i);
      opt.textContent = f.path;
      sel.appendChild(opt);
    });
    conflictIndex = 0;
    sel.value = "0";
    await showConflictAt(0);
  }

  async function showConflictAt(i) {
    const M = window.cctRepoSyncMonaco;
    if (!M || typeof M.mountDiffEditor !== "function") {
      setMsg("Monaco 未加载（请确认已构建 repo-sync-app 且存在 /repo-sync/repo-sync-monaco.js）", true);
      return;
    }
    const f = conflictFiles[i];
    if (!f) return;
    disposeDiff();
    const host = $("repo-sync-diff-host");
    if (!host) return;

    const { res, data } = await gitApi("diff-sides", { path: f.path });
    if (!res.ok) {
      setMsg((data && data.error) || "diff-sides 失败", true);
      return;
    }
    lastOurs = data.original != null ? String(data.original) : "";
    lastTheirs = data.modified != null ? String(data.modified) : "";
    if (!lastOurs && !lastTheirs && f.markedContent) {
      lastOurs = f.markedContent;
      lastTheirs = f.markedContent;
    }
    diffUi = M.mountDiffEditor(host, lastOurs, lastTheirs, { readOnly: false, language: "plaintext" });
  }

  async function saveCurrentConflictFile() {
    const f = conflictFiles[conflictIndex];
    if (!f || !diffUi) return;
    const content =
      typeof diffUi.getMergedModifiedText === "function" ? diffUi.getMergedModifiedText() : lastTheirs;
    const { res, data } = await gitApi("write-file", { path: f.path, content });
    if (!res.ok) {
      setMsg((data && data.error) || "写入失败", true);
      return;
    }
    await gitApi("add", { paths: [f.path] });
    setMsg("已保存并暂存: " + f.path);
    await refreshStatus();
  }

  function closeRepoPicker() {
    const ov = $("repo-sync-picker-overlay");
    if (ov) {
      ov.hidden = true;
      ov.setAttribute("aria-hidden", "true");
    }
  }

  async function renderRepoPickerList() {
    const listEl = $("repo-sync-picker-list");
    const crumbs = $("repo-sync-picker-crumbs");
    if (!listEl || !crumbs) return;
    crumbs.textContent = pickerCurRel ? pickerCurRel : "（工作区根）";
    listEl.innerHTML = '<p class="repo-sync-hint">加载中…</p>';
    const qs = pickerCurRel ? "?path=" + encodeURIComponent(pickerCurRel) : "";
    const res = await fetch("/api/workspace/list" + qs, { credentials: "include" });
    const data = await res.json().catch(() => ({}));
    if (!res.ok || !data.ok) {
      listEl.innerHTML =
        '<p class="repo-sync-hint repo-sync-msg--err">' + (data.error || "无法列出目录") + "</p>";
      return;
    }
    const entries = Array.isArray(data.entries) ? data.entries : [];
    const dirs = entries.filter((e) => e.type === "dir").sort((a, b) => a.name.localeCompare(b.name));
    listEl.innerHTML = "";
    if (dirs.length === 0) {
      listEl.innerHTML = '<p class="repo-sync-hint">当前目录下没有子文件夹。</p>';
      return;
    }
    for (const d of dirs) {
      const row = document.createElement("button");
      row.type = "button";
      row.className = "repo-sync-picker-item";
      row.textContent = d.name + " /";
      row.onclick = () => {
        pickerCurRel = pickerCurRel ? pickerCurRel + "/" + d.name : d.name;
        renderRepoPickerList();
      };
      listEl.appendChild(row);
    }
  }

  function openRepoPicker() {
    pickerCurRel = "";
    const ov = $("repo-sync-picker-overlay");
    if (ov) {
      ov.hidden = false;
      ov.setAttribute("aria-hidden", "false");
    }
    renderRepoPickerList();
  }

  function wireRepoPicker() {
    $("repo-sync-browse-repo") &&
      ($("repo-sync-browse-repo").onclick = () => {
        openRepoPicker();
      });
    $("repo-sync-picker-close") &&
      ($("repo-sync-picker-close").onclick = () => closeRepoPicker());
    $("repo-sync-picker-backdrop") &&
      ($("repo-sync-picker-backdrop").onclick = () => closeRepoPicker());
    $("repo-sync-picker-up") &&
      ($("repo-sync-picker-up").onclick = () => {
        if (!pickerCurRel) return;
        const parts = pickerCurRel.split("/").filter(Boolean);
        parts.pop();
        pickerCurRel = parts.join("/");
        renderRepoPickerList();
      });
    $("repo-sync-picker-use-root") &&
      ($("repo-sync-picker-use-root").onclick = () => {
        const inp = $("repo-sync-repo-rel");
        if (inp) inp.value = "";
        saveRepoRelToStorage();
        closeRepoPicker();
        refreshStatus();
      });
    $("repo-sync-picker-select") &&
      ($("repo-sync-picker-select").onclick = () => {
        const inp = $("repo-sync-repo-rel");
        if (inp) inp.value = pickerCurRel;
        saveRepoRelToStorage();
        closeRepoPicker();
        refreshStatus();
      });
    $("repo-sync-repo-rel") &&
      $("repo-sync-repo-rel").addEventListener("change", () => {
        saveRepoRelToStorage();
      });
  }

  function wire() {
    wireRepoPicker();

    $("repo-sync-refresh") &&
      ($("repo-sync-refresh").onclick = () => {
        saveRepoRelToStorage();
        refreshStatus();
      });

    $("repo-sync-stage") &&
      ($("repo-sync-stage").onclick = async () => {
        const btn = $("repo-sync-stage");
        const paths = selectedPaths();
        if (!paths.length) {
          setMsg("请先勾选文件", true);
          return;
        }
        setBtnBusy(btn, true);
        try {
          const { res, data } = await gitApi("stage", { paths });
          setMsg(res.ok && data.ok ? "已暂存" : data.error || "暂存失败", !res.ok || !data.ok);
          await refreshStatus();
        } finally {
          setBtnBusy(btn, false);
        }
      });

    $("repo-sync-unstage") &&
      ($("repo-sync-unstage").onclick = async () => {
        const btn = $("repo-sync-unstage");
        const paths = selectedPaths();
        if (!paths.length) {
          setMsg("请先勾选文件", true);
          return;
        }
        setBtnBusy(btn, true);
        try {
          const { res, data } = await gitApi("unstage", { paths });
          setMsg(res.ok && data.ok ? "已取消暂存" : data.error || "失败", !res.ok || !data.ok);
          await refreshStatus();
        } finally {
          setBtnBusy(btn, false);
        }
      });

    $("repo-sync-commit") &&
      ($("repo-sync-commit").onclick = async () => {
        const btn = $("repo-sync-commit");
        const msg = ($("repo-sync-commit-msg") && $("repo-sync-commit-msg").value.trim()) || "";
        if (!msg) {
          setMsg("请填写提交说明", true);
          return;
        }
        setBtnBusy(btn, true);
        try {
          const { res, data } = await gitApi("commit", { message: msg });
          setMsg(res.ok && data.ok ? "提交成功" : data.error || "提交失败", !res.ok || !data.ok);
          await refreshStatus();
        } finally {
          setBtnBusy(btn, false);
        }
      });

    $("repo-sync-push") &&
      ($("repo-sync-push").onclick = async () => {
        const btn = $("repo-sync-push");
        setBtnBusy(btn, true);
        setMsg("推送中…");
        try {
          const { res, data } = await gitApi("push", rb());
          setMsg(res.ok && data.ok ? "推送完成" : data.error || "推送失败", !res.ok || !data.ok);
          await refreshStatus();
        } finally {
          setBtnBusy(btn, false);
        }
      });

    $("repo-sync-pull") &&
      ($("repo-sync-pull").onclick = async () => {
        const btn = $("repo-sync-pull");
        setBtnBusy(btn, true);
        setMsg("拉取中…");
        try {
          const { res, data } = await gitApi("pull", rb());
          if (res.status === 409 && data.conflict) {
            setMsg("拉取产生冲突，请在下方处理。", true);
            await loadConflictUi();
            await refreshStatus();
            return;
          }
          setMsg(res.ok && data.ok ? "拉取完成" : data.error || "拉取失败", !res.ok || !data.ok);
          $("repo-sync-conflict-wrap") && ($("repo-sync-conflict-wrap").hidden = true);
          disposeDiff();
          await refreshStatus();
        } finally {
          setBtnBusy(btn, false);
        }
      });

    $("repo-sync-merge-abort") &&
      ($("repo-sync-merge-abort").onclick = async () => {
        if (!window.confirm("确定放弃本次合并（git merge --abort）？")) return;
        const btn = $("repo-sync-merge-abort");
        setBtnBusy(btn, true);
        try {
          const { res, data } = await gitApi("merge-abort", {});
          setMsg(res.ok && data.ok ? "已放弃合并" : data.error || "失败", !res.ok || !data.ok);
          $("repo-sync-conflict-wrap") && ($("repo-sync-conflict-wrap").hidden = true);
          disposeDiff();
          await refreshStatus();
        } finally {
          setBtnBusy(btn, false);
        }
      });

    $("repo-sync-conflict-sel") &&
      ($("repo-sync-conflict-sel").onchange = async (e) => {
        conflictIndex = parseInt(e.target.value, 10) || 0;
        await showConflictAt(conflictIndex);
      });

    $("repo-sync-keep-ours") &&
      ($("repo-sync-keep-ours").onclick = () => {
        if (diffUi && typeof diffUi.setModified === "function") diffUi.setModified(lastOurs);
      });
    $("repo-sync-keep-theirs") &&
      ($("repo-sync-keep-theirs").onclick = () => {
        if (diffUi && typeof diffUi.setModified === "function") diffUi.setModified(lastTheirs);
      });
    $("repo-sync-keep-both") &&
      ($("repo-sync-keep-both").onclick = () => {
        if (diffUi && typeof diffUi.setModified === "function") {
          diffUi.setModified(lastOurs + "\n\n/* ===== 传入 ===== */\n\n" + lastTheirs);
        }
      });

    $("repo-sync-save-file") &&
      ($("repo-sync-save-file").onclick = async () => {
        const btn = $("repo-sync-save-file");
        setBtnBusy(btn, true);
        try {
          await saveCurrentConflictFile();
        } finally {
          setBtnBusy(btn, false);
        }
      });

    $("repo-sync-commit-merge") &&
      ($("repo-sync-commit-merge").onclick = async () => {
        const btn = $("repo-sync-commit-merge");
        const msg = ($("repo-sync-commit-msg") && $("repo-sync-commit-msg").value.trim()) || "解决合并冲突";
        setBtnBusy(btn, true);
        try {
          const { res, data } = await gitApi("commit-merge", { message: msg });
          setMsg(res.ok && data.ok ? "合并提交完成" : data.error || "提交失败", !res.ok || !data.ok);
          $("repo-sync-conflict-wrap") && ($("repo-sync-conflict-wrap").hidden = true);
          disposeDiff();
          await refreshStatus();
        } finally {
          setBtnBusy(btn, false);
        }
      });
  }

  window.cctRepoSyncEnsureInit = function () {
    loadRepoRelFromStorage();
    if (inited) {
      refreshStatus();
      return;
    }
    inited = true;
    wire();
    refreshStatus();
  };
})();
