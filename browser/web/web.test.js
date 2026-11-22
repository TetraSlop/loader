// node --test, from browser/web. The runner checks need the Emscripten build (build/browser-wasm/tmweb-web.mjs, or
// $TMWEB_MODULE) and skip without it; with $TM_RUNTIME, a runtime folder, it also boots the user's files.
import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { test } from "node:test";
import { fileURLToPath } from "node:url";
import { retailPaths, runtimeFiles } from "./files.js";
import { DIK, onPicture } from "./input.js";
import { SoundQueue } from "./speakers.js";
import { SocketEvent, webSocketTransport } from "./transport.js";

const here = path.dirname(fileURLToPath(import.meta.url));

test("the runtime files are the ones retail-files.txt lists, matched without case, and the saved ones", () => {
  const paths = retailPaths(fs.readFileSync(path.join(here, "../../retail-files.txt"), "utf8"));
  assert.ok(paths.includes("TM_native.dll") && paths.includes("data/utf/env.dat"));
  const entries = [
    { path: "tm_native.DLL", file: 1 },
    { path: "Data\\Aucti.bin", file: 2 },
    { path: "other.txt", file: 3 },
    { path: "settings\\tm.cfg", file: 4 },
    { path: "logs/tm.log", file: 5 },
  ];
  const { found, missing } = runtimeFiles(entries, paths);
  assert.deepEqual(found.map(f => f.path), ["TM_native.dll", "data/Aucti.BIN", "settings/tm.cfg"]);
  assert.equal(missing.length, paths.length - 2);
});

test("keys go by where they sit, the pointer by where it falls on the picture", () => {
  assert.deepEqual([DIK.KeyE, DIK.Digit0, DIK.Enter, DIK.F10, DIK.ArrowLeft], [0x12, 0x0b, 0x1c, 0x44, 0xcb]);
  // A 4:3 picture in a 1000x600 box sits in an 800x600 band in the middle
  assert.deepEqual(onPicture({ left: 0, top: 0, width: 1000, height: 600 }, 4 / 3, 500, 300), [0.5, 0.5]);
  assert.deepEqual(onPicture({ left: 0, top: 0, width: 1000, height: 600 }, 4 / 3, 50, 0), [0, 0]);
});

test("the speakers' queue plays once it holds its target", () => {
  const q = new SoundQueue(4, 16);
  const [l, r] = [new Float32Array(2), new Float32Array(2)];
  q.push(new Float32Array([1, -1, 1, -1]));
  q.pull(l, r);
  assert.equal(l[0], 0, "short of the target it stays silent");
  q.push(new Float32Array([1, -1, 1, -1, 1, -1]));
  q.pull(l, r);
  assert.ok(l[1] > 0 && r[1] < 0);
});

test("a connect with no route fails, after the call returns", async () => {
  const t = webSocketTransport();
  const events = [];
  t.listen((id, kind) => events.push([id, kind]));
  t.connect(7, "127.0.0.1", 54230);
  assert.deepEqual(events, []);
  await Promise.resolve();
  assert.deepEqual(events, [[7, SocketEvent.Failed]]);
});

const module = process.env.TMWEB_MODULE ?? path.join(here, "../../build/browser-wasm/tmweb-web.mjs");
const runner = (...args) => execFileSync(process.execPath, [path.join(here, "run.mjs"), ...args], { encoding: "utf8", env: { ...process.env, TMWEB_MODULE: module } });

test("the Node runner boots a folder and stops", { skip: !fs.existsSync(module) && "no Emscripten build" }, () => {
  const empty = fs.mkdtempSync(path.join(os.tmpdir(), "tmweb-"));
  try {
    const out = runner(empty, "--budget", "1000");
    assert.match(out, /tmweb: runtime \/game/);
    assert.match(out, /tmweb: stopped \((halted|budget|idle)\)/);
  } finally {
    fs.rmSync(empty, { recursive: true });
  }
});

test("the Node runner boots the user's runtime folder", { skip: (!fs.existsSync(module) || !process.env.TM_RUNTIME) && "no build or no $TM_RUNTIME" }, () => {
  assert.match(runner(process.env.TM_RUNTIME, "--budget", "1000000"), /TM_native\.dll [1-9]\d* bytes/);
});
