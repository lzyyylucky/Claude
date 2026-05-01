const cctProfile = {
  userId: "",
  username: "",
  displayName: "",
  avatarDataUrl: "",
  llmDailyLimit: 0,
  llmCallsToday: 0,
  llmTotalCalls: 0,
  subscriptionTier: "free",
  subscriptionLabel: "免费版",
  tokenQuota: 0,
  tokensConsumed: 0,
  tokensRemaining: 0,
  periodYm: 0,
};

function cctRefreshLlmUsageUi() {
  const todayEl = document.getElementById("settings-llm-today");
  const limitEl = document.getElementById("settings-llm-limit");
  const totalEl = document.getElementById("settings-llm-total");
  const prog = document.getElementById("settings-llm-progress");
  const fill = document.getElementById("settings-llm-progress-fill");
  const hint = document.getElementById("settings-llm-progress-hint");
  const lim = Number(cctProfile.llmDailyLimit);
  const today = Number(cctProfile.llmCallsToday);
  const tot = Number(cctProfile.llmTotalCalls);
  if (todayEl) todayEl.textContent = String(Number.isFinite(today) ? today : 0);
  if (limitEl) limitEl.textContent = lim > 0 ? String(lim) : "∞";
  if (totalEl) totalEl.textContent = String(Number.isFinite(tot) ? tot : 0);
  if (prog && fill && hint) {
    if (lim > 0) {
      prog.hidden = false;
      const pct = Math.min(100, Math.round((today / lim) * 100));
      fill.style.width = pct + "%";
      fill.classList.toggle("settings-llm-progress-fill--warn", pct >= 85 && pct < 100);
      fill.classList.toggle("settings-llm-progress-fill--full", pct >= 100);
      hint.textContent = "当日额度已用约 " + pct + "%（" + today + " / " + lim + "）";
    } else {
      prog.hidden = true;
      fill.style.width = "0%";
      fill.classList.remove("settings-llm-progress-fill--warn", "settings-llm-progress-fill--full");
      hint.textContent = "";
    }
  }
}

async function cctRefreshLlmUsageFromServer() {
  try {
    const res = await fetch("/api/me", { credentials: "include" });
    const data = await res.json().catch(() => ({}));
    if (!data.ok) return;
    cctProfile.llmDailyLimit = data.llmDailyLimit != null ? Number(data.llmDailyLimit) : 0;
    cctProfile.llmCallsToday = data.llmCallsToday != null ? Number(data.llmCallsToday) : 0;
    cctProfile.llmTotalCalls = data.llmTotalCalls != null ? Number(data.llmTotalCalls) : 0;
    cctProfile.subscriptionTier = String(data.subscriptionTier || "free").toLowerCase();
    cctProfile.subscriptionLabel = data.subscriptionLabel || "免费版";
    cctProfile.tokenQuota = data.tokenQuota != null ? Number(data.tokenQuota) : 0;
    cctProfile.tokensConsumed = data.tokensConsumed != null ? Number(data.tokensConsumed) : 0;
    cctProfile.tokensRemaining = data.tokensRemaining != null ? Number(data.tokensRemaining) : 0;
    cctProfile.periodYm = data.periodYm != null ? Number(data.periodYm) : 0;
    cctRefreshLlmUsageUi();
    cctApplyProfileUI();
  } catch (_) {}
}

const CCT_THEME_KEY = "cct_theme_pref";

function cctReadThemePref() {
  try {
    return localStorage.getItem(CCT_THEME_KEY) || "dark";
  } catch (_) {
    return "dark";
  }
}

function cctSetThemePref(pref, syncServer) {
  if (pref !== "dark" && pref !== "light" && pref !== "system") return;
  try {
    localStorage.setItem(CCT_THEME_KEY, pref);
  } catch (_) {}
  cctApplyThemeFromStorage();
  cctSyncSettingsThemeUi();
  if (syncServer && cctProfile.userId) {
    fetch("/api/me/preferences", {
      method: "PUT",
      credentials: "include",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ theme: pref }),
    }).catch(() => {});
  }
}

