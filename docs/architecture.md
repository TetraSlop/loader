# Architecture

tmloader hosts the original Tetra Master client in its own process. It loads
the game DLL (`TM_native.dll`) and the Viewer DLL (`polcore_native.dll`) as
in-process COM servers, patches a small set of imports, vtable entries and one
block of game code in memory, and then hands control to the game's own online
startup. The loader never changes the DLLs or data files on disk.

All offsets in this document are RVAs in the supported game DLL (image base
`0x10000000`) unless stated otherwise, written as `+RVA`. They are valid only
for the builds whose SHA256 hashes are pinned in `src/config/validation.cpp`.
In the source, each offset is a named constant next to the hook that uses it.

## Source layout

Paths are relative to `src/`. Headers sit beside their sources and includes
use the source-root path, such as `app/runtime.h`. `tests/` mirrors these
folders.

| Module | Responsibility |
|---|---|
| `app/main` | Entry point, `--check`, `--help` and `--version` |
| `app/loader` | Process setup, COM factories, Viewer setup and GameStart |
| `app/abi.h`, `app/com.h` | COM classes, launch descriptor, Viewer vtable slots, named Viewer command-table indices and `callMethod` for vtable calls |
| `app/hooks.h` | One install function per subsystem |
| `app/log`, `app/runtime` | `std::format` logging (`logLine`), crash handler (one log line and a `MiniDumpNormal` minidump at `logs/tmloader-crash.dmp`), low-integrity token, shared native state |
| `config/` | CLI and config parsing (CLI11), host and path policy, DLL hash validation, SHA256 |
| `graphics/d3d8_abi.h` | Direct3D 8 vtable slots and structures, declared without the obsolete SDK |
| `graphics/graphics` | Direct3D 8 hooks: device creation, reset and presentation |
| `graphics/render_scale` | Resolution presets, automatic selection and the scale shader |
| `graphics/screenshot` | F12 backbuffer capture and PNG encoding |
| `input/` | DirectInput factory hook and the mouse bridge |
| `platform/system` | File-write boundary, virtual registry, message boxes |
| `platform/network` | Connect/DNS redirection and the Viewer profile session |
| `platform/desktop` | Removal of global keyboard hooks, cursor warping and focus stealing |
| `platform/audio` | DirectSound 8 factory |
| `platform/patch` | `replacePointer`, `setImport` and `writeCode` |
| `ui/` | Game window, per-account preferences, the shared ImGui window host, and the settings panel |

Hook functions are named `hooked<ApiName>`, for example `hookedCreateFileA` or
`hookedPresent`.

## Startup sequence

1. `main` parses the configuration (`--config`, or `tmloader.ini` next to the
   executable) and verifies the DLL hashes and required assets. `--check`
   also hashes every file in the `retail-files.txt` beside the executable, then
   stops without loading any DLL.
2. The log is opened, then the process enters the runtime directory and
   disables DEP (the game executes generated code from data pages). It labels
   `pub/`, `logs/` and `settings/` writable at low integrity, then lowers its
   own token to low integrity.
3. The game DLL is loaded and its game object created through
   `DllGetClassObject`. The live GameStart vtable entry (slot 9) must equal
   `+8210`, otherwise startup stops.
