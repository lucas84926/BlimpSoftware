import { defineConfig } from "vite";
import preact from "@preact/preset-vite";

// The client is a single-page app. In dev, Vite serves it with HMR and proxies
// the two backend endpoints to the Python bridge (run it on :8001). In prod the
// same paths are proxied by the Deno front door (server/main.ts), so the
// client always talks to same-origin /ws and /video — identical whether it's
// running under Vite dev, `deno serve`, or a packaged `deno desktop` binary.
const UPSTREAM = process.env.RS_UPSTREAM ?? "127.0.0.1:8001";

export default defineConfig({
  plugins: [preact()],
  build: {
    target: "es2022",
    outDir: "dist",
    assetsInlineLimit: 4096,
  },
  server: {
    host: true, // expose on the LAN so an iPad can hit the dev server too
    port: 5173,
    proxy: {
      "/ws": { target: `ws://${UPSTREAM}`, ws: true, changeOrigin: true },
      "/video": { target: `http://${UPSTREAM}`, changeOrigin: true },
    },
  },
});
