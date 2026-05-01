const cctAuth = {
  captchaId: "",
  /** @type {string|null} 释放上一张 Blob 验证码，避免泄漏 object URL */
  _captchaObjectUrl: null,
  _revokeCaptchaObjectUrl() {
    if (cctAuth._captchaObjectUrl) {
      try {
        URL.revokeObjectURL(cctAuth._captchaObjectUrl);
      } catch (_) {}
      cctAuth._captchaObjectUrl = null;
    }
  },
  errEl() {
    return document.getElementById("auth-err") || document.getElementById("err");
  },
  userEl() {
    return document.getElementById("modal-username") || document.getElementById("username");
  },
  passEl() {
    return document.getElementById("modal-password") || document.getElementById("password");
  },
  captchaAnswerEl() {
    return document.getElementById("captcha-answer");
  },
  showErr(msg) {
    const e = this.errEl();
    if (!e) return;
    e.textContent = msg;
    e.hidden = !msg;
  },
  async loadCaptcha() {
    const host = document.getElementById("captcha-svg-host");
    const inp = this.captchaAnswerEl();
    this.captchaId = "";
    this._revokeCaptchaObjectUrl();
    if (inp) inp.value = "";
    if (!host) return;
    host.innerHTML = "";
    try {
      const res = await fetch("/api/captcha");
      const data = await res.json().catch(() => ({}));
      if (!data.ok || !data.svg || !data.id) {
        host.innerHTML = "<span class=\"auth-captcha-fallback\">验证码加载失败</span>";
        return;
      }
      this.captchaId = data.id;
      host.innerHTML = "";
      const svgRaw = String(data.svg || "").trim();

      const appendParsedSvg = () => {
        const doc = new DOMParser().parseFromString(svgRaw, "image/svg+xml");
        const err = doc.querySelector("parsererror");
        const root = doc.documentElement;
        if (!err && root && root.nodeName.toLowerCase() === "svg") {
          host.appendChild(document.importNode(root, true));
          return true;
        }
        return false;
      };

      const appendImg = (src) => {
        const img = document.createElement("img");
        img.className = "auth-captcha-img";
        img.alt = "验证码";
        img.decoding = "async";
        img.width = 168;
        img.height = 52;
        img.loading = "eager";
        img.src = src;
        img.onerror = () => {
          host.innerHTML = "";
          if (!appendParsedSvg()) {
            host.innerHTML = "<span class=\"auth-captcha-fallback\">验证码渲染失败</span>";
          }
        };
        host.appendChild(img);
      };

      /** 1) 直接挂载 SVG（根因：部分 Chromium/WebView 对 img+data/blob URL 的 SVG 解码为空但仍触发 load） */
      if (appendParsedSvg()) return;

      /** 2) Blob URL（较 data URL 更长但更兼容） */
      try {
        const blob = new Blob([svgRaw], { type: "image/svg+xml;charset=utf-8" });
        const url = URL.createObjectURL(blob);
        this._captchaObjectUrl = url;
        appendImg(url);
        return;
      } catch (_) {}

      /** 3) UTF-8 base64 data URL */
      try {
        const b64 = btoa(unescape(encodeURIComponent(svgRaw)));
        appendImg("data:image/svg+xml;base64," + b64);
      } catch (_) {
        host.innerHTML = "<span class=\"auth-captcha-fallback\">验证码渲染失败</span>";
      }
    } catch (_) {
      host.innerHTML = "<span class=\"auth-captcha-fallback\">网络异常</span>";
    }
  },
  async register() {
    this.showErr("");
    const username = this.userEl().value.trim();
    const password = this.passEl().value;
    const captcha_answer = this.captchaAnswerEl() ? this.captchaAnswerEl().value.trim() : "";
    const captcha_id = this.captchaId || "";
    const res = await fetch("/api/register", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      credentials: "include",
      body: JSON.stringify({ username, password, captcha_id, captcha_answer }),
    });
    const data = await res.json().catch(() => ({}));
    if (!data.ok) {
      this.showErr(data.error || "注册失败");
      return;
    }
    const tabLogin = document.getElementById("tab-login");
    const tabReg = document.getElementById("tab-register");
    if (tabLogin && tabReg) {
      this.userEl().value = username;
      this.passEl().value = "";
      tabLogin.click();
      setTimeout(() => this.showErr("注册成功，请登录"), 20);
      return;
    }
    window.location.href = "/index.html#login";
  },
  async login() {
    this.showErr("");
    const username = this.userEl().value.trim();
    const password = this.passEl().value;
    const res = await fetch("/api/login", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      credentials: "include",
      body: JSON.stringify({ username, password }),
    });
    const data = await res.json().catch(() => ({}));
    if (!data.ok) {
      this.showErr(data.error || "登录失败");
      return;
    }
    window.location.href = "/app.html";
  },
};

window.cctAuth = cctAuth;