4. Each subsystem installs its hooks into the game DLL.
5. The Viewer DLL is loaded and hooked, and its core object is configured
   through the COM slots below. The common-function table must sit at Viewer
   RVA `+6FBE8`. The loader sets the language, registry root and install
   directory, then initializes the Viewer session (see [Network](#network)).
6. GameStart (slot 9) runs with the core object and a 28-byte launch
   descriptor. The game's own message loop runs until the player quits.
7. When GameStart returns, the loader stops the settings panel and terminates
   its own process without unloading the DLLs.

### COM ABI

All calls return `HRESULT` and use `__stdcall`. COM factories use the Windows
SDK `IClassFactory` ABI.

| Call | Slot | Arguments after `this` |
|---|---:|---|
| Viewer SetParamInit | 5 | `HINSTANCE`, mutable `LPSTR` |
| Viewer GetCommonFunctionTable | 7 | `unsigned long**` |
| Viewer SetAreaCode | 27 | `long` |
| Game GameStart | 9 | core `IUnknown*`, `LaunchDescriptor*` (28 bytes) |

Viewer slots 23/24 are the PlayOnline registry-name getters and 25/26 the
SquareEnix ones; neither pair may be skipped when counting to SetAreaCode.
Common-function-table entries are separate from the vtable and use the cdecl
variadic ABI.

## Configuration

`config/` parses the command line and `--config` file with CLI11, so every
option has one declaration. Settings given twice are rejected. The loader adds
its own validation on top:

- The server must be a numeric IPv4 address. The game port cannot equal the
  profile port, and the reserved game port 51241 cannot be used as the profile
  port.
- The account ID must be eight characters from the PlayOnline ID alphabet, and
  the password must not be empty.
- Log, screenshot and settings files must stay inside
  `runtime-dir/logs` or `runtime-dir/settings`. Paths that escape those trees,
  point into `data/` or `pub/`, alias a DLL or are hard links are rejected.
- The game and Viewer DLLs must match their pinned SHA256 hashes.

The runtime path must fit `MAX_PATH` because the client uses ANSI file APIs.

## System

`platform/system` guards writes and replaces the registry.

- **File writes.** `hookedCreateFileA`, installed in both DLLs, rejects
  writes, creates and truncates outside the runtime directory. This is
  compatibility protection, not a sandbox.
- **Virtual registry.** Registry imports (`+204000..+20400C`) return values
  from a process-local table. `LOADDIR` is empty, which avoids the archive-name
  parser's first-dot behavior. `SOUND` is 1: the game reads it at `+187149`,
  stores it at `+2369CC`, and `+180E40` skips all sound initialization when it
  is zero. `FULLSCREEN` is 0.
- **Message boxes.** Native message boxes are logged and answered with `IDOK`.

## Network

`platform/network` points the client at the configured server.

- `hookedConnect` redirects the Game IRC port to `--game-port`.
- `hookedGetHostByName` maps `pp000.pol.com` through `pp999.pol.com`, including
  `pp001` used after a match, to the server address. Unknown hostnames are
  rejected.
- Before the game caches its account identity, the Viewer encodes the POL ID
  (command 854) and sets the account (command 851). Game IRC credentials alone
  are not enough.
- The Viewer's native INET layer and profile endpoint are initialized with the
  server address and `--profile-port`.
- On the first successful Game IRC open, the 0x68-byte profile cipher context
  and its separate 0x1000-byte matrix are copied into loader storage. Game IRC
  slots are reused after a match, and the profile cipher must outlive them.
- The profile bootstrap issues the load-handle, load-character and load-status
  requests (commands 171, 175 and 193) and polls each ticket with the following
  command, for up to five seconds per request.

## Graphics

`graphics/graphics` hooks `Direct3DCreate8` and then the device vtable.

### Device and presentation

The game creates a windowed device (window flag at `+2FF248`) and the loader
activates the window on the first Present. The back buffer is 640x480 times a
scale chosen at startup: 1, 1.5, 2, 2.25, 3, 4, 4.5 or 6.

| Preset | Scale |
|---|---|
| 640x480 | 1x |
| 960x720 | 1.5x |
| 1280x960 | 2x |
| 1440x1080 | 2.25x |
| 1920x1440 | 3x |
| 2560x1920 | 4x |
| 2880x2160 | 4.5x |
| 3840x2880 | 6x |

`D3DSWAPEFFECT_COPY` allows an explicit presentation rectangle, so resizing the
window only changes presentation. One shared content rectangle drives both
presentation and input mapping. Present source rectangles use consistently
rounded edges at fractional scales.

Automatic selection reads the physical dimensions of the monitor hosting the
window at startup, falling back to the adapter display mode, and picks the
smallest preset whose 4:3 area covers it, capped at 4.5x. 6x is a manual
supersampling option. The resolution is a next-launch setting: changing it
mid-session would reset the device while native default-pool resources and an
online match are active. If the larger device or shader cannot be created, the
loader retries at 1x and logs why.

### Scale shader

The renderer sets FVF `0x1C4` at `+18297F` and creates a dynamic vertex buffer
with a 32-byte stride at `+1829C0`. All fourteen draw sites use DrawPrimitive
with this pretransformed XYZRHW, diffuse, specular and UV layout, so a larger
back buffer alone would leave coordinates at 640x480.

Above 1x:

- `hookedSetVertexShader` (slot 76) substitutes a small `vs.1.1` shader. It
  converts screen coordinates to clip space, preserves depth and reciprocal W,
  and passes colors and UVs through. Pixel edges follow
  `(coordinate + 0.5) * scale - 0.5`. The bytecode is hand-written and
  documented instruction by instruction; there is no runtime shader compiler.
- `hookedCreateVertexBuffer` (slot 23) clears the `0x1C4` FVF tag and uses an
  explicit shader declaration instead. D3D8's software path bypasses shader
  input for XYZRHW-tagged buffers, so leaving the tag crashes on draw. Other
  buffer formats pass through.

At 1x both hooks delegate unchanged. Reset keeps the resolution and recreates
the shader.

Native initialization selects linear filtering (`+1828AB`, `+1828BF`) and clamp
addressing (`+1828D3`, `+1828E7`). A scan of the native code through `+1B0000`
found no SetViewport (slot 40) call that would restore a 640x480 viewport after
CreateDevice.

References: [Direct3D 8 interfaces](https://github.com/wine-mirror/wine/blob/master/include/d3d8.h)
and the [Direct3D rasterization convention](https://learn.microsoft.com/en-us/windows/win32/direct3d9/directly-mapping-texels-to-pixels).

### Screenshots

F12 is handled by the game window and never forwarded to the game. A queued
request is consumed on the render thread before Present: the 32-bit back buffer
is copied respecting row pitch, unlocked, and encoded as an opaque PNG with WIC
(render-target alpha is unreliable). Files are created with `CREATE_NEW` under
`logs/screenshots`, so collisions never overwrite. Without a request there is
no readback or disk I/O.

## Input

`input/` hooks the `DirectInput8Create` import (`+204014`) and always returns
the loader's own focus-aware mouse bridge. The game reads only the mouse
through DirectInput; keyboard input arrives as window messages.

- Mouse positions are mapped from the content rectangle into 640x480 and fed
  through the game's own virtual cursor integration (input state at `+2FF570`).
  Clicks in black bars are ignored.
- Mouse state resets on focus, acquisition and window-size transitions.
  Keyboard cursor animation continues while the mouse is stationary.
- A button press released before the next input sample is latched for one
  sample, so short clicks register.
- Input requires the game window itself to have focus and is suspended while
  the settings panel is open. F10 and F12 are never forwarded.

## Desktop

`platform/desktop` and `ui/window` make the game behave like a normal window.

- The window procedure at `+1863D0` discards `WM_SYSCOMMAND` at `+186838`,
  including `SC_SIZE`, `SC_MOVE`, `SC_MAXIMIZE` and `SC_CLOSE`. The loader
  routes these to `DefWindowProc`, handles close and Alt+F4 directly, and posts
  `WM_QUIT` to the native main loop (`+18778E`, `+18798C`). Quitting is not a
  game result or save; the server handles the disconnected session.
- GameStart installs a global `WH_KEYBOARD_LL` hook (`+8030`) that suppresses
  the Windows key, Alt+Tab and other shortcuts regardless of focus. The loader
  suppresses it at import `+2041E4`.
- The game's `ShowCursor` loop, `AttachThreadInput`, `SetForegroundWindow`,
  `SetActiveWindow` and `SystemParametersInfoA` writes (animation,
  foreground lock, screensaver) are virtualized. These replacements affect
  only the game DLL's imports.
- The renderer at `+185753..+185830` warps the OS pointer and clamps the
  software cursor to unscaled client coordinates every frame. A five-byte jump
  skips that block in memory; the DLL hash and the original instruction bytes
  must both match before it is written.
- Message `0x64`, posted by the loss-of-activation handler to force the game
  back to the foreground, is discarded.
- The loader owns the cursor through `WM_SETCURSOR`: it is hidden only over
  focused game content, and borders, black bars and the settings panel keep a
  normal cursor.

## Audio

`platform/audio` hooks the `DirectSoundCreate8` import at `+20401C` (dsound
ordinal 11). The music (`+1AAFCF`) and effect (`+1AA933`) paths both query
`IDirectSoundBuffer8`. Substituting `DirectSoundCreate` initializes but its
buffers return `E_NOINTERFACE` for that interface, leaving playback silent, so
the hook keeps the SDK's exact `DirectSoundCreate8` signature. Decoding, mixing
and volume stay in the game.

See the [buffer API](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee418039(v=vs.85))
and [DirectSoundCreate8](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee416762(v=vs.85)).

## Settings panel

`ui/settings_ui` runs Dear ImGui with its official Win32 and DirectX 9 backends
in a separate owned window and thread, so it works while the game is inactive
and does not pause an online match. Only that thread touches ImGui or its DX9
device. Changes are posted to the game's window thread, and a mutex protects
the shared settings snapshot. Both threads enable per-monitor DPI awareness
through ImGui's helper, avoiding Windows bitmap scaling.

Preferences are stored per account as JSON in
`runtime-dir/settings/<POL-ID>.json`, with the same path and hard-link
protections as logs. Malformed files fall back to defaults; out-of-range sizes
and scales fall back individually. Only the eight advertised scales are accepted.

## Known limitations

- Offsets and pointer scans are tied to the two pinned DLLs. Other builds are
  rejected rather than supported.
- Native state is module-global, so one process hosts one game session. There
  is no reconnect or account manager.
- Profile bootstrap polling is synchronous, up to five seconds per request.
- After GameStart returns, the loader terminates its own process. Full COM and
  DLL teardown is not attempted.
- The input bridge implements only the mouse path the game uses;
  joysticks, buffered input and force feedback are not supported.
- Moving the window to another monitor does not change the automatic
  resolution; it is chosen once at startup.
- The viewport notes above come from reading the native code, not from
  tests.
