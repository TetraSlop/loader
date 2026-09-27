# tmloader

A standalone 32-bit host for the original Tetra Master PC client, arranged into
small loader modules in the style of xiloader. It opens the original online
startup and connects to a separately running compatible game/profile server.

For the integrated local setup, the sibling `tmserver` project now provides
`Play.cmd` and a [native test checklist](../tmserver/docs/NATIVE_TEST_CHECKLIST.md).
It starts the packaged services, passes their free ports and account IDs to this
loader, and keeps them alive until all game windows close. This machine's
prepared working copy is in ignored `runtime/`; `tmloader.ini` points there.

The user confirmed the desktop input/focus fixes work. The remaining silent
audio was traced to the loader selecting DirectSound instead of DirectSound 8;
the corrected build passes real Windows music/effect buffer checks without
playing audio. Audible in-game output still needs confirmation after relaunch.
See [validation](docs/VALIDATION.md).

## Window controls

- **F10** opens/closes the Dear ImGui settings panel; Escape closes the panel.
- **Alt+Enter** toggles borderless fullscreen on the current monitor.
- Drag the edges to resize, or use the normal minimize/maximize buttons.
- The title-bar **X**, **Alt+F4**, and **Quit game** exit through the game's
  existing message loop.

The panel offers 640x480, 960x720 and 1280x960 presets and a **Keep original
4:3 proportions** toggle. Rendering remains 640x480 and scales into the window;
mouse mapping follows the same content rectangle and ignores black bars.
Alt+Tab and Windows shortcuts remain available. The wrapper disables the
client's legacy global keyboard hook, pointer warping and focus reclaim.
The Windows cursor is hidden only over focused game content; settings, window
borders, black bars and other apps keep a normal cursor.
The panel has its own window/render loop so it can work while the original
client is inactive. It does not pause an online match.

Window size, aspect preference and fullscreen state are remembered per account
in `runtime/settings/<POL-ID>.window`. Music and effect levels remain in the
game's Options menu; the loader now enables the native sound engine at startup.

## Build and headless checks

Requires CMake 3.24+ and Visual Studio with the Desktop C++ workload and an x86
Windows SDK toolchain. CMake downloads Dear ImGui 1.92.5 from an immutable
upstream commit and verifies its SHA256. Later builds use the cached source.
Its official Win32/DX9 backends render the settings panel; the game retains
its D3D8 renderer. See [third-party notices](docs/THIRD_PARTY.md).

```powershell
cmake -S . -B build -A Win32
cmake --build build --config RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo --output-on-failure
```

Output: `build/RelWithDebInfo/tmloader.exe`. The executable has an explicit
**asInvoker** manifest; it does not request administrator privileges.

The repository contains loader source only. It does not include or download
game/Viewer DLLs, game assets, local account state or a server. Point it at an
external user-owned runtime directory containing the previously prepared,
unchanged DLLs and assets:

```
TM_native.dll
polcore_native.dll
data/                       (complete Tetra Master assets)
data/utf/env.dat             (Viewer environment data)
pub/home00/option.bin        (Viewer local settings)
```

The known DLL SHA256 hashes are pinned in `src/validation.cpp`. Unknown builds
are rejected before loading; fixed offsets require the matching original
builds. The loader executable does not discover installed files or copy assets.
The optional `python tools/prepare_runtime.py --source PATH` setup helper copies
a prepared user-owned runtime into ignored `runtime/`, labels that new working
copy for the existing low-integrity client, and writes local configuration.
It refuses existing destinations and never launches the game.
Legacy runtime paths must fit MAX_PATH and the client uses ANSI file
APIs internally.

Copy `tmloader.example.ini` to the ignored `tmloader.ini`, then set runtime-dir
to the external directory. Paths for game/core/log are relative to runtime-dir
unless absolute. A relative runtime-dir in a config is resolved beside that
config. Command-line settings override config entries.

Log destinations must remain inside `runtime-dir/logs`; the default is
`logs/tmloader.log`. Captures use `logs/captures/frame_NNNNN.bmp`. Canonical
paths escaping that tree, paths inside data/pub, input DLL aliases and existing
hard-linked output files are rejected. These directories are created only
during an actual launch, not by `--check`.

This command is read-only: it verifies config, DLL hashes and representative
required assets without loading DLLs, creating windows or connecting:

```powershell
.\build\RelWithDebInfo\tmloader.exe --config .\tmloader.ini --check
```

When native testing is wanted, first start the separate compatible server,
then run the same command without `--check`. No game launch is part of the build
or tests. The loader neither starts/stops the server nor manages other processes.
Normal operation lasts until the user closes the game; there is no watchdog or
automatic menu navigation. `--help` lists settings. Diagnostics opt into frame
captures inside the runtime directory.

The existing loopback server uses the default synthetic account EEEEEEEF /
tm-local-test and ports51241/51220. Other configured accounts/endpoints need a
server supporting them; only the default protocol path has prior native
evidence. Configuration passwords are local plaintext: do not commit the
ignored personal config. Credentials are not logged.

## Compatibility behavior

- Direct COM factories, no registration or installed game changes.
- Original network startup, account context and profile bootstrap; persistent
  profile cipher storage survives game-connection reuse.
- Numeric service IPv4 configuration; legacy `pp000.pol.com` through
  `pp999.pol.com` map to that server. Unknown hostnames/endpoints are rejected.
- Process-local registry configuration with native audio enabled, empty
  LOADDIR prefix, scaled windowed640x480 Direct3D8 rendering, first-frame
  activation and normal window controls.
- Native DirectInput first, then the existing focus-aware keyboard/mouse
  fallback. Pointer alignment and short-click latch are retained.
- The legacy process keeps the existing reduced-integrity/DEP compatibility
  settings. Runtime writable data must already permit low-integrity writes;
  this loader does not change directory ACLs or Windows UAC settings. File
  interception is best-effort compatibility protection, not a complete sandbox.

See [architecture](docs/ARCHITECTURE.md) for module responsibilities and known
debt. xiloader was a read-only architecture reference; this is not an upstream
fork and has no remote configured.
