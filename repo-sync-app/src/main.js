/**
 * Monaco Diff Editor for cct-cn 仓库同步（IIFE，挂载 window.cctRepoSyncMonaco）
 */
import editorWorker from "monaco-editor/esm/vs/editor/editor.worker?worker";

globalThis.MonacoEnvironment = {
  getWorker(_workerId, _label) {
    return new editorWorker();
  },
};

import * as monaco from "monaco-editor";

function effectiveTheme() {
  const t = document.documentElement.getAttribute("data-theme");
  return t === "light" ? "vs" : "vs-dark";
}

/**
 * @param {HTMLElement} container
 * @param {string} original
 * @param {string} modified
 * @param {{ language?: string, readOnly?: boolean }} opts
 */
export function mountDiffEditor(container, original, modified, opts = {}) {
  const language = opts.language || "plaintext";
  const readOnly = opts.readOnly !== false;
  const diff = monaco.editor.createDiffEditor(container, {
    readOnly,
    renderSideBySide: true,
    automaticLayout: true,
    theme: effectiveTheme(),
    originalEditable: !readOnly,
    modifiedEditable: !readOnly,
  });
  const o = monaco.editor.createModel(original ?? "", language);
  const m = monaco.editor.createModel(modified ?? "", language);
  diff.setModel({ original: o, modified: m });

  const obs = new MutationObserver(() => {
    try {
      diff.updateOptions({ theme: effectiveTheme() });
    } catch (_) {}
  });
  obs.observe(document.documentElement, { attributes: true, attributeFilter: ["data-theme"] });

  return {
    diff,
    getMergedModifiedText() {
      return m.getValue();
    },
    setModified(text) {
      m.setValue(text ?? "");
    },
    setOriginal(text) {
      o.setValue(text ?? "");
    },
    dispose() {
      obs.disconnect();
      o.dispose();
      m.dispose();
      diff.dispose();
    },
  };
}

globalThis.cctRepoSyncMonaco = {
  mountDiffEditor,
  monaco,
};
