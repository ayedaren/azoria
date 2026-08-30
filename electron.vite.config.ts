import path from "node:path"
import { fileURLToPath } from "node:url"
import tailwindcss from "@tailwindcss/vite"
import react from "@vitejs/plugin-react"
import { defineConfig, externalizeDepsPlugin } from "electron-vite"

const projectRoot = path.dirname(fileURLToPath(import.meta.url))

export default defineConfig({
  main: {
    plugins: [externalizeDepsPlugin()],
    build: { rollupOptions: { input: path.join(projectRoot, "desktop/src/main/index.ts") } },
  },
  preload: {
    plugins: [externalizeDepsPlugin()],
    build: { rollupOptions: { input: path.join(projectRoot, "desktop/src/preload/index.ts") } },
  },
  renderer: {
    root: path.join(projectRoot, "desktop/src/renderer"),
    plugins: [react(), tailwindcss()],
    resolve: { alias: { "@": path.join(projectRoot, "desktop/src/renderer/src") } },
    build: { rollupOptions: { input: path.join(projectRoot, "desktop/src/renderer/index.html") } },
  },
})
