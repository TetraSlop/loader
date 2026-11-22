// The Node runner: boots the module on a runtime folder and runs it to a stop condition, printing its log.
//   node browser/web/run.mjs <runtime folder> [--module tmweb-web.mjs] [--budget N] [--until module!name] [--trace]
//                            [--slice N] [--idle-ms N]
// --budget stops after N instructions (default: none). --until halts at the first call to that stand-in, as
// "kernel32.dll!CreateFileA" or "d3d8.dll!Direct3DCreate8". --trace logs every stand-in call. --slice is the
// instructions per run call; between calls socket events arrive. --idle-ms stops once the guest has been idle that
// long (every thread waiting on the host). The module defaults to $TMWEB_MODULE, then build/browser-wasm.
// Exit code: 0 once it stopped, 1 when the module would not boot, 2 for bad usage.
import path from "node:path";
import { setTimeout as sleep } from "node:timers/promises";
import { fileURLToPath, pathToFileURL } from "node:url";
import { parseArgs } from "node:util";
import { loadTmweb, ROOT, RunState } from "./module.js";

const { values: o, positionals } = parseArgs({
  allowPositionals: true,
  options: {
    module: { type: "string", default: process.env.TMWEB_MODULE ?? fileURLToPath(new URL("../../build/browser-wasm/tmweb-web.mjs", import.meta.url)) },
    budget: { type: "string", default: "Infinity" },
    until: { type: "string", default: "" },
    trace: { type: "boolean", default: false },
    slice: { type: "string", default: "1000000" },
    "idle-ms": { type: "string", default: "10000" },
  },
});
if (positionals.length !== 1) {
  console.error("usage: node browser/web/run.mjs <runtime folder> [--module tmweb-web.mjs] [--budget N] [--until module!name] [--trace] [--slice N] [--idle-ms N]");
  process.exit(2);
}

const tmweb = await loadTmweb((await import(pathToFileURL(path.resolve(o.module)).href)).default, line => console.log(line));
tmweb.FS.mkdir(ROOT);
tmweb.FS.mount(tmweb.NODEFS, { root: path.resolve(positionals[0]) }, ROOT);
if (tmweb.boot(ROOT, { trace: o.trace, until: o.until }) !== 0) process.exit(1);

const budget = Number(o.budget), slice = Number(o.slice), idleMs = Number(o["idle-ms"]);
let state = RunState.Budget, idleSince;
while (tmweb.instructions() < budget) {
  state = tmweb.run(Math.min(slice, budget - tmweb.instructions()));
  if (state === RunState.Halted) break;
  if (state !== RunState.Idle) {
    idleSince = undefined;
    continue;
  }
  idleSince ??= performance.now();
  if (performance.now() - idleSince > idleMs) break;
  // Socket events and timeouts arrive while the runner waits
  await sleep(1);
}
const why = state === RunState.Halted ? "halted" : state === RunState.Idle ? "idle" : "budget";
console.log(`tmweb: stopped (${why}) after ${tmweb.instructions()} instructions`);
// Open sockets would keep Node alive
process.exit(0);
