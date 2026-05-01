const cctComp = {
  kind: "agents",
  current: "",
  /** 与下拉框 #comp-kind 保持一致，避免刷新列表 / 保存后用错类（Agents↔Skills 串栏） */
  syncKindFromDom() {
    const sel = document.getElementById("comp-kind");
    if (sel && sel.value) this.kind = sel.value;
  },
  async api(path, opts = {}) {
    const res = await fetch(path, { credentials: "include", ...opts });
    return res.json().catch(() => ({}));
  },
  setMsg(t) {
    const m = document.getElementById("comp-msg");
    if (m) m.textContent = t || "";
  },
  kindPath() {
    return "/api/" + this.kind;
  },
  async refreshList() {
    this.syncKindFromDom();
    this.setMsg("");
    const data = await this.api(this.kindPath());
    if (!data.ok) {
      this.setMsg(data.error || "加载失败");
      return;
    }
    const ul = document.getElementById("comp-list");
    ul.innerHTML = "";
    (data.items || []).forEach((name) => {
      const li = document.createElement("li");
      li.textContent = name;
      li.dataset.name = name;
      li.onclick = () => this.open(name);
      ul.appendChild(li);
    });
  },
  /** @returns {Promise<boolean>} */
  async open(name) {
    this.syncKindFromDom();
    this.current = name;
    document.querySelectorAll("#comp-list li").forEach((li) => {
      li.classList.toggle("active", li.dataset.name === name);
    });
    const data = await this.api(this.kindPath() + "/" + encodeURIComponent(name));
    if (!data.ok) {
      this.setMsg(data.error || "读取失败");
      return false;
    }
    document.getElementById("comp-name").value = name;
    document.getElementById("comp-body").value = data.content || "";
    return true;
  },
  newDoc() {
    this.syncKindFromDom();
    this.current = "";
    document.getElementById("comp-name").value = "";
    document.getElementById("comp-body").value = "---\nname: \n---\n\n";
    document.querySelectorAll("#comp-list li").forEach((li) => li.classList.remove("active"));
    this.setMsg("新建后填写名称并保存");
  },
  async save() {
    this.syncKindFromDom();
    const name = document.getElementById("comp-name").value.trim();
    const content = document.getElementById("comp-body").value;
    if (!name) {
      this.setMsg("请填写名称");
      return;
    }
    if (this.current && this.current !== name) {
      this.setMsg("名称与当前项不一致：请先选列表项或新建");
      return;
    }
    let path = this.kindPath();
    let method = "POST";
    let body = JSON.stringify({ name, content });
    if (this.current) {
      path += "/" + encodeURIComponent(this.current);
      method = "PUT";
      body = JSON.stringify({ content });
    }
    const data = await this.api(path, {
      method,
      headers: { "Content-Type": "application/json" },
      body,
    });
    if (!data.ok) {
      this.setMsg(data.error || "保存失败");
      return;
    }
    this.current = name;
    await this.refreshList();
    const opened = await this.open(name);
    if (opened) this.setMsg("已保存");
    if (typeof window.cctChatRefreshAgentSkillOptions === "function") window.cctChatRefreshAgentSkillOptions();
  },
  async deleteDoc() {
    this.syncKindFromDom();
    if (!this.current) {
      this.setMsg("请先选择一项");
      return;
    }
    if (!confirm("确定删除 " + this.current + " ?")) return;
    const data = await this.api(this.kindPath() + "/" + encodeURIComponent(this.current), {
      method: "DELETE",
    });
    if (!data.ok) {
      this.setMsg(data.error || "删除失败");
      return;
    }
    this.newDoc();
    await this.refreshList();
    if (typeof window.cctChatRefreshAgentSkillOptions === "function") window.cctChatRefreshAgentSkillOptions();
  },
};
