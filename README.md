# tmloader

A standalone 32-bit host for the original Tetra Master PC client, arranged into
small loader modules in the style of xiloader. It opens the original online
startup and connects to a separately running compatible game/profile server.

For the integrated local setup, the sibling `tmserver` project now provides
`Play.cmd` and a [native test checklist](../tmserver/docs/NATIVE_TEST_CHECKLIST.md).
It starts the packaged services, passes their free ports and account IDs to this
loader, and keeps them alive until all game windows close. This machine's
prepared working copy is in ignored `runtime/`; `tmloader.ini` points there.

The user confirmed desktop input/focus and sound work. Optional higher internal
rendering resolutions are now available in F10 settings. These pass actual
Direct3D 8 pixel comparisons; the complete game still needs a user test at the
new resolutions. See [validation](docs/VALIDATION.md).

## Source layout

```text
cmake/Dependencies.cmake  Pinned third-party libraries
src/
  app/                   Entry point, native bootstrap, ABI and shared runtime
  config/                Configuration, endpoint rules and validation
  graphics/              Direct3D bridge, render scaling and screenshots
  input/                 DirectInput and keyboard/mouse fallback
  platform/              Windows audio, desktop, file, network and patch hooks
  textures/              Replacement packs, manifests and texture hooks
  ui/                    Game window, preferences and ImGui settings
tests/                   Matching subsystem folders; Python tool tests in tools/
tools/                   Build/runtime helpers and asset preparation scripts
docs/                    Architecture, protocol research and validation
```

The root `CMakeLists.txt` selects the toolchain and build options;
`src/CMakeLists.txt` owns loader targets and `tests/CMakeLists.txt` owns tests.
Headers live beside their implementations and includes name their subsystem.
See [architecture](docs/ARCHITECTURE.md) and [C++ style](docs/CPP_STYLE.md).

## Window controls

- **F10** opens/closes the Dear ImGui settings panel; Escape closes the panel.
- **Alt+Enter** toggles borderless fullscreen on the current monitor.
- **F12** saves a clean PNG at the internal rendering resolution in
  `runtime/logs/screenshots`, without desktop/window borders or the settings panel.
- Drag the edges to resize, or use the normal minimize/maximize buttons.
- The title-bar **X**, **Alt+F4**, and **Quit game** exit through the game's
  existing message loop.

The panel offers 640x480, 960x720 and 1280x960 presets and a **Keep original
4:3 proportions** toggle. **Rendering resolution > Automatic** chooses the
smallest preset that fits the monitor's 4:3 content area, capped at 4K height.
For example: 1440x1080 on a 1080p monitor, 1920x1440 on a 1440p monitor, or
2880x2160 on a 4K monitor. Manual choices are 640x480 (1x), 960x720 (720p),
1280x960 (2x), 1440x1080 (1080p), 1920x1440 (1440p), 2560x1920 (4x),
2880x2160 (4K), and 3840x2880 (supersampling). Choose a resolution,
close normally, and relaunch with `Play.cmd`; changing this setting does not
reset the graphics device during a match. The panel displays the active
resolution. New accounts default to Automatic; existing accounts keep their
previous manual choice until Automatic is selected. Automatic selects once at
startup; moving an open game to another monitor does not change its rendering
resolution. Both the game and
settings threads use ImGui's DPI-awareness helper to avoid Windows bitmap
scaling on displays with increased text/UI size.

The **Screenshots** tab shows capture status and offers **Save screenshot and
return**. It queues a capture of the next game frame and closes the panel.
Capture failures are reported there and in the loader log. Filenames are unique;
existing screenshots are never overwritten. Captures are opaque PNGs and retain
the internal 4:3 image even when the visible window is stretched or letterboxed.

The 4K preset fills the height of a 3840x2160 display while preserving the
game's 4:3 composition. Keep **original 4:3 proportions** enabled to avoid
stretching cards and text across a widescreen or ultrawide monitor.

Higher scales rasterize the game's geometry at the selected resolution before
fitting the image into the window. They preserve the original 640x480 layout;
mouse mapping follows the same content rectangle and ignores black bars.
Textures without an installed replacement retain their source detail. An unsupported
higher-resolution device/shader falls back to 640x480 and records the reason
in the loader log.

The [chaiNNer texture workflow](docs/texture-upscaling.md) creates separate 4x
replacement PNGs. The loader reads `runtime/textures/pack.json` by default;
`--texture-pack FILE` selects another manifest. **F10 > Artwork & text** has
independent **Enhanced cards and artwork** and **Enhanced fonts** toggles.
Both switch immediately and are remembered per account, so HD cards can be
combined with original fonts (or the reverse). The font choice affects the 42
verified font atlases; lettering embedded in menu/card images follows artwork.
Missing or invalid replacements use their originals.
The prepared local pack covers all 503 source images: 250 card illustrations,
30 portraits, 42 font atlases and 181 UI/background images. Illustrated artwork
uses the selected D (general RealESRGAN) model. Mapped Latin text, main-menu
labels, large card digits and direction arrows have crisp vector replacements;
version/copyright text, PlayOnline controls and the three glove cursors now have
separate vector replacements too. Other text/symbols use conservative filtering. Five backgrounds are processed
as joined images before splitting them back into their original tiles.
The title's reviewed D pass also sharpens its baked-in logo and subtitle.
The prepared INI selects `runtime/textures-hd-v2/pack.json`; the earlier packs
remain separate. Original archives are never rewritten. See the
[title/UI refinements](docs/title-ui-refinement.md),
[remaining visual review](docs/HD_REVIEW.md), [font work](docs/ui-texture-upscaling.md)
and [UI work](docs/ui-asset-plan.md) for coverage and limits.
Headless GPU and native-call tests cover the new loading/drawing adapters;
the complete native game still needs a user-run visual check.

