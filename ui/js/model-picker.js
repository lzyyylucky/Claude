/**
 * @ui — 顶部模型选择器（与 /api/llm-info 同步；所选模型随 /api/chat/stream 与 /api/chat 的 model 字段提交）
 */
(function () {
  const LS_KEY = "cct_selected_model_id";

  let serverDefault = "";
  let provider = "mock";
  let modelsFromServer = [];

  function safeModelId(s) {
    return typeof s === "string" && /^[a-zA-Z0-9][a-zA-Z0-9._-]{0,63}$/.test(s);
  }

  window.cctGetSelectedChatModel = function () {
    const saved = (() => {
      try {
        return localStorage.getItem(LS_KEY) || "";
      } catch (_) {
        return "";
      }
    })();
    if (saved && safeModelId(saved)) return saved;
    return serverDefault && safeModelId(serverDefault) ? serverDefault : "";
  };

  function persistModel(id) {
    try {
      if (id) localStorage.setItem(LS_KEY, id);
      else localStorage.removeItem(LS_KEY);
    } catch (_) {}
  }

  function labelForId(id) {
    if (!id) return "模型";
    const short = id.length > 22 ? id.slice(0, 20) + "…" : id;
    return short;
  }

  function closeDropdown() {
    const dd = document.getElementById("model-picker-dropdown");
    const tr = document.getElementById("model-picker-trigger");
    if (dd) dd.hidden = true;
    if (tr) tr.setAttribute("aria-expanded", "false");
  }

  function openDropdown() {
    const dd = document.getElementById("model-picker-dropdown");
    const tr = document.getElementById("model-picker-trigger");
    if (dd) dd.hidden = false;
    if (tr) tr.setAttribute("aria-expanded", "true");
  }

  function renderDropdown() {
    const dd = document.getElementById("model-picker-dropdown");
    if (!dd) return;
    dd.innerHTML = "";
    const cur = window.cctGetSelectedChatModel();
    let list = Array.isArray(modelsFromServer) && modelsFromServer.length ? modelsFromServer.slice() : [];
    if (serverDefault && safeModelId(serverDefault) && !list.includes(serverDefault)) list.unshift(serverDefault);
    if (cur && safeModelId(cur) && !list.includes(cur)) list.push(cur);
    if (!list.length && serverDefault && safeModelId(serverDefault)) list = [serverDefault];

    list.forEach((id) => {
      if (!safeModelId(id)) return;
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "model-picker-item" + (id === cur ? " is-active" : "");
      btn.setAttribute("role", "option");
      btn.setAttribute("aria-selected", id === cur ? "true" : "false");
      btn.dataset.modelId = id;
      const check = document.createElement("span");
      check.className = "model-picker-check";
      check.textContent = "✓";
      const main = document.createElement("span");
      main.className = "model-picker-item-main";
      const title = document.createElement("span");
      title.className = "model-picker-item-title";
      title.textContent = id;
      const desc = document.createElement("span");
      desc.className = "model-picker-item-desc";
      desc.textContent =
        provider === "zhipu"
          ? "智谱开放平台 · 对话补全"
          : provider === "anthropic"
            ? "Anthropic Messages API"
            : provider === "mock"
              ? "占位响应（与本地演示一致）"
              : "服务端配置";
      main.appendChild(title);
      main.appendChild(desc);
      btn.appendChild(check);
      btn.appendChild(main);
      btn.addEventListener("click", () => {
        persistModel(id);
        syncTriggerLabel();
        renderDropdown();
        closeDropdown();
      });
      dd.appendChild(btn);
    });
  }

  function syncTriggerLabel() {
    const lab = document.getElementById("model-picker-label");
    if (lab) lab.textContent = labelForId(window.cctGetSelectedChatModel());
  }

  window.cctModelPickerInit = async function () {
    const root = document.getElementById("model-picker-root");
    const tr = document.getElementById("model-picker-trigger");
    if (!root || !tr) return;

    try {
      const r = await fetch("/api/llm-info", { credentials: "same-origin" });
      const d = await r.json().catch(() => ({}));
      if (d.ok) {
        provider = d.provider || "mock";
        serverDefault = d.model || "";
        modelsFromServer = Array.isArray(d.models) ? d.models.filter(safeModelId) : [];
      }
    } catch (_) {}

    if (!window.cctGetSelectedChatModel() && serverDefault) persistModel(serverDefault);
    syncTriggerLabel();
    renderDropdown();

    tr.addEventListener("click", (e) => {
      e.stopPropagation();
      const dd = document.getElementById("model-picker-dropdown");
      if (!dd) return;
      if (dd.hidden) {
        renderDropdown();
        openDropdown();
      } else {
        closeDropdown();
      }
    });

    document.addEventListener("click", (e) => {
      if (!e.target.closest("#model-picker-root")) closeDropdown();
    });
    document.addEventListener("keydown", (e) => {
      if (e.key === "Escape") closeDropdown();
    });
  };
})();
