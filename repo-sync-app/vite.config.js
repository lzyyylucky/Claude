import { defineConfig } from "vite";
import { resolve } from "path";

export default defineConfig({
  base: "/repo-sync/",
  root: resolve(__dirname),
  publicDir: false,
  build: {
    outDir: resolve(__dirname, "../ui/repo-sync"),
    emptyOutDir: true,
    lib: {
      entry: resolve(__dirname, "src/main.js"),
      name: "CctRepoSyncMonaco",
      formats: ["iife"],
      fileName: () => "repo-sync-monaco.js",
    },
    rollupOptions: {
      output: {
        inlineDynamicImports: true,
      },
    },
  },
});
