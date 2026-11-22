# Browser client

Tetra Master in a browser, without Wine: an x86-32 user-mode interpreter compiled to WebAssembly. It runs the user's
own `TM_native.dll` and `polcore_native.dll`, and answers their Win32, Direct3D 8, DirectInput, DirectSound and
socket calls with C++ stand-ins. WASM_SPIKE.md on the `wasm-spike` branch has the reasoning and the measurements.

This branch (`feat/browser-client`) is separate from `main` and is not merged into it.

## Where this stands

Parked on 2026-11-22.

- **Checkpoint A passed.** With no server the page reaches the connection error ("TRM-8196-37302 Error occurred
  while connecting to the server") through WebGL2. Chromium's frame 300 scores 0.0021 against native tmloader's
  frame 300 (`tools/compare_frame.py`, threshold 0.02). The game paces at about 57 fps, and the first frame comes
  about 3 s after boot. A click on Retry reaches the game and brings up its retry screen. That visual check stands in
  for a native input reference.
- **Checkpoint B is next:** Now Loading, the handle screen, and the main menu after Game IRC sign-in and the profile
  load, compared with native's with-server frames 300, 600 and 1200. It needs:
  - polcore's PR #17 (real polcore as the Viewer, `tmweb_configure('viewer','polcore')`), still open. Its CI is red.
    Merged onto this branch, the native test "Messages reach the window procedure, and host keys go through the
    low-level hook" segfaults before its first assertion.
  - tmserver's fix for hosted-account passwords. tmserver hands out 16 characters (`token_hex(8)`), and polcore
    takes at most 15. Until the fix lands, run a local tmserver with `token_hex(7)` in `register_hosted_account`.
  - tmserver's dev identity, which is on tmserver main. Start it with `--auth-config examples/auth-dev.json
    --auth-port 8081 --auth-insecure-loopback --auth-dev-identity`, or use `tools/polcore/gateway.py` from #17.
    Serve the page from `http://localhost:5173`. The harness does not sign in yet.
- **Known issues:**
  - Native input automation does not reach the tmloader window from a background session, so
    `tools/capture_reference.py`'s `--key`, `--mouse` and `--click` are best effort.
  - Startup runs at about 50–65 MIPS in Chrome. A decoded-instruction cache would shorten the boot.
  - The D3DX stand-in (`com/d3dx.cpp`) handles A8R8G8B8 and X8R8G8B8, one mip level, no filter and 4-channel
    PNGs. Anything else goes through the game's own D3DX.

The title-screen check, with native references from `tools/capture_reference.py`:

```sh
python browser/tools/capture_reference.py --loader <tmloader.exe> --config <tmloader.ini> --runtime <runtime> --out <folder>
node browser/web/harness.mjs --folder <runtime folder> --frames 300 --reference <folder>/frame300.png
node browser/web/harness.mjs --folder <runtime folder> --frames 360 --click <x>,<y>@300   # input
```

## What works

- **The interpreter** (`core/cpu.cpp`, `core/x87.cpp`): the x86-32 integer ISA, 16- and 32-bit addressing, string
  instructions, precise faults, CPUID without MMX or SSE, and every x87 instruction on `f80/`.
  - Stepped in lockstep with Unicorn over 1,316,129 instructions of the real boot run (protector, CRT start-up,
    GameStart), with registers, flags and memory writes identical.
  - TM's battle and growth routines run through it and match tmserver's frozen native vectors: 1,168 battles and
    6,248 growth cases. Growth also agrees with Unicorn on every case at 24, 53 and 64-bit precision.
- **Guest memory**: identity-mapped in Wasm (guest address = linear-memory address), offset into a 4 GiB reservation
  natively.
- **Threads and the run loop**: TEBs and stacks, waits with timeouts, round robin, idle reporting, structured
  exceptions handed to `Machine::raise`, and `callGuest` for callbacks.
- **The x87's arithmetic** (`f80/`), checked against a real x87 (see "The x87 in software").
- COM proxies: guest vtables for host objects, generated from the C++ method types (`com/proxies.h`).
- d3d8.dll, dinput8.dll and dsound.dll on compat's objects (`com/directx.cpp`, Wasm only): every slot of the
  interfaces they hand out, locks through guest staging buffers, and CreateDevice setting the x87 to 24-bit precision.
  Enumeration callbacks go through `Machine::callGuest`; volume texture locks halt.
- **The boot** (`app/boot.cpp`, `hle/boot.h`): TM_native.dll goes through its protector and the COM start-up to its
  first Direct3D call, `Direct3DCreate8(220)`, against a stand-in Viewer. On the interpreter, natively and as Wasm
  under Node (about 177 million instructions, 3 s), it matches the Unicorn reference run: the 20 SEH round trips,
  all 148 imports named, the image byte-identical to tmserver's verified one when the protector hands over to the
  CRT, then GameStart's 41 imports with their counts, 6 core slots, 11 Viewer entries, 59 file opens in order, the
  registry values, and the `Tetra Master` window class at `+1863D0`.
- **The real Viewer** (`hle/viewer.h`, behind a switch): the user's `polcore_native.dll` at `0x10400000`, unpacked
  by its own POL1 stub, set up as tmloader sets it up, with winsock on the host's WebSocket transport
  (`hle/ws2_32.cpp`). TM boots to `Direct3DCreate8(220)` on it, natively and under Node (215 million instructions,
  4 s), and a scripted run logs in to Game IRC and loads the handles, characters and status from a local tmserver
  through its browser streams (31 million instructions, 2 s). See "The real Viewer".
- Win32 stand-ins (`hle/`) for what TM.dll and its protector import from kernel32, user32, advapi32, imm32, ole32,
  oleaut32 and gdi32. The registry serves tmloader's virtual values. Files come read-only from the runtime folder
  through `hle::HostFiles`.

## Layout

| Folder | What |
|---|---|
| `core/memory.h` | Guest memory: the address layout, mapping, checked reads and writes that throw `GuestFault`, allocation |
| `core/cpu.h` | Registers, flags, x87 state, `Context` per thread, the interpreter's `run`/`step` and why it stopped |
| `core/boundary.h` | Stand-ins: synthetic module images with exports, trap slots, `Call` (arguments, `this`, strings), `Outcome` (`Return`, `Block`, `Resume`, `Halt`), COM vtables and guest objects |
| `core/threads.h` | Guest threads, `Waitable` kernel objects, `Wait`/`WaitResult`, the scheduler and its clock |
| `core/machine.h` | One process: the run loop, structured exceptions, `callGuest` for callbacks into guest code |
| `hle/` | Win32 stand-ins, the PE loader, the host file API (`files.h`), the boot sequence (`boot.h`), the real Viewer (`viewer.h`) and winsock (`ws2_32.cpp`) |
| `host/host.h` | The module's way out: log, keyboard and mouse, sound, stream sockets, and the exports the host drives |
| `web/` | The host side: the Node runner, the page and its worker, input, WebAudio, the game-file picker, the WebSocket transport |
| `f80/f80.h` | The 80-bit soft float the x87 runs on, on Berkeley SoftFloat 3e (`third_party/softfloat`) |
| `com/` | COM proxies, and the DirectX DLLs built on them |
| `compat/` | Kestrel's Direct3D 8 on WebGL2, DirectInput and DirectSound (Wasm only; see Origins) |
| `app/` | The command-line entry for Node and native runs |
| `tests/` | Catch2 tests: native ones, and `directx_test.cpp`, which builds for Wasm only and runs under Node |
| `tools/` | Local checks with the user's files: `difftest.py`, `rules.py`, the Unicorn reference run in `reference/`, the C interface they use (`capi.cpp`) and the polcore research in `polcore/` |
| `docs/` | Design notes: `polcore.md` |

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

The Wasm build also makes `tmweb_wasm_tests.js`, for what builds only there: `node build/browser-wasm/tmweb_wasm_tests.js`.
compat/ is written against the Windows headers, so the Wasm build downloads mingw-w64 v14.0.0's (hash-pinned).
The boot test needs the user's runtime folder and skips without it. `TMWEB_TM_MAPPED` adds the comparison with
tmserver's `var/native/TM_emulated_mapped.dll`:

```sh
TMWEB_RUNTIME=<runtime folder> TMWEB_TM_MAPPED=<tmserver>/var/native/TM_emulated_mapped.dll \
    ctest --test-dir build/browser -R TM_native --output-on-failure
```

`-DTMWEB_UNICORN=ON` also builds `tmweb_unicorn_tests`: the same guest and boot tests with Unicorn 2.1.4 as the CPU,
for comparison with the interpreter. CI does not build it.

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

The canvas is 640x480 at the largest whole scale that fits the window (`?scale=N` sets one), or the largest 4:3
below 1x. The worker paces itself on the game's frames: each task runs until the game presents one (`host::presented`)
or 12 ms pass, and after a frame the next task waits for the display's refresh. Keys and the pointer go to
`tmweb_input`, which keeps `host::inputState()`, tells the `host::onInput` listeners and drives compat's DirectInput;
a button release waits until the game has had a frame to see the press. What the game writes (`host::fileWritten`)
is copied from MEMFS to OPFS, and a picked folder's `pub/` and `settings/` come along with the listed files.

`web/harness.mjs` runs the page in headless Chromium: it uploads a runtime folder through the directory input,
starts the game, waits for frames and screenshots the canvas at 640x480, then compares it with a reference through
`tools/compare_frame.py`. Screenshots of game frames stay local (`build/` by default) and are never committed.

```sh
node browser/web/harness.mjs --folder <runtime folder> --frames 60 --reference <title screen.png>
node browser/web/harness.mjs --synthetic     # CI: stand-in files, checks the harness itself
```

Sockets go through `web/transport.js`: one binary WebSocket per guest connection, at the URL `route(address, port)`
gives, with the play-session token as the first text message, as tmserver's gateway expects of a browser stream
(its LOADER_AUTH_SPEC.md, "Browser clients"). No route exists until sign-in lands, so every connect fails.

## The real Viewer

By default the boot answers TM's Viewer calls with a stand-in. `tmweb_configure("viewer", "polcore")` before
`tmweb_boot` (`--viewer polcore` for the Node runner) runs the user's `polcore_native.dll` instead
(`docs/polcore.md` has the research):

- It is mapped at `layout::kViewerImage` with the data directory's relocations applied, as the OS loader does; its
  POL1 stub then decompresses `.text` and applies the `.text` relocations it hides from the OS.
- Between TM's CreateInstance and GameStart the boot runs tmloader's Viewer set-up (`src/app/loader.cpp`,
  `src/platform/network.cpp`): DllMain, the core object, the area code, parameters, languages, registry root, install
  folder, account, and the INET and profile clients. GameStart then gets polcore's core object.
- Common-table entries 132 and 133 are hooked as tmloader hooks them: Game IRC open gets the server and the play
  session's account (`pol_id`, `password`), and the first accepted login seeds the profile cipher and loads the
  handles, characters and status, which pol.exe would have loaded before starting the game.
- Its internal addresses fit only the pinned build; the PE header and the common table's address are checked.
- The RSA key generation in CreateInstance runs four routines as host code (`keygen`, on by default): 461 million
  instructions to `Direct3DCreate8` become 205 million. `tests/viewer_test.cpp` checks them against polcore's own on
  inputs recorded from a real key generation.
- polcore's sockets are non-blocking TCP polled with `select`; each is one host stream, routed by port: 51241 to the
  game stream, 51220 to a profile stream.

A scripted login without TM.dll (`probe`) checks the whole path against a local tmserver:

```sh
# tmserver's services and gateway on loopback, with a stand-in sign-in; writes a browser play session
TMSERVER=<tmserver checkout> python browser/tools/polcore/gateway.py play.json &
node browser/web/run.mjs <runtime folder> --viewer polcore --probe --session play.json
# viewer: profile request 171 ticket 0 result 1 ... viewer probe: Game IRC login accepted; profile handles 1, ...
```

`--session` without `--probe` boots TM.dll with that account and routes its connections the same way. Under Node the
transport sends the session's `origin` as the WebSocket's Origin header, as a page would.

## The x87 in software

`f80/` is the x87's arithmetic on Berkeley SoftFloat 3e: the 80-bit format with denormals, pseudo-denormals,
infinities, quiet and signaling NaNs, and the unnormals the 387 and later reject; precision control at 24, 53 and
64 bits; all four rounding modes; the exception flags and C1. The interpreter keeps the register stack, the tag word
and stack faults; it makes a fresh `Env` per instruction with `envFromControl`.

How close it is to hardware, measured on an AMD Ryzen 9 9950X3D with `tests/f80_hostdiff.cpp` (random operands
biased toward edge cases, 100,000 per instruction at each precision and rounding mode):

- Bit for bit, with the same exception flags and C1: FADD, FSUB, FMUL, FDIV, FSQRT, FRNDINT, FSCALE, FPREM, FPREM1
  (including the partial step), FXTRACT, FCOM, FUCOM, FXAM, FST m32/m64, FIST m16/m32/m64, FLD m32/m64, the
  load-constant instructions, and the memory forms FADD m32, FMUL m64, FIDIV m32 and FICOM m32. No differences.
- Arithmetic with an m32 or m64 operand is not FLD then the register form: the x87 converts the operand inside
  the instruction. Convert it with `floatOperand`/`doubleOperand` and call `denormalOperand` afterwards; the
  header says how.
- The transcendentals are computed in SoftFloat's 128-bit format and rounded once, so they are nearly always
  correctly rounded; the x87 is not, so some differ from it by one ulp. None differ by more, and their exception
  flags (other than inexact and C1) always match.

  | | Bit-exact | 1 ulp off |
  |---|---:|---:|
  | FSIN | 99.8% | 0.2% |
  | FCOS | 99.8% | 0.2% |
  | FSINCOS | 99.6% | 0.4% |
  | FPTAN | 86.6% (99% when rounding to nearest) | 13.4% |
  | FPATAN | 99.5% | 0.5% |
  | F2XM1 | 99.3% | 0.7% |
  | FYL2X | 99.2% | 0.8% |
  | FYL2XP1 | 99.2% | 0.8% |

- FSIN, FCOS, FSINCOS and FPTAN reduce the argument by the x87's 66-bit pi, as the hardware does, so large
  arguments match it rather than the true sine.
- Outside their documented ranges, F2XM1 (|x| > 1) and FYL2XP1 (x <= -1) are undefined; they return x, inexact,
  as the measured CPU does. Intel CPUs have not been measured.

Not modelled: the biased-exponent result an unmasked overflow or underflow delivers (the flags are right; the game
runs with every exception masked), and FBLD/FBSTP. SoftFloat keeps its rounding mode and flags in globals, so f80
runs on one host thread.

Native, Wasm (Emscripten 5.0.7 under Node), MSVC x86 and x64, and GCC x86-64 builds give identical results:
`tests/f80_digest.cpp` hashes every operation over fixed random operands and checks the hash in ctest for both
builds. If a change to f80 is meant to change results, rerun `f80_hostdiff` and update the digest.

To compare with your own CPU (Windows, MSVC, x86 only):

```sh
cmake -S browser -B build/browser-x86 -A Win32
cmake --build build/browser-x86 --config Release --target f80_hostdiff
build/browser-x86/Release/f80_hostdiff.exe 100000        # cases per instruction and mode; optional seed and instruction
```

## Local checks with the game files

These need the user's own runtime folder and skip without it. They use Python with `pefile`, `capstone` and
`unicorn`, and the native build's `tmweb_capi` library.

```sh
# The interpreter against Unicorn over stretches of the boot run
TM_RUNTIME=<runtime folder> python browser/tools/difftest.py [--count 200000] [--x87]
# Battle and growth against tmserver's frozen vectors, and growth at 24/53/64 bits against Unicorn
TM_RUNTIME=<runtime folder> TMSERVER=<tmserver checkout> python browser/tools/rules.py
# The Wasm build under Node, booted to Direct3DCreate8, against the Unicorn reference's observations
TM_RUNTIME=<runtime folder> python browser/tools/boot_compare.py
```

Reference frames come from native tmloader. `capture_reference.py` runs it with `--diagnostics` on a scratch copy of
the runtime folder at 640x480 (screenshots at frames 60, 300, 600, 900, 1200, ...), optionally with a tmserver on a
scratch state, so the game reaches the title and main menu by frame 1200. `compare_frame.py` scores a browser frame
against one: the share of pixels that differ after a slight blur, a match at 0.02 or below. Keep the frames outside
the repository.

```sh
python browser/tools/capture_reference.py --loader <tmloader.exe> --config <tmloader.ini> --runtime <runtime folder> \
    --out <folder outside the repo> [--tmserver <tmserver checkout>]
python browser/tools/compare_frame.py <browser frame.png> <folder>/frame1200.png
```

## Game files

Nothing from the game is committed: no DLLs, images, data files, traces or recompiled code. Tests that need the
user's files read them from paths in environment variables and skip without them, so CI runs without game files.

## Origins

`third_party/softfloat` is Berkeley SoftFloat 3e (BSD 3-Clause), commit `07ebd29`; its README lists what was
taken.

Code taken from Kestrel's port (the owner's theater-fit `kestrel/port/compat`, commit `e1240c4`, GPL-3.0) is noted
in the file that holds it: `hle/kernel32.cpp` takes the narrow and wide string conversion and the copy-out rule
(`win32/impl/kernel.cpp`). The registry values and the GUIDs come from tmloader's own `src/platform/system.cpp` and
`src/app/loader.cpp`. The boot's stand-in Viewer and the protector observations come from the spike's Python run. `web/input.js` and `web/speakers.js` are taken from theater-fit's `kestrel/port/run/`
(commit `658d8c2`), the owner's code, relicensed here under GPL-3.0.

