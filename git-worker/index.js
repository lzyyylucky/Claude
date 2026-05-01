/**
 * Loopback-only Git worker for cct-cn. Bound to 127.0.0.1.
 * Usage: node index.js [--port N]
 */
import express from "express";
import fs from "fs";
import path from "path";
import { simpleGit } from "simple-git";

const MAX_READ = 2 * 1024 * 1024;

function parsePort(argv) {
  const i = argv.indexOf("--port");
  if (i >= 0 && argv[i + 1]) {
    const p = parseInt(argv[i + 1], 10);
    if (Number.isFinite(p) && p > 0 && p < 65536) return p;
  }
  const env = process.env.CCT_GIT_WORKER_PORT;
  if (env) {
    const p = parseInt(env, 10);
    if (Number.isFinite(p) && p > 0 && p < 65536) return p;
  }
  return 47821;
}

/** Resolve rel inside workspace; return absolute normalized path or throw */
function resolveWorkspacePath(workspaceRoot, relPath) {
  const root = path.resolve(workspaceRoot);
  const rel = String(relPath || "").replace(/\\/g, "/").replace(/^\/+/, "");
  const joined = path.resolve(root, rel);
  const rootLow = root.toLowerCase();
  const joinedLow = joined.toLowerCase();
  if (!joinedLow.startsWith(rootLow + path.sep) && joinedLow !== rootLow) {
    throw new Error("路径越出工作区");
  }
  return joined;
}

function gitOpen(workspaceRoot) {
  return simpleGit(path.resolve(workspaceRoot));
}

async function listConflictPaths(git) {
  const out = await git.raw(["diff", "--name-only", "--diff-filter=U"]);
  return out
    .split(/[\r\n]+/)
    .map((s) => s.trim())
    .filter(Boolean);
}

async function tryRawShow(git, stage, relPosix) {
  const spec = `${stage}:${relPosix.replace(/\\/g, "/")}`;
  try {
    return await git.raw(["show", spec]);
  } catch {
    return "";
  }
}

function readMarkedFile(absPath) {
  const st = fs.statSync(absPath);
  if (!st.isFile()) throw new Error("不是文件");
  if (st.size > MAX_READ) throw new Error("文件过大（>2MB）");
  return fs.readFileSync(absPath, "utf8");
}

const app = express();
app.use(express.json({ limit: "6mb" }));

app.use((req, res, next) => {
  res.setHeader("X-CCT-Git-Worker", "1");
  next();
});

app.post("/git/status", async (req, res) => {
  try {
    const { workspaceRoot } = req.body || {};
    if (!workspaceRoot) return res.status(400).json({ ok: false, error: "需要 workspaceRoot" });
    const root = path.resolve(workspaceRoot);
    if (!fs.existsSync(path.join(root, ".git"))) {
      return res.json({
        ok: true,
        isRepo: false,
        message: "当前工作区根目录不是 Git 仓库（缺少 .git）",
      });
    }
    const git = gitOpen(root);
    const st = await git.status();
    const branch = st.current || "";
    const tracking = st.tracking || "";
    let ahead = 0;
    let behind = 0;
    try {
      ahead = parseInt((await git.raw(["rev-list", "--count", "@{u}..HEAD"])).trim(), 10) || 0;
      behind = parseInt((await git.raw(["rev-list", "--count", "HEAD..@{u}"])).trim(), 10) || 0;
    } catch {
      /* no upstream */
    }
    const files = [];
    for (const raw of st.files || []) {
      files.push({
        path: raw.path.replace(/\\/g, "/"),
        index: raw.index,
        working_dir: raw.working_dir,
      });
    }
    return res.json({
      ok: true,
      isRepo: true,
      branch,
      tracking,
      ahead,
      behind,
      clean: st.isClean(),
      files,
    });
  } catch (e) {
    return res.status(500).json({ ok: false, error: String(e && e.message ? e.message : e) });
  }
});

