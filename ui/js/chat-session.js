/** @ui — 当前对话线程（与后端 thread_id 对应，存 localStorage 以便刷新/重登后仍恢复） */
(function () {
  const KEY = "cct_active_thread_id";
  window.cctGetActiveThreadId = function () {
    try {
      const v = localStorage.getItem(KEY);
      return v && String(v).trim() ? String(v).trim() : "main";
    } catch (_) {
      return "main";
    }
  };
  window.cctSetActiveThreadId = function (id) {
    try {
      if (id) localStorage.setItem(KEY, String(id));
      else localStorage.removeItem(KEY);
    } catch (_) {}
    try {
      if (typeof window.cctOnActiveThreadChanged === "function") window.cctOnActiveThreadChanged(id || "");
    } catch (_) {}
  };
})();
