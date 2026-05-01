(function () {
  const overlay = document.getElementById("auth-overlay");
  const backdrop = document.getElementById("auth-backdrop");
  const closeBtn = document.getElementById("auth-close");
  const btnExp = document.getElementById("btn-experience");
  const btnLater = document.getElementById("btn-later");
  const toast = document.getElementById("toast-later");
  const tabLogin = document.getElementById("tab-login");
  const tabReg = document.getElementById("tab-register");
  const submit = document.getElementById("auth-submit");
  const title = document.getElementById("auth-title");

  let mode = "login";

  function openAuth(m) {
    mode = m || "login";
    syncTabs();
    overlay.hidden = false;
    overlay.setAttribute("aria-hidden", "false");
    document.body.classList.add("modal-open");
    document.getElementById("modal-username").focus();
  }

  function closeAuth() {
    overlay.hidden = true;
    overlay.setAttribute("aria-hidden", "true");
    document.body.classList.remove("modal-open");
    if (window.cctAuth) cctAuth.showErr("");
  }

  function syncTabs() {
    tabLogin.classList.toggle("active", mode === "login");
    tabReg.classList.toggle("active", mode === "register");
    tabLogin.setAttribute("aria-selected", mode === "login");
    tabReg.setAttribute("aria-selected", mode === "register");
    title.textContent = mode === "login" ? "登录" : "注册";
    submit.textContent = mode === "login" ? "继续" : "创建账号";
    const regExtra = document.getElementById("auth-register-extra");
    if (regExtra) regExtra.hidden = mode !== "register";
    if (mode === "register" && window.cctAuth && typeof window.cctAuth.loadCaptcha === "function") {
      window.cctAuth.loadCaptcha();
    }
  }

  btnExp.addEventListener("click", () => openAuth("login"));
  closeBtn.addEventListener("click", closeAuth);
  backdrop.addEventListener("click", closeAuth);
  tabLogin.addEventListener("click", () => {
    mode = "login";
    syncTabs();
    if (window.cctAuth) cctAuth.showErr("");
  });
  tabReg.addEventListener("click", () => {
    mode = "register";
    syncTabs();
    if (window.cctAuth) cctAuth.showErr("");
  });

  document.getElementById("captcha-refresh")?.addEventListener("click", () => {
    if (window.cctAuth && typeof window.cctAuth.loadCaptcha === "function") window.cctAuth.loadCaptcha();
  });

  submit.addEventListener("click", () => {
    if (mode === "login") cctAuth.login();
    else cctAuth.register();
  });

  btnLater.addEventListener("click", () => {
    toast.textContent = "好的，随时点击「即刻体验」即可登录。";
    toast.hidden = false;
    clearTimeout(btnLater._t);
    btnLater._t = setTimeout(() => {
      toast.hidden = true;
    }, 3200);
  });

  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape" && !overlay.hidden) closeAuth();
  });

  overlay.addEventListener("keydown", (e) => {
    if (e.key !== "Enter" || e.isComposing) return;
    const t = e.target;
    if (!t || t.tagName !== "INPUT" || !t.classList || !t.classList.contains("auth-pill-input")) return;
    e.preventDefault();
    if (mode === "login") cctAuth.login();
    else cctAuth.register();
  });

  const h = (location.hash || "").replace("#", "");
  if (h === "login" || h === "register") {
    openAuth(h === "register" ? "register" : "login");
  }
})();