app.post("/git/diff-sides", async (req, res) => {
  try {
    const { workspaceRoot, path: relPath } = req.body || {};
    if (!workspaceRoot || !relPath) {
      return res.status(400).json({ ok: false, error: "需要 workspaceRoot 与 path" });
    }
    const git = gitOpen(workspaceRoot);
    const posix = String(relPath).replace(/\\/g, "/");
    const ours = await tryRawShow(git, ":2", posix);
    const theirs = await tryRawShow(git, ":3", posix);
    if (!ours && !theirs) {
      try {
        const headVer = await git.raw(["show", `HEAD:${posix}`]);
        return res.json({ ok: true, original: headVer || "", modified: readMarkedFile(resolveWorkspacePath(workspaceRoot, relPath)) });
      } catch {
        return res.json({ ok: true, original: "", modified: readMarkedFile(resolveWorkspacePath(workspaceRoot, relPath)) });
      }
    }
    return res.json({ ok: true, original: ours, modified: theirs });
  } catch (e) {
    return res.status(500).json({ ok: false, error: String(e && e.message ? e.message : e) });
  }
});

app.post("/git/stage", async (req, res) => {
  try {
    const { workspaceRoot, paths } = req.body || {};
    if (!workspaceRoot || !Array.isArray(paths) || paths.length === 0) {
      return res.status(400).json({ ok: false, error: "需要 workspaceRoot 与 paths 数组" });
    }
    const git = gitOpen(workspaceRoot);
    const norm = paths.map((p) => String(p).replace(/\\/g, "/"));
    await git.add(norm);
    return res.json({ ok: true });
  } catch (e) {
    return res.status(500).json({ ok: false, error: String(e && e.message ? e.message : e) });
  }
});

app.post("/git/unstage", async (req, res) => {
  try {
    const { workspaceRoot, paths } = req.body || {};
    if (!workspaceRoot || !Array.isArray(paths)) {
      return res.status(400).json({ ok: false, error: "需要 workspaceRoot 与 paths" });
    }
    const git = gitOpen(workspaceRoot);
    const norm = paths.map((p) => String(p).replace(/\\/g, "/"));
    await git.reset(["HEAD", "--", ...norm]);
    return res.json({ ok: true });
  } catch (e) {
    return res.status(500).json({ ok: false, error: String(e && e.message ? e.message : e) });
  }
});

app.post("/git/commit", async (req, res) => {
  try {
    const { workspaceRoot, message } = req.body || {};
    if (!workspaceRoot) return res.status(400).json({ ok: false, error: "需要 workspaceRoot" });
    const msg = message != null ? String(message) : "";
    if (!msg.trim()) return res.status(400).json({ ok: false, error: "需要非空提交说明 message" });
    const git = gitOpen(workspaceRoot);
    await git.commit(msg.trim());
    return res.json({ ok: true });
  } catch (e) {
    return res.status(500).json({ ok: false, error: String(e && e.message ? e.message : e) });
  }
});

app.post("/git/push", async (req, res) => {
  try {
    const { workspaceRoot, remote, branch } = req.body || {};
    if (!workspaceRoot) return res.status(400).json({ ok: false, error: "需要 workspaceRoot" });
    const git = gitOpen(workspaceRoot);
    const r = remote && String(remote).trim() ? String(remote).trim() : "origin";
    const st = await git.status();
    const b = branch && String(branch).trim() ? String(branch).trim() : st.current;
    await git.push(r, b);
    return res.json({ ok: true });
  } catch (e) {
    return res.status(500).json({ ok: false, error: String(e && e.message ? e.message : e) });
  }
});

app.post("/git/pull", async (req, res) => {
  const { workspaceRoot, remote, branch } = req.body || {};
  try {
    if (!workspaceRoot) return res.status(400).json({ ok: false, error: "需要 workspaceRoot" });
    const git = gitOpen(workspaceRoot);
    const r = remote && String(remote).trim() ? String(remote).trim() : "origin";
    const st = await git.status();
    const b = branch && String(branch).trim() ? String(branch).trim() : st.current;
    await git.pull(r, b);
    return res.json({ ok: true, conflict: false });
  } catch (e) {
    try {
      if (!workspaceRoot) throw e;
      const git = gitOpen(workspaceRoot);
      const conflicts = await listConflictPaths(git);
      if (conflicts.length > 0) {
        return res.status(409).json({
          ok: false,
          conflict: true,
          conflicts,
          error: String(e && e.message ? e.message : e),
        });
      }
    } catch (_) {
      /* ignore */
    }
    return res.status(500).json({ ok: false, error: String(e && e.message ? e.message : e) });
  }
});

