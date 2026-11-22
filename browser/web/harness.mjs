// The page in headless Chromium: serves it with Vite, uploads a runtime folder through the page's directory input,
// starts the game, waits for frames, and screenshots the canvas at 640x480.
//   node browser/web/harness.mjs (--folder <runtime folder> | --synthetic) [--frames N] [--timeout s] [--out shot.png]
//                                [--reference ref.png [--threshold N]] [--chrome path]
// --synthetic makes a folder of stand-in files with no game bytes, for CI: the boot fails on it, which the harness
// takes as settled. --frames waits for that many presented frames (0: until the page halts or fails). --reference
// compares the screenshot with browser/tools/compare_frame.py. The browser is $CHROME, else a Playwright Chromium or
// the system's Chrome. Exit code: 0 when it got the frames (and they match), 1 otherwise, 2 for bad usage.
// Screenshots of game frames stay local: the default output is under build/, which git ignores.
import { spawn, spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { setTimeout as sleep } from "node:timers/promises";
import { fileURLToPath } from "node:url";
import { parseArgs } from "node:util";
import { createServer } from "vite";

const here = path.dirname(fileURLToPath(import.meta.url));
const repo = path.resolve(here, "../..");
const { values: o } = parseArgs({
  options: {
    folder: { type: "string" },
    synthetic: { type: "boolean", default: false },
    frames: { type: "string", default: "0" },
    timeout: { type: "string", default: "120" },
    out: { type: "string", default: path.join(repo, "build/browser-harness/canvas.png") },
    reference: { type: "string" },
    threshold: { type: "string" },
    chrome: { type: "string", default: process.env.CHROME },
    key: { type: "string", multiple: true, default: [] },
    mouse: { type: "string", multiple: true, default: [] },
    click: { type: "string", multiple: true, default: [] },
  },
});
// --key Name@N presses and releases a key once the game has presented N frames. Name is a KeyboardEvent code:
// ArrowRight, Enter, KeyA... Several are pressed in frame order.
const keyCodes = { ArrowLeft: 37, ArrowUp: 38, ArrowRight: 39, ArrowDown: 40, Enter: 13, Escape: 27, Space: 32 };
const keys = o.key
  .map(k => {
    const [code, frame] = k.split("@");
    return { code, frame: Number(frame), vk: keyCodes[code] ?? code.replace(/^Key/, "").charCodeAt(0) };
  })
  .sort((a, b) => a.frame - b.frame);
// --mouse X,Y@N moves the pointer onto the canvas at 640x480 coordinates once the game has presented N frames
// --click X,Y@N moves there the same way, then presses and releases the left button
const moves = [...o.mouse.map(m => [m, false]), ...o.click.map(m => [m, true])]
  .map(([m, click]) => {
    const [point, frame] = m.split("@");
    const [x, y] = point.split(",").map(Number);
    return { x, y, frame: Number(frame), click };
  })
  .sort((a, b) => a.frame - b.frame);
if (!o.folder === !o.synthetic) {
  console.error("usage: node browser/web/harness.mjs (--folder <runtime folder> | --synthetic) [--frames N] [--key Code@frame]... [--mouse X,Y@frame]... [--click X,Y@frame]... [--timeout s] [--out shot.png] [--reference ref.png [--threshold N]] [--chrome path]");
  process.exit(2);
}

const temp = fs.mkdtempSync(path.join(os.tmpdir(), "tmweb-harness-"));
const folder = o.folder ? path.resolve(o.folder) : synthetic(path.join(temp, "runtime"));
const wanted = Number(o.frames), deadline = Date.now() + Number(o.timeout) * 1000;

/** Files at the runtime folder's paths holding only text: enough for the page to take the folder and start. */
function synthetic(dir) {
  for (const [name, text] of [["TM_native.dll", "MZ not a game file"], ["polcore_native.dll", "MZ"], ["data/Aucti.BIN", "x"], ["settings/saved.txt", "kept"]]) {
    fs.mkdirSync(path.dirname(path.join(dir, name)), { recursive: true });
    fs.writeFileSync(path.join(dir, name), text);
  }
  return dir;
}

function findChrome() {
  if (o.chrome) return o.chrome;
  const playwright = process.platform === "win32" ? path.join(os.homedir(), "AppData/Local/ms-playwright") : path.join(os.homedir(), ".cache/ms-playwright");
  const builds = fs.existsSync(playwright) ? fs.readdirSync(playwright).filter(d => /^chromium-\d+$/.test(d)).sort().reverse() : [];
  const candidates = [
    ...builds.flatMap(d => [path.join(playwright, d, "chrome-win64/chrome.exe"), path.join(playwright, d, "chrome-linux/chrome"), path.join(playwright, d, "chrome-linux64/chrome")]),
    "/usr/bin/google-chrome",
    "/usr/bin/chromium",
    "/usr/bin/chromium-browser",
    "C:/Program Files/Google/Chrome/Application/chrome.exe",
  ];
  const found = candidates.find(c => fs.existsSync(c));
  if (!found) throw new Error("no Chromium found; pass --chrome or set $CHROME");
  return found;
}

const server = await createServer({ configFile: path.join(here, "vite.config.js"), server: { port: 0, strictPort: false }, logLevel: "error" });
await server.listen();
const url = server.resolvedUrls.local[0];

const profile = path.join(temp, "profile");
const chrome = spawn(findChrome(), [
  "--headless=new",
  "--remote-debugging-port=0",
  `--user-data-dir=${profile}`,
  "--no-first-run",
  "--no-default-browser-check",
  // WebGL2 without a GPU
  "--use-angle=swiftshader",
  "--enable-unsafe-swiftshader",
  // Ubuntu 24.04 keeps unprivileged user namespaces from the sandbox
  ...(process.platform === "linux" ? ["--no-sandbox"] : []),
  "about:blank",
], { stdio: "ignore" });

let code = 1;
try {
  code = await drive();
} catch (e) {
  console.error(`harness: ${e?.message ?? e}`);
} finally {
  chrome.kill();
  await server.close();
  // Chrome lets go of its profile a moment after it exits
  await sleep(500);
  fs.rmSync(temp, { recursive: true, force: true, maxRetries: 5 });
}
process.exit(code);

async function drive() {
  const port = await until(() => fs.readFileSync(path.join(profile, "DevToolsActivePort"), "utf8").split("\n")[0], "Chromium's debugging port");
  const targets = await until(() => fetch(`http://127.0.0.1:${port}/json`).then(r => r.json()), "Chromium's targets");
  const ws = new WebSocket(targets.find(t => t.type === "page").webSocketDebuggerUrl);
  await new Promise((resolve, reject) => ((ws.onopen = resolve), (ws.onerror = reject)));
  let id = 0;
  const pending = new Map();
  ws.onmessage = ({ data }) => {
    const m = JSON.parse(data);
    if (m.id) pending.get(m.id)?.(m);
    else if (m.method === "Runtime.exceptionThrown") console.error(`page: ${m.params.exceptionDetails.exception?.description ?? m.params.exceptionDetails.text}`);
  };
  const send = (method, params = {}) =>
    new Promise((resolve, reject) => {
      pending.set(++id, m => (m.error ? reject(new Error(`${method}: ${m.error.message}`)) : resolve(m.result)));
      ws.send(JSON.stringify({ id, method, params }));
    });
  const evaluate = async expression => (await send("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true })).result.value;
  const loaded = () => until(async () => (await evaluate(`document.getElementById("pick")?.textContent`)) || undefined, "the page");

  await send("Runtime.enable");
  await send("Emulation.setDeviceMetricsOverride", { width: 800, height: 1000, deviceScaleFactor: 1, mobile: false });
  await send("Page.navigate", { url: `${url}?scale=1` });
  await loaded();
  // A clean cache, so the run takes the uploaded folder
  await evaluate(`navigator.storage.getDirectory().then(r => r.removeEntry("runtime", { recursive: true })).catch(() => {}).then(() => true)`);
  await send("Page.reload");
  await loaded();

  // Again if the page reloaded meanwhile (Vite does once it has optimized the dependencies)
  await until(async () => {
    const { root } = await send("DOM.getDocument");
    const { nodeId } = await send("DOM.querySelector", { nodeId: root.nodeId, selector: "#folder" });
    await send("DOM.setFileInputFiles", { nodeId, files: [folder] });
    return true;
  }, "the folder input");
  await until(async () => (await evaluate(`!document.getElementById("start").disabled`)) || undefined, "the picked folder");
  await evaluate(`document.getElementById("start").click(), true`);

  const status = () => evaluate(`({ state: document.body.dataset.state, frames: Number(document.body.dataset.frames ?? 0) })`);
  for (const k of keys) {
    await until(async () => ((await status()).frames >= k.frame ? true : undefined), `frame ${k.frame} for ${k.code}`);
    await evaluate(`document.getElementById("screen").focus(), true`);
    for (const type of ["keyDown", "keyUp"]) {
      await send("Input.dispatchKeyEvent", { type, code: k.code, key: k.code, windowsVirtualKeyCode: k.vk });
      await sleep(50);
    }
    console.log(`harness: pressed ${k.code} at frame ${(await status()).frames}`);
  }
  for (const m of moves) {
    await until(async () => ((await status()).frames >= m.frame ? true : undefined), `frame ${m.frame} for the pointer`);
    const r = await evaluate(`(r => ({ x: r.x, y: r.y, width: r.width, height: r.height }))(document.getElementById("screen").getBoundingClientRect())`);
    // From the canvas's centre, in a few steps, as a hand would
    for (let step = 1; step <= 8; ++step) {
      const x = r.x + r.width * (0.5 + ((m.x / 640 - 0.5) * step) / 8);
      const y = r.y + r.height * (0.5 + ((m.y / 480 - 0.5) * step) / 8);
      await send("Input.dispatchMouseEvent", { type: "mouseMoved", x, y });
      await sleep(30);
    }
    if (m.click) {
      const x = r.x + (r.width * m.x) / 640;
      const y = r.y + (r.height * m.y) / 480;
      await send("Input.dispatchMouseEvent", { type: "mousePressed", x, y, button: "left", clickCount: 1 });
      await sleep(80);
      await send("Input.dispatchMouseEvent", { type: "mouseReleased", x, y, button: "left", clickCount: 1 });
    }
    console.log(`harness: pointer at ${m.x},${m.y}${m.click ? ", clicked" : ""} at frame ${(await status()).frames}`);
  }
  const settled = await until(async () => {
    const s = await status();
    return (wanted > 0 ? s.frames >= wanted : false) || ["halted", "failed", "need-files"].includes(s.state) ? s : undefined;
  }, wanted > 0 ? `${wanted} frames` : "the page to settle");
  console.log(`harness: ${settled.state ?? "running"}, ${settled.frames} frames`);
  console.log(await evaluate(`document.getElementById("log").textContent`));

  const box = await evaluate(`(r => ({ x: r.x, y: r.y, width: r.width, height: r.height }))(document.getElementById("screen").getBoundingClientRect())`);
  const shot = await send("Page.captureScreenshot", { format: "png", clip: { ...box, scale: 1 } });
  const png = Buffer.from(shot.data, "base64");
  fs.mkdirSync(path.dirname(path.resolve(o.out)), { recursive: true });
  fs.writeFileSync(o.out, png);
  const [w, h] = [png.readUInt32BE(16), png.readUInt32BE(20)];
  console.log(`harness: canvas ${w}x${h} in ${o.out}`);
  ws.close();
  if (w !== 640 || h !== 480) return 1;
  if (wanted > 0 && settled.frames < wanted) return 1;
  if (!o.reference) return 0;
  const python = process.platform === "win32" ? "python" : "python3";
  const compare = [path.join(repo, "browser/tools/compare_frame.py"), o.out, o.reference, ...(o.threshold ? ["--threshold", o.threshold] : [])];
  return spawnSync(python, compare, { stdio: "inherit" }).status === 0 ? 0 : 1;
}

/** Polls f until it gives a value, up to the deadline. */
async function until(f, what) {
  while (Date.now() < deadline) {
    try {
      const v = await f();
      if (v !== undefined && v !== null && v !== false) return v;
    } catch {
      // not there yet
    }
    await sleep(250);
  }
  throw new Error(`timed out waiting for ${what}`);
}