Alt+Tab and Windows shortcuts remain available. The wrapper disables the
client's legacy global keyboard hook, pointer warping and focus reclaim.
The Windows cursor is hidden only over focused game content; settings, window
borders, black bars and other apps keep a normal cursor.
The panel has its own window/render loop so it can work while the original
client is inactive. It does not pause an online match.

Window size, aspect preference, fullscreen state, resolution, artwork and font choices are remembered per account
in `runtime/settings/<POL-ID>.window`. Music and effect levels remain in the
game's Options menu; the loader now enables the native sound engine at startup.

## Build and headless checks

Requires CMake 3.29+ and Visual Studio with MSVC 19.43+ (VS 2022 17.13 or newer),
the Desktop C++ workload and an x86 Windows SDK toolchain. The project requests
C++23 using MSVC's `/std:c++23preview`; x64 is deliberately rejected because the
original DLL interfaces and hooks are 32-bit. CMake downloads Dear ImGui 1.92.5 from an immutable
upstream commit and verifies its SHA256. Later builds use the cached source.
Its official Win32/DX9 backends render the settings panel; the game retains
its D3D8 renderer. CMake also fetches pinned nlohmann/json 3.12.0 for texture
pack manifests, Microsoft WIL for Windows resource ownership, and Catch2 3.16.0
for tests. Each dependency archive has a pinned SHA256; none adds a runtime DLL.
See [third-party notices](docs/THIRD_PARTY.md) and the [library review](docs/LIBRARY_REVIEW.md).

```powershell
cmake -S . -B build -A Win32
cmake --build build --config RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo --output-on-failure
```

Catch2 cases are discovered individually by CTest, retain assertions in optimized
builds, and report unavailable audio/graphics devices as skipped. For example,
`ctest --test-dir build -C RelWithDebInfo -L texture_hooks --output-on-failure`
runs the guarded x86 call-site tests. Use `-DBUILD_TESTING=OFF` when configuring
a loader-only build; Catch2 is then neither downloaded nor linked. For an isolated
build that leaves the deployed loader alone, use `-B build-modernize` in the same
commands. The checked-in `.clang-tidy` rules enforce const local bindings and
trailing returns on non-void functions, and reject C-style casts, `NULL` and
`typedef`. Native signatures retain their Windows calling conventions and types.
The loader also uses the LandSandBoat clang-format 22 rules. Run
`py tools/run_clang_format.py --check` to verify first-party source and test
formatting, or omit `--check` to apply it. See [C++ style](docs/CPP_STYLE.md) for
the declaration/header layout and CMake format targets.

Python asset-tool tests are discovered with
`python -m unittest discover -s tests -p "test_*.py"`. Use the asset-processing
environment described in [texture upscaling](docs/texture-upscaling.md), which
contains NumPy, Pillow and OpenCV; these are not loader runtime dependencies.

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

The known DLL SHA256 hashes are pinned in `src/config/validation.cpp`. Unknown builds
are rejected before loading; fixed offsets require the matching original
builds. The loader executable does not discover installed files or copy assets.
The optional `python tools/prepare_runtime.py --source PATH` setup helper copies
a prepared user-owned runtime into ignored `runtime/`, labels that new working
copy for the existing low-integrity client, and writes local configuration.
It refuses existing destinations and never launches the game.
Legacy runtime paths must fit MAX_PATH and the client uses ANSI file
APIs internally.

Copy `tmloader.example.ini` to the ignored `tmloader.ini`, then set runtime-dir
to the external directory. Paths for game/core/log/texture-pack are relative to runtime-dir
unless absolute. A relative runtime-dir in a config is resolved beside that
config. Command-line settings override config entries.

Log destinations must remain inside `runtime-dir/logs`; the default is
`logs/tmloader.log`. F12 screenshots use `logs/screenshots/*.png`; diagnostic
captures use `logs/captures/frame_NNNNN.bmp`. Canonical
paths escaping that tree, paths inside data/pub, input DLL aliases and existing
hard-linked output files are rejected. These directories are created only
during an actual launch, not by `--check`.

This command is read-only: it verifies config, DLL hashes and representative
required assets, and any installed texture pack's paths/dimensions/hashes,
without loading DLLs, creating windows or connecting:

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

For a client-only unzip-and-run app that connects to a separately deployed server, see
[portable packaging](docs/DISTRIBUTION.md). Original client binaries, media and
HD replacements are excluded by default and have separate inclusion options.

The experimental [BoxedWine browser setup](docs/BROWSER.md) imports that package
and connects to the real Python server. Account registration and its original
confirmation screen are verified; browser confirmation input still needs review.

See [architecture](docs/ARCHITECTURE.md) for module responsibilities and known
debt. xiloader was a read-only architecture reference; this is not an upstream
fork and has no remote configured.