app.post("/git/conflicts-detail", async (req, res) => {
  try {
    const { workspaceRoot } = req.body || {};
    if (!workspaceRoot) return res.status(400).json({ ok: false, error: "需要 workspaceRoot" });
    const git = gitOpen(workspaceRoot);
    const paths = await listConflictPaths(git);
    const items = [];
    for (const rel of paths) {
      const posix = rel.replace(/\\/g, "/");
      let markedContent = "";
      try {
        markedContent = readMarkedFile(resolveWorkspacePath(workspaceRoot, posix));
      } catch {
        markedContent = "";
      }
      const ours = await tryRawShow(git, ":2", posix);
      const theirs = await tryRawShow(git, ":3", posix);
      items.push({ path: posix, markedContent, ours, theirs });
    }
    return res.json({ ok: true, files: items });
  } catch (e) {
    return res.status(500).json({ ok: false, error: String(e && e.message ? e.message : e) });
  }
});

app.post("/git/write-file", async (req, res) => {
  try {
    const { workspaceRoot, path: relPath, content } = req.body || {};
    if (!workspaceRoot || relPath == null) {
      return res.status(400).json({ ok: false, error: "需要 workspaceRoot 与 path" });
    }
    const body = content != null ? String(content) : "";
    if (body.length > MAX_READ) return res.status(413).json({ ok: false, error: "内容过大" });
    const abs = resolveWorkspacePath(workspaceRoot, relPath);
    fs.mkdirSync(path.dirname(abs), { recursive: true });
    fs.writeFileSync(abs, body, "utf8");
    return res.json({ ok: true });
  } catch (e) {
    return res.status(500).json({ ok: false, error: String(e && e.message ? e.message : e) });
  }
});

app.post("/git/add", async (req, res) => {
  try {
    const { workspaceRoot, paths } = req.body || {};
    if (!workspaceRoot || !Array.isArray(paths)) {
      return res.status(400).json({ ok: false, error: "需要 workspaceRoot 与 paths" });
    }
    const git = gitOpen(workspaceRoot);
    const norm = paths.map((p) => String(p).replace(/\\/g, "/"));
    await git.add(norm);
    return res.json({ ok: true });
  } catch (e) {
    return res.status(500).json({ ok: false, error: String(e && e.message ? e.message : e) });
  }
});

app.post("/git/commit-merge", async (req, res) => {
  try {
    const { workspaceRoot, message } = req.body || {};
    if (!workspaceRoot) return res.status(400).json({ ok: false, error: "需要 workspaceRoot" });
    const msg =
      message != null && String(message).trim()
        ? String(message).trim()
        : "Merge conflict resolution";
    const git = gitOpen(workspaceRoot);
    await git.commit(msg);
    return res.json({ ok: true });
  } catch (e) {
    return res.status(500).json({ ok: false, error: String(e && e.message ? e.message : e) });
  }
});

app.post("/git/merge-abort", async (req, res) => {
  try {
    const { workspaceRoot } = req.body || {};
    if (!workspaceRoot) return res.status(400).json({ ok: false, error: "需要 workspaceRoot" });
    const git = gitOpen(workspaceRoot);
    await git.merge(["--abort"]);
    return res.json({ ok: true });
  } catch (e) {
    return res.status(500).json({ ok: false, error: String(e && e.message ? e.message : e) });
  }
});

const port = parsePort(process.argv);
app.listen(port, "127.0.0.1", () => {
  console.error(`[cct-git-worker] listening http://127.0.0.1:${port}`);
});
