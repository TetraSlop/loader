// The Node runner: boots the module on a runtime folder and runs it to a stop condition, printing its log.
//   node browser/web/run.mjs <runtime folder> [--module tmweb-web.mjs] [--budget N] [--until module!name] [--trace]
//                            [--slice N] [--idle-ms N] [--viewer polcore|stub] [--probe] [--sign-in URL [--as NAME]]
// --budget stops after N instructions (default: none). --until halts at the first call to that stand-in, as
// "kernel32.dll!CreateFileA" or "d3d8.dll!Direct3DCreate8". --trace logs every stand-in call. --slice is the
// instructions per run call; between calls socket events arrive. --idle-ms stops once the guest has been idle that
// long (every thread waiting on the host). The module defaults to $TMWEB_MODULE, then build/browser-wasm.
// --viewer polcore runs the user's polcore_native.dll instead of the stand-in Viewer; --probe then logs in to Game IRC
// and loads the profile instead of booting TM.dll. --sign-in signs in to a tmserver gateway with its loopback dev
// identity (tmserver docs/LOADER_AUTH_SPEC.md, "Local development without Supabase") as NAME (default tmweb): the play
// session's account logs in, and the game port (51241) and the profile port (51220) go to its game and profile
// streams, opened as a page on http://localhost:5173 would open them.
// Exit code: 0 once it stopped, 1 when the module would not boot, 2 for bad usage.
import path from "node:path";
import { setTimeout as sleep } from "node:timers/promises";
import { fileURLToPath, pathToFileURL } from "node:url";
import { parseArgs } from "node:util";
import { loadTmweb, ROOT, RunState } from "./module.js";
import { webSocketTransport } from "./transport.js";

const { values: o, positionals } = parseArgs({
  allowPositionals: true,
  options: {
    module: { type: "string", default: process.env.TMWEB_MODULE ?? fileURLToPath(new URL("../../build/browser-wasm/tmweb-web.mjs", import.meta.url)) },
    budget: { type: "string", default: "Infinity" },
    until: { type: "string", default: "" },
    trace: { type: "boolean", default: false },
    slice: { type: "string", default: "1000000" },
    "idle-ms": { type: "string", default: "10000" },
    viewer: { type: "string", default: "stub" },
    probe: { type: "boolean", default: false },
    "sign-in": { type: "string" },
    as: { type: "string", default: "tmweb" },
  },
});
if (positionals.length !== 1) {
  console.error("usage: node browser/web/run.mjs <runtime folder> [--module tmweb-web.mjs] [--budget N] [--until module!name] [--trace] [--slice N] [--idle-ms N]");
  process.exit(2);
}

const log = line => console.log(line);
// The origin tmserver's examples/auth-dev.json lists for a dev page
const PAGE = "http://localhost:5173";

// POST /v1/play/tickets with the dev identity's bearer name, then redeem the ticket for a play session
async function signIn(gateway, name) {
  const post = async (route, body, headers = {}) => {
    const response = await fetch(new URL(route, gateway), { method: "POST", headers: { "Content-Type": "application/json", Origin: PAGE, ...headers }, body: JSON.stringify(body) });
    if (!response.ok) throw new Error(`${route}: ${response.status} ${await response.text()}`);
    return response.json();
  };
  const bearer = { Authorization: `Bearer ${name}` };
  const redeem = async () => post("/v1/play/sessions", { ticket: (await post("/v1/play/tickets", { protocol: 1 }, bearer)).ticket });
  try {
    return await redeem();
  } catch (error) {
    // One play session per account: a dev run ends the name's previous one, as "Disconnect other session" would
    if (!String(error.message).includes(" 409 ")) throw error;
    await fetch(new URL("/v1/play/account-session", gateway), { method: "DELETE", headers: { Origin: PAGE, ...bearer } });
    return redeem();
  }
}

const play = o["sign-in"] && (await signIn(o["sign-in"], o.as));
if (play) log(`run.mjs: signed in as ${o.as}: account ${play.account_key}, streams ${play.streams.game} and ${play.streams.profile}`);
const PORTS = { 51241: "game", 51220: "profile" };
const route = play ? (address, port) => PORTS[port] && { url: play.streams[PORTS[port]], token: play.session_token, origin: PAGE } : undefined;
const tmweb = await loadTmweb((await import(pathToFileURL(path.resolve(o.module)).href)).default, log, { transport: webSocketTransport(route, log) });
tmweb.FS.mkdir(ROOT);
tmweb.FS.mount(tmweb.NODEFS, { root: path.resolve(positionals[0]) }, ROOT);
const options = { viewer: o.viewer, probe: o.probe ? 1 : 0, ...(play && { pol_id: play.legacy.pol_id, password: play.legacy.password }) };
for (const [key, value] of Object.entries(options)) {
  if (tmweb.configure(key, value) !== 0) {
    console.error(`run.mjs: bad option ${key} ${value}`);
    process.exit(2);
  }
}
if (tmweb.boot(ROOT, { trace: o.trace, until: o.until }) !== 0) process.exit(1);

const budget = Number(o.budget), slice = Number(o.slice), idleMs = Number(o["idle-ms"]);
let state = RunState.Budget, idleSince;
while (tmweb.instructions() < budget) {
  state = tmweb.run(Math.min(slice, budget - tmweb.instructions()));
  if (state === RunState.Halted) break;
  if (state !== RunState.Idle) {
    idleSince = undefined;
    // A guest that polls without sleeping still lets socket events in
    await new Promise(resolve => setImmediate(resolve));
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
