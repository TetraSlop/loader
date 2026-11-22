# Browser client

Tetra Master in a browser, without Wine: an x86-32 user-mode interpreter compiled to WebAssembly. It runs the user's
own `TM_native.dll` and `polcore_native.dll`, and answers their Win32, Direct3D 8, DirectInput, DirectSound and
socket calls with C++ stand-ins. WASM_SPIKE.md on the `wasm-spike` branch has the reasoning and the measurements.

This branch (`feat/browser-client`) is separate from `main` and is not merged into it.

## What works

- The interfaces: guest memory, the CPU context, the guest/host boundary, guest threads and the f80 API. Most
  bodies are still stubs.
- Guest memory: identity-mapped in Wasm (guest address = linear-memory address), offset into a 4 GiB reservation
  natively.

## Layout

| Folder | What |
|---|---|
| `core/memory.h` | Guest memory: the address layout, mapping, checked reads and writes that throw `GuestFault`, allocation |
| `core/cpu.h` | Registers, flags, x87 state, `Context` per thread, the interpreter's `run`/`step` and why it stopped |
| `core/boundary.h` | Stand-ins: synthetic module images with exports, trap slots, `Call` (arguments, `this`, strings), `Outcome` (`Return`, `Block`, `Resume`, `Halt`), COM vtables and guest objects |
| `core/threads.h` | Guest threads, `Waitable` kernel objects, `Wait`/`WaitResult`, the scheduler and its clock |
| `core/machine.h` | One process: the run loop, structured exceptions, `callGuest` for callbacks into guest code |
| `f80/f80.h` | The 80-bit soft float the x87 runs on |
| `host/host.h` | The module's way out: log, keyboard and mouse, sound, stream sockets, and the exports the host drives |
| `web/` | The host side: the Node runner, the page and its worker, input, WebAudio, the game-file picker, the WebSocket transport |
| `app/` | The command-line entry for Node and native runs |
| `tests/` | Catch2 tests, native only |

## Build

Emscripten 5.0.7, as Kestrel uses:

```sh
emcmake cmake -G Ninja -S browser -B build/browser-wasm
cmake --build build/browser-wasm
node build/browser-wasm/tmweb.js
```

Native, for tests (any C++23 compiler; on Windows from a developer prompt):

```sh
cmake -G Ninja -S browser -B build/browser
cmake --build build/browser
ctest --test-dir build/browser --output-on-failure
```

## Node runner and page

Both load `tmweb-web.mjs` from the Wasm build. The Node runner mounts a runtime folder and runs to a stop condition:

```sh
node browser/web/run.mjs <runtime folder> [--budget N] [--until d3d8.dll!Direct3DCreate8] [--trace] [--slice N] [--idle-ms N]
```

The page picks the runtime folder (the File System Access API, or a directory upload where there is none), keeps the
files `retail-files.txt` lists, caches them in the origin private file system and runs the module in a worker. The
worker copies the files into MEMFS under `/game`, hands the module the canvas as an OffscreenCanvas
(`Module.canvas`, which Kestrel's `gl/gl.js` draws on), forwards keys and the pointer to `tmweb_input`, and posts
the sound `tmweb_sound_take` returns to an AudioWorklet.

```sh
cd browser/web
npm ci
npm run dev     # http://localhost:5173, the module from build/browser-wasm (or $TMWEB_BUILD)
npm test        # the runner checks need the Wasm build; $TM_RUNTIME boots the user's folder too
npm run build   # the page into build/browser-page
```

Sockets go through `web/transport.js`: one binary WebSocket per guest connection, at the URL `route(address, port)`
gives, with the play-session token as the first text message, as tmserver's gateway expects of a browser stream
(its LOADER_AUTH_SPEC.md, "Browser clients"). No route exists until sign-in lands, so every connect fails.

## Game files

Nothing from the game is committed: no DLLs, images, data files, traces or recompiled code. Tests that need the
user's files read them from paths in environment variables and skip without them, so CI runs without game files.

## Origins

Code taken from Kestrel's port (the owner's theater-fit `kestrel/port/compat`, commit `e1240c4`, GPL-3.0) is noted
in the file that holds it. `web/input.js` and `web/speakers.js` are taken from theater-fit's `kestrel/port/run/`
(commit `658d8c2`), the owner's code, relicensed here under GPL-3.0.
