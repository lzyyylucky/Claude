import { defineConfig } from "vite";
import { resolve } from "path";

export default defineConfig({
  root: resolve(__dirname),
  publicDir: false,
  build: {
    outDir: resolve(__dirname, "../ui/ide"),
    emptyOutDir: true,
    lib: {
      entry: resolve(__dirname, "src/main.js"),
      name: "CctIde",
      formats: ["iife"],
      fileName: () => "ide.js",
    },
    rollupOptions: {
      output: {
        inlineDynamicImports: true,
      },
    },
  },
});