function cctEffectiveThemeFromPref() {
  const p = cctReadThemePref();
  if (p === "system") {
    return window.matchMedia("(prefers-color-scheme: light)").matches ? "light" : "dark";
  }
  return p === "light" ? "light" : "dark";
}

function cctApplyThemeFromStorage() {
  const t = cctEffectiveThemeFromPref();
  document.documentElement.setAttribute("data-theme", t);
  document.documentElement.setAttribute("data-theme-pref", cctReadThemePref());
  if (typeof window.cctIdeNotifyColorScheme === "function") {
    try {
      window.cctIdeNotifyColorScheme();
    } catch (_) {}
  }
}

let cctThemeMqlBound = false;
function cctSetupThemeListener() {
  if (cctThemeMqlBound) return;
  cctThemeMqlBound = true;
  const m = window.matchMedia("(prefers-color-scheme: light)");
  m.addEventListener("change", () => {
    if (cctReadThemePref() === "system") cctApplyThemeFromStorage();
  });
}

function cctSyncSettingsThemeUi() {
  const pref = cctReadThemePref();
  document.querySelectorAll(".settings-seg-btn").forEach((btn) => {
    const v = btn.getAttribute("data-theme-value");
    btn.setAttribute("aria-pressed", v === pref ? "true" : "false");
  });
}

/** 供后续 IDE / 项目编辑对接：当前选中的文件或文件夹（仅浏览器内演示） */
window.cctWorkspace = {
  lastFile: null,
  folderLabel: "",
  folderFileCount: 0,
  dirHandle: null,
};

function cctAvatarStorageKey() {
  return "cct_avatar_" + (cctProfile.userId || cctProfile.username || "anon");
}

function cctLoadAvatarFromStorage() {
  try {
    const v = localStorage.getItem(cctAvatarStorageKey());
    cctProfile.avatarDataUrl = v && v.startsWith("data:image/") ? v : "";
  } catch (_) {
    cctProfile.avatarDataUrl = "";
  }
}

function cctSetAvatarDataUrl(dataUrl) {
  cctProfile.avatarDataUrl = dataUrl && dataUrl.startsWith("data:image/") ? dataUrl : "";
  try {
    if (cctProfile.avatarDataUrl) localStorage.setItem(cctAvatarStorageKey(), cctProfile.avatarDataUrl);
    else localStorage.removeItem(cctAvatarStorageKey());
  } catch (_) {
    window.alert("无法写入本地存储（可能超出配额）。请换一张较小的图片。");
    return;
  }
  cctApplyAvatarUI();
}

function cctResizeImageToDataUrl(file, maxDim, jpegQuality) {
  return new Promise((resolve, reject) => {
    const fr = new FileReader();
    fr.onload = () => {
      const img = new Image();
      img.onload = () => {
        let nw = img.width;
        let nh = img.height;
        if (nw > maxDim || nh > maxDim) {
          if (nw >= nh) {
            nh = Math.round((nh * maxDim) / nw);
            nw = maxDim;
          } else {
            nw = Math.round((nw * maxDim) / nh);
            nh = maxDim;
          }
        }
        const canvas = document.createElement("canvas");
        canvas.width = nw;
        canvas.height = nh;
        const ctx = canvas.getContext("2d");
        if (!ctx) {
          reject(new Error("canvas"));
          return;
        }
        ctx.drawImage(img, 0, 0, nw, nh);
        resolve(canvas.toDataURL("image/jpeg", jpegQuality));
      };
      img.onerror = () => reject(new Error("img"));
      img.src = fr.result;
    };
    fr.onerror = () => reject(new Error("read"));
    fr.readAsDataURL(file);
  });
}

async function cctOnAvatarFileChosen(file) {
  if (!file || !file.type.startsWith("image/")) {
    window.alert("请选择图片文件（如 JPG、PNG）。");
    return;
  }
  try {
    const dataUrl = await cctResizeImageToDataUrl(file, 320, 0.82);
    cctSetAvatarDataUrl(dataUrl);
  } catch (_) {
    window.alert("图片读取失败，请换一张图片重试。");
  }
}