`compat/` is that port's `dx/` (Direct3D 8, D3DX pixel formats, DirectInput, DirectSound), `gl/` and the
`win32/` header shims they need, copied unchanged at `e1240c4` except:

- The DirectX 8 SDK's headers are not used. `dx/d3d8.h` reads mingw-w64's own `d3d8.h` (Wine's, LGPL-2.1+), and
  `dx/d3dx8.h` is ours: just `D3DXMATRIX`, `D3DXMatrixMultiply` and two filter constants, which `gl/` and
  `dx/d3dx8_pixels.cpp` use. The game links its own D3DX.
- `dx/d3dx8_math.cpp` keeps only `D3DXMatrixMultiply`.
- `NullDevice::SetCursorPosition` takes `UINT` coordinates, as Wine's header declares it (the SDK's takes `int`).
- `win32/guids.cpp` holds the two GUID definitions from the port's `win32/impl/com.cpp`.
- `dx/dinput.cpp`: the mouse's `GetDeviceState` clears the relative axes after reading them, so each read reports
  the motion since the last one.
- `dx/d3d8_resources.cpp`: `UpdateTexture` copies each level on the CPU. Kestrel's returned `D3DERR_INVALIDCALL`;
  TM fills every texture in system memory and updates a default-pool one from it, so without it nothing it drew
  had pixels.
- `third_party/stb/stb_image.h` is stb_image v2.34 (public domain or MIT), unmodified; its README gives the commit.
  `com/d3dx.cpp` uses it for the D3DX texture stand-in.
