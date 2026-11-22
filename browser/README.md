# Browser client

Tetra Master in a browser, without Wine: an x86-32 user-mode interpreter compiled to WebAssembly. It runs the user's
own `TM_native.dll` and `polcore_native.dll`, and answers their Win32, Direct3D 8, DirectInput, DirectSound and
socket calls with C++ stand-ins. WASM_SPIKE.md on the `wasm-spike` branch has the reasoning and the measurements.

This branch (`feat/browser-client`) is separate from `main` and is not merged into it.

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
- Not yet: the Win32 stand-ins and the protector unpack in C++ (`Boundary::dispatch`, `Machine::raise`, `boot`).

## Layout

| Folder | What |
|---|---|
| `core/memory.h` | Guest memory: the address layout, mapping, checked reads and writes that throw `GuestFault`, allocation |
| `core/cpu.h` | Registers, flags, x87 state, `Context` per thread, the interpreter's `run`/`step` and why it stopped |
| `core/boundary.h` | Stand-ins: synthetic module images with exports, trap slots, `Call` (arguments, `this`, strings), `Outcome` (`Return`, `Block`, `Resume`, `Halt`), COM vtables and guest objects |
| `core/threads.h` | Guest threads, `Waitable` kernel objects, `Wait`/`WaitResult`, the scheduler and its clock |
| `core/machine.h` | One process: the run loop, structured exceptions, `callGuest` for callbacks into guest code |
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
```

## Game files

Nothing from the game is committed: no DLLs, images, data files, traces or recompiled code. Tests that need the
user's files read them from paths in environment variables and skip without them, so CI runs without game files.

## Origins

`third_party/softfloat` is Berkeley SoftFloat 3e (BSD 3-Clause), commit `07ebd29`; its README lists what was
taken.

Code taken from Kestrel's port (the owner's theater-fit `kestrel/port/compat`, commit `e1240c4`, GPL-3.0) is noted
in the file that holds it. `web/input.js` and `web/speakers.js` are taken from theater-fit's `kestrel/port/run/`
(commit `658d8c2`), the owner's code, relicensed here under GPL-3.0.

`compat/` is that port's `dx/` (Direct3D 8, D3DX pixel formats, DirectInput, DirectSound), `gl/` and the
`win32/` header shims they need, copied unchanged at `e1240c4` except:

- The DirectX 8 SDK's headers are not used. `dx/d3d8.h` reads mingw-w64's own `d3d8.h` (Wine's, LGPL-2.1+), and
  `dx/d3dx8.h` is ours: just `D3DXMATRIX`, `D3DXMatrixMultiply` and two filter constants, which `gl/` and
  `dx/d3dx8_pixels.cpp` use. The game links its own D3DX.
- `dx/d3dx8_math.cpp` keeps only `D3DXMatrixMultiply`.
- `NullDevice::SetCursorPosition` takes `UINT` coordinates, as Wine's header declares it (the SDK's takes `int`).
- `win32/guids.cpp` holds the two GUID definitions from the port's `win32/impl/com.cpp`.
