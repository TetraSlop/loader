// The module, off the page's thread. The page sends start (the picked files, or none to use the cache, and the
// canvas as an OffscreenCanvas) and then input; the worker sends log lines, sound chunks, frame counts and state
// changes.
import retailList from "../../retail-files.txt?raw";
import createTmweb from "tmweb/tmweb-web.mjs";
import wasmUrl from "tmweb/tmweb-web.wasm?url";
import { loadCache, retailPaths, runtimeFiles, saveCache, saveFile } from "./files.js";
import { InputKind } from "./input.js";
import { loadTmweb, ROOT, RunState } from "./module.js";

const post = (m, transfer = []) => postMessage(m, { transfer });
const log = line => post({ type: "log", line });
const state = s => post({ type: "state", state: s });

/** @type {Awaited<ReturnType<typeof loadTmweb>> | undefined} */
let tmweb;
let canvas;

onmessage = ({ data: m }) => {
  if (m.type === "start") {
    start(m).catch(e => {
      log(`tmweb: ${e?.message ?? e}`);
      state("failed");
    });
  } else if (m.type === "input" && tmweb) {
    input(m);
  }
};

// A button let go before the game saw it pressed would be lost: the game polls the mouse once a frame
// (GetDeviceState). So a release waits until a frame has been presented since its press, or LATCH_MS have passed.
const LATCH_MS = 100;
const pressed = new Map(); // button -> { frames, at } of its press
const held = []; // releases waiting
const release = () => {
  for (let i = held.length - 1; i >= 0; i--) {
    const p = pressed.get(held[i]);
    if (!p || tmweb.frames() > p.frames || performance.now() - p.at > LATCH_MS) {
      pressed.delete(held[i]);
      tmweb.input(InputKind.Button, held.splice(i, 1)[0], 0);
    }
  }
};

function input({ kind, a, b }) {
  if (kind === InputKind.Move) {
    // Move comes as 0 to 1 across the picture; the stand-ins take client pixels
    const [w, h] = [canvas?.width || 640, canvas?.height || 480];
    return tmweb.input(kind, Math.min(w - 1, Math.floor(a * w)), Math.min(h - 1, Math.floor(b * h)));
  }
  if (kind === InputKind.Button && b) pressed.set(a, { frames: tmweb.frames(), at: performance.now() });
  if (kind === InputKind.Button && !b) return held.push(a), release();
  tmweb.input(kind, a, b);
}

// What the game writes goes on to the cache, one file at a time and in order
let saving = Promise.resolve();
const written = path => {
  const bytes = tmweb.FS.readFile(`${ROOT}/${path}`);
  saving = saving.then(() => saveFile(path, bytes)).catch(e => log(`tmweb: could not keep ${path}: ${e?.message ?? e}`));
};

async function start({ files, canvas: c }) {
  canvas = c;
  const { found, missing } = runtimeFiles(files ?? await loadCache(), retailPaths(retailList));
  if (!found.length) return state("need-files");
  if (missing.length) log(`tmweb: ${missing.length} listed files are not in the folder, as ${missing.slice(0, 3).join(", ")}`);
  const read = await Promise.all(found.map(async ({ path, file }) => ({ path, bytes: new Uint8Array(await file.arrayBuffer()) })));
  if (files) await saveCache(read);

  tmweb = await loadTmweb(createTmweb, log, { canvas, locateFile: () => wasmUrl, onFileWritten: written });
  for (const { path, bytes } of read) {
    const full = `${ROOT}/${path}`;
    tmweb.FS.mkdirTree(full.slice(0, full.lastIndexOf("/")));
    tmweb.FS.writeFile(full, bytes);
  }
  const mb = read.reduce((n, f) => n + f.bytes.length, 0) / 2 ** 20;
  log(`tmweb: ${read.length} game files, ${mb.toFixed(1)} MB${files ? ", cached" : " from the cache"}`);
  if (tmweb.boot(ROOT) !== 0) return state("failed");
  state("running");
  tick();
}

// Pacing. Each task runs slices for up to TASK_MS, or until the game presents a frame; then the worker takes its
// messages (input, socket events) and the canvas shows the frame. After a frame the next task waits for the
// display's next refresh (requestAnimationFrame), so the game draws at most once per refresh; until the game
// presents, tasks follow each other at once. A MessageChannel yields without setTimeout's 4 ms clamp.
const TASK_MS = 12;
const SLICE_MS = 4;
let budget = 100_000;
let frames = 0;
const channel = new MessageChannel();
channel.port1.onmessage = () => tick();
const nextFrame = self.requestAnimationFrame?.bind(self) ?? (f => setTimeout(f, 16));

function tick() {
  const start = performance.now();
  let s;
  do {
    const t = performance.now();
    s = tmweb.run(budget);
    const ms = performance.now() - t;
    if (s === RunState.Budget) budget = Math.round(Math.min(50_000_000, Math.max(10_000, budget * SLICE_MS / Math.max(ms, 0.25))));
  } while (s === RunState.Budget && performance.now() - start < TASK_MS);
  const sound = tmweb.takeSound();
  if (sound.length) post({ type: "sound", samples: sound }, [sound.buffer]);
  if (tmweb.frames() !== frames) post({ type: "frames", frames: frames = tmweb.frames() });
  release();
  if (s === RunState.Halted) return state("halted");
  if (s === RunState.Presented) nextFrame(() => tick());
  else if (s === RunState.Idle) setTimeout(tick, 4);
  else channel.port2.postMessage(0);
}
