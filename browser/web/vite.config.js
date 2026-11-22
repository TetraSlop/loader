// The page's dev server and build. The module comes from the Emscripten build, build/browser-wasm by default or
// $TMWEB_BUILD.
import path from "node:path";
import { fileURLToPath } from "node:url";
import { defineConfig } from "vite";

const here = path.dirname(fileURLToPath(import.meta.url));
const repo = path.resolve(here, "../..");

export default defineConfig({
  root: here,
  resolve: { alias: { tmweb: path.resolve(process.env.TMWEB_BUILD ?? path.join(repo, "build/browser-wasm")) } },
  // retail-files.txt sits at the repository's root, the module in its build folder
  server: { fs: { allow: [repo] } },
  worker: { format: "es" },
  build: { target: "esnext", outDir: path.join(repo, "build/browser-page"), emptyOutDir: true },
});
