(function () {
  let plansCache = [];
  let checkoutPlan = null;
  let payMethod = "wechat";

  function openOverlay(el) {
    if (!el) return;
    el.hidden = false;
    el.setAttribute("aria-hidden", "false");
    document.body.classList.add("modal-open");
  }

  function closeOverlay(el) {
    if (!el) return;
    el.hidden = true;
    el.setAttribute("aria-hidden", "true");
  }

  function syncBodyModalClass() {
    const open =
      document.getElementById("billing-plans-modal")?.hidden === false ||
      document.getElementById("billing-checkout-modal")?.hidden === false ||
      document.getElementById("profile-modal")?.hidden === false ||
      document.getElementById("settings-modal")?.hidden === false ||
      document.getElementById("help-modal")?.hidden === false ||
      document.getElementById("chat-history-overlay")?.hidden === false;
    if (open) document.body.classList.add("modal-open");
    else document.body.classList.remove("modal-open");
  }

  async function fetchPlans() {
    const r = await fetch("/api/billing/plans", { credentials: "include" });
    const d = await r.json().catch(() => ({}));
    if (!d.ok || !Array.isArray(d.plans)) return [];
    return d.plans;
  }

  async function getCurrentTier() {
    const r = await fetch("/api/me", { credentials: "include" });
    const d = await r.json().catch(() => ({}));
    if (!d.ok) return "free";
    return String(d.subscriptionTier || "free").toLowerCase();
  }

  function tierOrder(t) {
    const o = { free: 0, go: 1, plus: 2, pro: 3 };
    return o[t] != null ? o[t] : 0;
  }

  function planTierLabel(p) {
    if (p.tier === "free") return "免费版";
    return p.name || p.tier;
  }

  function renderPlans(currentTier) {
    const grid = document.getElementById("billing-plans-grid");
    if (!grid) return;
    grid.innerHTML = "";
    const curOrder = tierOrder(currentTier);
    plansCache.forEach((p) => {
      const tier = String(p.tier || "").toLowerCase();
      const card = document.createElement("div");
      card.className = "billing-plan-card";
      if (p.popular) card.classList.add("billing-plan-card-popular");
      if (p.popular) {
        const b = document.createElement("span");
        b.className = "billing-plan-badge";
        b.textContent = "热门";
        card.appendChild(b);
      }
      const h = document.createElement("h3");
      h.className = "billing-plan-name";
      h.textContent = planTierLabel(p);
      card.appendChild(h);
      const price = document.createElement("div");
      price.className = "billing-plan-price";
      const yuan = Number(p.priceCny) || 0;
      price.textContent = "¥" + yuan;
      if (yuan > 0) price.textContent += " /月";
      card.appendChild(price);
      const desc = document.createElement("p");
      desc.className = "billing-plan-desc";
      desc.textContent =
        tier === "free"
          ? "了解 AI 与代码助手能力"
          : tier === "go"
            ? "解锁更多额度，日常开发够用"
            : tier === "plus"
              ? "解锁全面体验与更高额度"
              : "团队与高强度使用";
      card.appendChild(desc);
      const dcap = p.dailyCallCap != null ? Number(p.dailyCallCap) : 0;
      if (dcap > 0) {
        const dc = document.createElement("p");
        dc.className = "billing-plan-desc";
        dc.style.marginTop = "-0.15rem";
        dc.textContent = "单日模型调用上限 " + dcap + " 次（对话+检测合计）";
        card.appendChild(dc);
      } else if (tier === "free") {
        const dc = document.createElement("p");
        dc.className = "billing-plan-desc";
        dc.style.marginTop = "-0.15rem";
        dc.textContent = "免费档单日默认不限，可用配置单独限定";
        card.appendChild(dc);
      }
      const ul = document.createElement("ul");
      ul.className = "billing-plan-features";
      (p.features || []).forEach((f) => {
        const li = document.createElement("li");
        li.textContent = f;
        ul.appendChild(li);
      });
      card.appendChild(ul);
      const btn = document.createElement("button");
      btn.type = "button";
      btn.className = "billing-plan-cta";
      const isCurrent = tier === currentTier;
      const higher = tierOrder(tier) > curOrder;
      if (tier === "free") {
        btn.classList.add("billing-plan-cta-current");
        btn.textContent = curOrder > 0 ? "不可降级" : "你当前的套餐";
        btn.disabled = true;
      } else if (isCurrent) {
        btn.classList.add("billing-plan-cta-current");
        btn.textContent = "你当前的套餐";
        btn.disabled = true;
      } else if (!higher) {
        btn.classList.add("billing-plan-cta-current");
        btn.textContent = "不可降级";
        btn.disabled = true;
      } else {
        if (p.popular) btn.classList.add("billing-plan-cta-primary");
        btn.textContent = "升级至 " + (p.name || p.tier);
        btn.addEventListener("click", () => openCheckout(p));
      }
      card.appendChild(btn);
      grid.appendChild(card);
    });
  }

  function openCheckout(plan) {
    checkoutPlan = plan;
    payMethod = "wechat";
    const chk = document.getElementById("billing-checkout-modal");
    const pl = document.getElementById("billing-plans-modal");
    const title = document.getElementById("billing-summary-plan-name");
    const feats = document.getElementById("billing-summary-features");
    const priceEl = document.getElementById("billing-summary-price");
    const totalEl = document.getElementById("billing-summary-total");
    const quotaEl = document.getElementById("billing-summary-quota");
    if (title) title.textContent = (plan.name || plan.tier) + " 套餐";
    if (feats) {
      feats.innerHTML = "";
      (plan.features || []).forEach((f) => {
        const li = document.createElement("li");
        li.textContent = f;
        feats.appendChild(li);
      });
    }
    const yuan = Number(plan.priceCny) || 0;
    if (priceEl) priceEl.textContent = "¥" + yuan.toFixed(2) + " / 月";
    if (totalEl) totalEl.textContent = "¥" + yuan.toFixed(2);
    if (quotaEl) quotaEl.textContent = plan.tokenQuota != null ? String(plan.tokenQuota) : "—";
    document.querySelectorAll(".billing-pay-tile").forEach((t) => {
      const pm = t.getAttribute("data-pay");
      const on = pm === "wechat";
      t.classList.toggle("billing-pay-tile-active", on);
      t.setAttribute("aria-checked", on ? "true" : "false");
    });
    closeOverlay(pl);
    openOverlay(chk);
    syncBodyModalClass();
  }

  function closePlans() {
    closeOverlay(document.getElementById("billing-plans-modal"));
    syncBodyModalClass();
  }

  function closeCheckout() {
    closeOverlay(document.getElementById("billing-checkout-modal"));
    syncBodyModalClass();
  }

  async function doSubscribe() {
    if (!checkoutPlan || !checkoutPlan.tier) return;
    const subBtn = document.getElementById("billing-checkout-subscribe");
    if (subBtn) subBtn.disabled = true;
    try {
      const r = await fetch("/api/billing/subscribe", {
        method: "POST",
        credentials: "include",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ tier: checkoutPlan.tier, payMethod }),
      });
      const d = await r.json().catch(() => ({}));
      if (!r.ok || !d.ok) {
        window.alert((d && d.error) || "订阅失败");
        return;
      }
      window.alert("订阅成功（演示）。本月配额已更新。");
      closeCheckout();
      closePlans();
      if (typeof window.cctRefreshLlmUsageFromServer === "function") {
        await window.cctRefreshLlmUsageFromServer();
      }
    } catch (e) {
      window.alert("网络错误：" + (e && e.message ? e.message : String(e)));
    } finally {
      if (subBtn) subBtn.disabled = false;
    }
  }

  function init() {
    document.getElementById("billing-plans-backdrop")?.addEventListener("click", closePlans);
    document.getElementById("billing-plans-close")?.addEventListener("click", closePlans);
    document.getElementById("billing-checkout-backdrop")?.addEventListener("click", closeCheckout);
    document.getElementById("billing-checkout-back")?.addEventListener("click", () => {
      closeCheckout();
      openOverlay(document.getElementById("billing-plans-modal"));
      syncBodyModalClass();
    });
    document.getElementById("billing-checkout-subscribe")?.addEventListener("click", doSubscribe);
    document.querySelectorAll(".billing-pay-tile").forEach((t) => {
      t.addEventListener("click", () => {
        payMethod = t.getAttribute("data-pay") || "wechat";
        document.querySelectorAll(".billing-pay-tile").forEach((x) => {
          const on = x === t;
          x.classList.toggle("billing-pay-tile-active", on);
          x.setAttribute("aria-checked", on ? "true" : "false");
        });
      });
    });
    const hintEl = document.getElementById("billing-plans-hint");
    const hintText = "企业版与个人版展示相同（演示）";
    document.getElementById("billing-seg-personal")?.addEventListener("click", () => {
      document.getElementById("billing-seg-business")?.classList.remove("billing-seg-btn-active");
      document.getElementById("billing-seg-personal")?.classList.add("billing-seg-btn-active");
      if (hintEl) hintEl.textContent = hintText;
    });
    document.getElementById("billing-seg-business")?.addEventListener("click", () => {
      document.getElementById("billing-seg-personal")?.classList.remove("billing-seg-btn-active");
      document.getElementById("billing-seg-business")?.classList.add("billing-seg-btn-active");
      if (hintEl) hintEl.textContent = hintText;
    });

    document.addEventListener("keydown", (e) => {
      if (e.key !== "Escape") return;
      if (document.getElementById("billing-checkout-modal")?.hidden === false) {
        closeCheckout();
        openOverlay(document.getElementById("billing-plans-modal"));
        syncBodyModalClass();
        e.preventDefault();
      } else if (document.getElementById("billing-plans-modal")?.hidden === false) {
        closePlans();
        syncBodyModalClass();
      }
    });
  }

  window.cctOpenBillingPlans = async function () {
    const pop = document.getElementById("user-menu-popover");
    if (pop) pop.hidden = true;
    const tr = document.getElementById("user-menu-trigger");
    if (tr) tr.setAttribute("aria-expanded", "false");
    plansCache = await fetchPlans();
    const cur = await getCurrentTier();
    renderPlans(cur);
    openOverlay(document.getElementById("billing-plans-modal"));
    syncBodyModalClass();
  };

  init();
})();
