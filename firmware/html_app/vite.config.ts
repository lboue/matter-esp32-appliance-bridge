import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// Build output goes to ../html_compiled_app as fixed-name app.js/app.css, which
// main/CMakeLists.txt embeds in the firmware image. `npm run dev` proxies /api
// (including the /api/events WebSocket) to a board on the LAN.
const target = process.env.VITE_API_TARGET ?? "http://appliance-bridge.local";

export default defineConfig({
  plugins: [react()],
  server: {
    proxy: {
      "/api": { target, ws: true },
    },
  },
  build: {
    rollupOptions: {
      output: {
        dir: "../html_compiled_app",
        entryFileNames: "app.js",
        assetFileNames: "app.css",
        chunkFileNames: "chunk.js",
        manualChunks: undefined,
      },
    },
  },
});
