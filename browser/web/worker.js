// The module, off the page's thread. The page sends start (the picked files, or none to use the cache, and the
// canvas as an OffscreenCanvas) and then input; the worker sends log lines, sound chunks and state changes.
import retailList from "../../retail-files.txt?raw";
import createTmweb from "tmweb/tmweb-web.mjs";
import wasmUrl from "tmweb/tmweb-web.wasm?url";
import { loadCache, retailPaths, runtimeFiles, saveCache } from "./files.js";
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
    // Move comes as 0 to 1 across the picture; the stand-ins take back-buffer pixels
    const [w, h] = [canvas?.width || 640, canvas?.height || 480];
    const pixels = m.kind === InputKind.Move;
    tmweb.input(m.kind, pixels ? Math.min(w - 1, Math.floor(m.a * w)) : m.a, pixels ? Math.min(h - 1, Math.floor(m.b * h)) : m.b);
  }
};

async function start({ files, canvas: c }) {
  canvas = c;
  const { found, missing } = runtimeFiles(files ?? await loadCache(), retailPaths(retailList));
  if (!found.length) return state("need-files");
  if (missing.length) log(`tmweb: ${missing.length} listed files are not in the folder, as ${missing.slice(0, 3).join(", ")}`);
  const read = await Promise.all(found.map(async ({ path, file }) => ({ path, bytes: new Uint8Array(await file.arrayBuffer()) })));
  if (files) await saveCache(read);

  tmweb = await loadTmweb(createTmweb, log, { canvas, locateFile: () => wasmUrl });
  for (const { path, bytes } of read) {
    const full = `${ROOT}/${path}`;
    tmweb.FS.mkdirTree(full.slice(0, full.lastIndexOf("/")));
    tmweb.FS.writeFile(full, bytes);
  }
  const mb = read.reduce((n, f) => n + f.bytes.length, 0) / 2 ** 20;
  log(`tmweb: ${read.length} game files, ${mb.toFixed(1)} MB${files ? ", cached" : " from the cache"}`);
  if (tmweb.boot(ROOT) !== 0) return state("failed");
  state("running");
  slice();
}

// Each slice runs for about SLICE_MS, then the worker takes its messages (input, socket events) and the canvas
// presents. A MessageChannel yields without setTimeout's 4 ms clamp.
const SLICE_MS = 8;
let budget = 100_000;
const channel = new MessageChannel();
channel.port1.onmessage = () => slice();

function slice() {
  const t = performance.now();
  const s = tmweb.run(budget);
  const ms = performance.now() - t;
  if (s === RunState.Budget) budget = Math.round(Math.min(50_000_000, Math.max(10_000, budget * SLICE_MS / Math.max(ms, 0.5))));
  const sound = tmweb.takeSound();
  if (sound.length) post({ type: "sound", samples: sound }, [sound.buffer]);
  if (s === RunState.Halted) return state("halted");
  if (s === RunState.Idle) setTimeout(slice, 4);
  else channel.port2.postMessage(0);
}
