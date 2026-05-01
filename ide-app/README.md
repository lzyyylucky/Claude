# Web IDE 前端（CodeMirror 6 + Vite）

构建产物输出到 `../ui/ide/ide.js`，由 `app.html` 以 ES module 加载。

```bash
npm install
npm run build
```

修改 IDE 源码后需重新执行 `npm run build`，并同步复制 `ui/` 到可执行文件目录（或生成 Visual Studio 中的 `ui-sync` / 重新生成 `cct-cn`）。