function cctApplyAvatarUI() {
  const url = cctProfile.avatarDataUrl;
  const sets = [
    ["user-avatar-img", "user-avatar-initials", "user-avatar-box"],
    ["menu-avatar-img", "menu-avatar-initials", "menu-avatar-box"],
    ["profile-avatar-img", "profile-avatar-text", "profile-avatar-inner"],
  ];
  for (const [imgId, textId, boxId] of sets) {
    const img = document.getElementById(imgId);
    const text = document.getElementById(textId);
    const box = document.getElementById(boxId);
    if (!img || !text) continue;
    if (url) {
      img.src = url;
      img.hidden = false;
      text.hidden = true;
      if (box) box.classList.add("has-image");
    } else {
      img.removeAttribute("src");
      img.hidden = true;
      text.hidden = false;
      if (box) box.classList.remove("has-image");
    }
  }
}

function cctInitials(displayName, username) {
  const s = String((displayName || "").trim() || (username || "").trim());
  if (!s) return "?";
  const arr = [...s];
  if (arr.length === 1) {
    const c = arr[0];
    return /[a-zA-Z]/.test(c) ? c.toUpperCase() : c;
  }
  const a = arr[0];
  const b = arr[1];
  const up = (ch) => (/[a-zA-Z]/.test(ch) ? ch.toUpperCase() : ch);
  return up(a) + up(b);
}

function cctApplyProfileUI() {
  const show = cctProfile.displayName || cctProfile.username || "用户";
  const initials = cctInitials(cctProfile.displayName, cctProfile.username);
  const handle = cctProfile.username ? "@" + cctProfile.username : "";

  const av1 = document.getElementById("user-avatar-initials");
  const av2 = document.getElementById("menu-avatar-initials");
  const av3 = document.getElementById("profile-avatar-text");
  if (av1) av1.textContent = initials;
  if (av2) av2.textContent = initials;
  if (av3) av3.textContent = initials;
  cctApplyAvatarUI();

  const dn = document.getElementById("user-display-name");
  const md = document.getElementById("menu-display-name");
  const mu = document.getElementById("menu-username");
  const st = document.getElementById("user-subscription-label");
  if (dn) dn.textContent = show;
  if (md) md.textContent = show;
  if (mu) mu.textContent = handle;
  if (st) st.textContent = cctProfile.subscriptionLabel || "免费版";
}

function cctPositionUserMenu() {
  const trigger = document.getElementById("user-menu-trigger");
  const pop = document.getElementById("user-menu-popover");
  if (!trigger || !pop || pop.hidden) return;
  const r = trigger.getBoundingClientRect();
  const gap = 8;
  pop.style.left = Math.max(12, r.left) + "px";
  pop.style.bottom = window.innerHeight - r.top + gap + "px";
  const w = Math.max(r.width, 232);
  pop.style.width = Math.min(w, window.innerWidth - 24) + "px";
}

function cctCloseUserMenu() {
  const pop = document.getElementById("user-menu-popover");
  const tr = document.getElementById("user-menu-trigger");
  if (pop) pop.hidden = true;
  if (tr) tr.setAttribute("aria-expanded", "false");
}

function cctToggleUserMenu() {
  const pop = document.getElementById("user-menu-popover");
  const tr = document.getElementById("user-menu-trigger");
  if (!pop || !tr) return;
  const open = pop.hidden;
  pop.hidden = !open;
  tr.setAttribute("aria-expanded", open ? "true" : "false");
  if (open) {
    cctPositionUserMenu();
    requestAnimationFrame(() => cctPositionUserMenu());
  }
}

function cctOpenProfileModal() {
  const ov = document.getElementById("profile-modal");
  const inp = document.getElementById("profile-display-name");
  const ro = document.getElementById("profile-username-ro");
  if (inp) inp.value = cctProfile.displayName || cctProfile.username || "";
  if (ro) ro.value = cctProfile.username || "";
  if (ov) {
    ov.hidden = false;
    ov.setAttribute("aria-hidden", "false");
    document.body.classList.add("modal-open");
  }
  cctCloseUserMenu();
}

