// The Wasm module (host/host.h's exports) as the page's worker and the Node runner use it. The module reads the game
// files under ROOT: the worker copies them into MEMFS there, the Node runner mounts the runtime folder.
import { webSocketTransport } from "./transport.js";

export const ROOT = "/game";
export const RunState = { Budget: 0, Idle: 1, Halted: 2 };

/**
 * Starts the module. factory is the Emscripten build's default export (tmweb-web.mjs); options go to it as well
 * (canvas, instantiateWasm, preRun...). onLog gets each line of the module's log.
 */
export async function loadTmweb(factory, onLog, { transport = webSocketTransport(undefined, onLog), ...options } = {}) {
  const m = await factory({ ...options, onLog, transport, print: onLog, printErr: onLog });
  const string = s => {
    const n = m.lengthBytesUTF8(s) + 1, p = m._malloc(n);
    m.stringToUTF8(s, p, n);
    return p;
  };
  transport.listen((id, kind, bytes) => {
    const p = bytes?.length ? m._malloc(bytes.length) : 0;
    if (p) m.HEAPU8.set(bytes, p);
    m._tmweb_socket_event(id, kind, p, bytes?.length ?? 0);
    if (p) m._free(p);
  });
  return {
    FS: m.FS,
    NODEFS: m.NODEFS,
    /** 0 on success. trace logs every stand-in call; until halts at the first call to that "module!name". */
    boot(root = ROOT, { trace = false, until = "" } = {}) {
      const [r, u] = [string(root), string(until)];
      try {
        return m._tmweb_boot(r, trace ? 1 : 0, u);
      } finally {
        m._free(r);
        m._free(u);
      }
    },
    /** One of RunState. */
    run: budget => m._tmweb_run(budget),
    instructions: () => m._tmweb_instructions(),
    /** input.js's InputKind, and its a and b. */
    input: (kind, a, b) => m._tmweb_input(kind, a, b),
    /** The stereo frames played since the last call, left and right interleaved, at SOUND_RATE. */
    takeSound() {
      const p = m._tmweb_sound_take(), n = m._tmweb_sound_frames();
      return m.HEAPF32.slice(p / 4, p / 4 + 2 * n);
    },
  };
}

/** host::kSoundRate */
export const SOUND_RATE = 48000;