function cctCloseProfileModal() {
  const ov = document.getElementById("profile-modal");
  if (ov) {
    ov.hidden = true;
    ov.setAttribute("aria-hidden", "true");
    document.body.classList.remove("modal-open");
  }
}

function cctOpenSettingsModal() {
  const ov = document.getElementById("settings-modal");
  cctSyncSettingsThemeUi();
  void cctRefreshLlmUsageFromServer();
  if (ov) {
    ov.hidden = false;
    ov.setAttribute("aria-hidden", "false");
    document.body.classList.add("modal-open");
  }
  cctCloseUserMenu();
}

function cctCloseSettingsModal() {
  const ov = document.getElementById("settings-modal");
  if (ov) {
    ov.hidden = true;
    ov.setAttribute("aria-hidden", "true");
    document.body.classList.remove("modal-open");
  }
}

function cctOpenHelpModal() {
  const ov = document.getElementById("help-modal");
  if (ov) {
    ov.hidden = false;
    ov.setAttribute("aria-hidden", "false");
    document.body.classList.add("modal-open");
  }
  cctCloseUserMenu();
}

function cctCloseHelpModal() {
  const ov = document.getElementById("help-modal");
  if (ov) {
    ov.hidden = true;
    ov.setAttribute("aria-hidden", "true");
    document.body.classList.remove("modal-open");
  }
}

function cctSetSidebarCollapsed(collapsed) {
  const shell = document.getElementById("app-root");
  const exp = document.getElementById("sidebar-expand");
  if (!shell) return;
  const desktop = window.matchMedia("(min-width: 769px)").matches;
  if (!desktop) {
    shell.classList.remove("sidebar-collapsed");
    if (exp) exp.hidden = true;
    return;
  }
  shell.classList.toggle("sidebar-collapsed", collapsed);
  if (exp) exp.hidden = !collapsed;
}

async function cctBoot() {
  cctSetupThemeListener();
  cctApplyThemeFromStorage();
  cctSyncSettingsThemeUi();

  const res = await fetch("/api/me", { credentials: "include" });
  if (res.status === 401) {
    window.location.href = "/index.html#login";
    return;
  }
  const data = await res.json().catch(() => ({}));
  if (!data.ok) {
    window.alert(data.error || "加载用户信息失败（HTTP " + res.status + "）");
    return;
  }
  cctProfile.userId = data.userId != null ? String(data.userId) : "";
  cctProfile.username = data.username || "";
  cctProfile.displayName = (data.displayName || data.username || "").trim() || data.username || "";
  cctProfile.llmDailyLimit = data.llmDailyLimit != null ? Number(data.llmDailyLimit) : 0;
  cctProfile.llmCallsToday = data.llmCallsToday != null ? Number(data.llmCallsToday) : 0;
  cctProfile.llmTotalCalls = data.llmTotalCalls != null ? Number(data.llmTotalCalls) : 0;
  cctProfile.subscriptionTier = String(data.subscriptionTier || "free").toLowerCase();
  cctProfile.subscriptionLabel = data.subscriptionLabel || "免费版";
  cctProfile.tokenQuota = data.tokenQuota != null ? Number(data.tokenQuota) : 0;
  cctProfile.tokensConsumed = data.tokensConsumed != null ? Number(data.tokensConsumed) : 0;
  cctProfile.tokensRemaining = data.tokensRemaining != null ? Number(data.tokensRemaining) : 0;
  cctProfile.periodYm = data.periodYm != null ? Number(data.periodYm) : 0;
  const uit = data.uiTheme;
  if (uit === "dark" || uit === "light" || uit === "system") cctSetThemePref(uit, false);
  cctLoadAvatarFromStorage();
  cctApplyProfileUI();
  cctRefreshLlmUsageUi();

  const shell = document.getElementById("app-root");
  const sidebar = document.getElementById("gpt-sidebar");
  const menuToggle = document.getElementById("menu-toggle");
  const viewTitle = document.getElementById("view-title");
  const collapseBtn = document.getElementById("sidebar-collapse");
  const expandBtn = document.getElementById("sidebar-expand");

  const chatTopActions = document.getElementById("gpt-top-chat-actions");

  function cctCloseChatHistoryPanel() {
    const ov = document.getElementById("chat-history-overlay");
    if (ov) {
      ov.hidden = true;
      ov.setAttribute("aria-hidden", "true");
    }
  }

  function setView(name) {
    if (name !== "chat" && typeof window.cctChatProjectPauseForOtherPanels === "function") {
      window.cctChatProjectPauseForOtherPanels();
    }
    shell.dataset.activeView = name;
    document.querySelectorAll(".gpt-side-item[data-view]").forEach((b) => {
      b.classList.toggle("active", b.dataset.view === name);
    });
    document.querySelectorAll(".gpt-panel").forEach((p) => {
      p.classList.toggle("active", p.id === "panel-" + name);
    });
    if (name === "chat") {
      if (viewTitle) viewTitle.hidden = true;
      if (chatTopActions) chatTopActions.hidden = false;
    } else {
      if (viewTitle) {
        viewTitle.hidden = false;
        viewTitle.textContent =
          name === "ide"
            ? "代码工作区"
            : name === "components"
              ? "组件工作室"
              : name === "code-scan"
                ? "代码检测"
                : name === "analytics"
                  ? "数据分析"
                  : name === "repo-sync"
                    ? "仓库同步"
                    : "";
      }
      if (chatTopActions) chatTopActions.hidden = true;
    }
    shell.classList.remove("sidebar-open");
    if (name === "chat" && typeof window.cctChatProjectResumeIfWorkspace === "function") {
      window.cctChatProjectResumeIfWorkspace();
    }
    if (name === "chat" && typeof window.cctChatRefreshAgentSkillOptions === "function") {
      window.cctChatRefreshAgentSkillOptions();
    }
    if (name === "components") cctComp.refreshList();
    if (name === "ide" && typeof window.cctIdeBoot === "function") window.cctIdeBoot();
    if (name === "code-scan" && typeof window.cctCodeScanEnsureInit === "function") window.cctCodeScanEnsureInit();
    if (name === "analytics") {
      if (typeof window.cctAnalyticsEnsureInit === "function") window.cctAnalyticsEnsureInit();
      if (typeof window.cctAnalyticsReload === "function") window.cctAnalyticsReload();
    }
    if (name === "repo-sync" && typeof window.cctRepoSyncEnsureInit === "function") window.cctRepoSyncEnsureInit();
  }

  window.cctSetActiveAppView = function (name) {
    setView(name);
  };

  document.querySelectorAll(".gpt-side-item[data-view]").forEach((btn) => {
    btn.onclick = () => setView(btn.dataset.view);
  });

  if (menuToggle && sidebar) {
    menuToggle.onclick = () => shell.classList.toggle("sidebar-open");
  }

  if (collapseBtn) {
    collapseBtn.onclick = () => {
      if (window.matchMedia("(max-width: 768px)").matches) {
        shell.classList.remove("sidebar-open");
      } else {
        cctSetSidebarCollapsed(true);
      }
    };
  }
  if (expandBtn) {
    expandBtn.onclick = () => cctSetSidebarCollapsed(false);
  }

  window.addEventListener("resize", () => {
    cctSetSidebarCollapsed(shell.classList.contains("sidebar-collapsed"));
    cctPositionUserMenu();
  });

  const userTrigger = document.getElementById("user-menu-trigger");
  const userPop = document.getElementById("user-menu-popover");
  if (userTrigger) {
    userTrigger.onclick = (e) => {
      e.stopPropagation();
      cctToggleUserMenu();
    };
  }

  document.getElementById("user-menu-profile-open")?.addEventListener("click", (e) => {
    e.stopPropagation();
    cctOpenProfileModal();
  });

  document.getElementById("menu-settings")?.addEventListener("click", () => cctOpenSettingsModal());
  document.getElementById("menu-billing-upgrade")?.addEventListener("click", (e) => {
    e.stopPropagation();
    cctCloseUserMenu();
    if (typeof window.cctOpenBillingPlans === "function") window.cctOpenBillingPlans();
  });
  document.getElementById("sidebar-upgrade-btn")?.addEventListener("click", (e) => {
    e.stopPropagation();
    cctCloseUserMenu();
    if (typeof window.cctOpenBillingPlans === "function") window.cctOpenBillingPlans();
  });
  document.getElementById("menu-help")?.addEventListener("click", () => {
    cctOpenHelpModal();
  });

  document.getElementById("menu-logout")?.addEventListener("click", async (e) => {
    e.preventDefault();
    e.stopPropagation();
    try {
      await fetch("/api/logout", { method: "POST", credentials: "include" });
    } catch (_) {}
    window.location.href = "/index.html";
  });

  document.addEventListener("click", (e) => {
    if (!userPop || userPop.hidden) return;
    if (e.target.closest("#user-menu-popover") || e.target.closest("#user-menu-trigger")) return;
    cctCloseUserMenu();
  });

  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape") {
      cctCloseUserMenu();
      cctCloseProfileModal();
      cctCloseSettingsModal();
      cctCloseHelpModal();
      const hist = document.getElementById("chat-history-overlay");
      if (hist && !hist.hidden) cctCloseChatHistoryPanel();
    }
  });

  const avatarInput = document.getElementById("avatar-file-input");
  document.getElementById("profile-avatar-picker")?.addEventListener("click", () => avatarInput?.click());
  document.getElementById("profile-avatar-reset")?.addEventListener("click", () => {
    cctSetAvatarDataUrl("");
  });
  avatarInput?.addEventListener("change", (e) => {
    const f = e.target.files && e.target.files[0];
    e.target.value = "";
    if (f) cctOnAvatarFileChosen(f);
  });

  if (typeof cctChatProjectBoot === "function") cctChatProjectBoot();

  document.getElementById("profile-modal-backdrop")?.addEventListener("click", cctCloseProfileModal);
  document.getElementById("profile-cancel")?.addEventListener("click", cctCloseProfileModal);
  document.getElementById("settings-modal-backdrop")?.addEventListener("click", cctCloseSettingsModal);
  document.getElementById("settings-close")?.addEventListener("click", cctCloseSettingsModal);
  document.querySelectorAll(".settings-seg-btn").forEach((btn) => {
    btn.addEventListener("click", () => {
      const v = btn.getAttribute("data-theme-value");
      if (v === "dark" || v === "light" || v === "system") cctSetThemePref(v, true);
    });
  });

  document.getElementById("profile-save")?.addEventListener("click", async (e) => {
    e.preventDefault();
    e.stopPropagation();
    const inp = document.getElementById("profile-display-name");
    const name = (inp && inp.value.trim()) || "";
    if (!name) {
      window.alert("显示名称不能为空");
      return;
    }
    let r;
    try {
      r = await fetch("/api/me/profile", {
        method: "PUT",
        headers: { "Content-Type": "application/json" },
        credentials: "include",
        body: JSON.stringify({ display_name: name }),
      });
    } catch (err) {
      window.alert("网络错误：" + (err && err.message ? err.message : String(err)));
      return;
    }
    const out = await r.json().catch(() => ({}));
    if (!r.ok || !out.ok) {
      window.alert(out.error || "保存失败（HTTP " + r.status + "）");
      return;
    }
    cctProfile.displayName = (out.displayName || name).trim();
    cctProfile.username = out.username || cctProfile.username;
    cctApplyProfileUI();
    cctCloseProfileModal();
  });

  cctChat.box = document.getElementById("chat-messages");
  cctChat.input = document.getElementById("chat-input");
  cctChat.scrollEl = document.querySelector(".chat-scroll");

  document.getElementById("chat-send").onclick = () => cctChat.send();
  document.getElementById("chat-clear").onclick = () => cctChat.clear();
  cctChat.input.addEventListener("input", () => cctChat.resizeInput());
  cctChat.resizeInput();
  cctChat.input.addEventListener("keydown", (e) => {
    if (e.key === "Enter" && !e.shiftKey) {
      e.preventDefault();
      cctChat.send();
    }
  });

  if (typeof window.cctModelPickerInit === "function") {
    window.cctModelPickerInit();
  }

  if (typeof window.cctChatRefreshAgentSkillOptions === "function") {
    window.cctChatRefreshAgentSkillOptions();
  }

  document.getElementById("btn-new-chat")?.addEventListener("click", async () => {
    try {
      const r = await fetch("/api/chat/threads", { method: "POST", credentials: "include" });
      const d = await r.json().catch(() => ({}));
      if (!d.ok) {
        window.alert(d.error || "新建对话失败");
        return;
      }
      if (typeof window.cctSetActiveThreadId === "function") {
        window.cctSetActiveThreadId(d.id || "");
      }
      if (cctChat.box) cctChat.box.innerHTML = "";
      cctChat.showWelcome();
      if (typeof window.cctChatProjectEnsureServerWorkspaceForThread === "function") {
        await window.cctChatProjectEnsureServerWorkspaceForThread(d.id || "");
      }
      if (typeof window.cctReloadChatHistoryList === "function") {
        await window.cctReloadChatHistoryList();
      }
    } catch (_) {
      window.alert("网络错误，请稍后重试。");
    }
  });

  document.getElementById("btn-chat-history")?.addEventListener("click", () => {
    if (typeof window.cctOpenChatHistoryPanel === "function") {
      window.cctOpenChatHistoryPanel();
    }
  });

  /** 刷新页面后仍可根据上次会话的消息推断左侧树锚点（不写聊天 DOM，仅补 localStorage） */
  (async function cctRestoreThreadWorkspaceAnchorIfPossible() {
    const tid =
      typeof window.cctGetActiveThreadId === "function" ? window.cctGetActiveThreadId() : "";
    if (!tid || tid === "main") return;
    try {
      const r = await fetch("/api/chat/thread?id=" + encodeURIComponent(tid), { credentials: "include" });
      const d = await r.json().catch(() => ({}));
      if (!d.ok || !Array.isArray(d.messages)) return;
      if (typeof window.cctChatProjectSyncThreadWorkspaceAnchorFromApi === "function") {
        window.cctChatProjectSyncThreadWorkspaceAnchorFromApi(tid, d, "");
      } else {
        if (d.workspaceAnchor && typeof window.cctChatProjectSetThreadWorkspaceAnchor === "function") {
          window.cctChatProjectSetThreadWorkspaceAnchor(tid, d.workspaceAnchor);
        } else if (typeof window.cctChatProjectInferAnchorFromMessages === "function") {
          window.cctChatProjectInferAnchorFromMessages(tid, d.messages);
        }
      }
    } catch (_) {}
  })();

  document.getElementById("chat-history-close")?.addEventListener("click", cctCloseChatHistoryPanel);
  document.getElementById("chat-history-backdrop")?.addEventListener("click", cctCloseChatHistoryPanel);

  document.getElementById("help-modal-backdrop")?.addEventListener("click", cctCloseHelpModal);
  document.getElementById("help-modal-close")?.addEventListener("click", cctCloseHelpModal);
  document.getElementById("help-modal-done")?.addEventListener("click", cctCloseHelpModal);

  setView(shell.dataset.activeView || "chat");

  const compKindEl = document.getElementById("comp-kind");
  if (compKindEl && compKindEl.value) cctComp.kind = compKindEl.value;

  document.getElementById("comp-kind").onchange = (e) => {
    cctComp.kind = e.target.value;
    cctComp.current = "";
    cctComp.refreshList();
  };
  document.getElementById("comp-refresh").onclick = () => cctComp.refreshList();
  document.getElementById("comp-new").onclick = () => cctComp.newDoc();
  document.getElementById("comp-save").onclick = () => cctComp.save();
  document.getElementById("comp-delete").onclick = () => cctComp.deleteDoc();
}

if (document.readyState === "loading") {
  document.addEventListener("DOMContentLoaded", cctBoot);
} else {
  cctBoot();
}

window.cctRefreshLlmUsageFromServer = cctRefreshLlmUsageFromServer;
