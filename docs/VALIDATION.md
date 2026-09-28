# Refactor validation

## Portable app packaging, 2026-09-27

`tools/package_app.py` produced an 11.2 MiB default Windows ZIP and a 219.3 MiB
local opt-in bundle with original client DLLs, data/audio and the current HD pack.
Both contain isolated Python 3.13.15, current tmserver runtime files and the
Release x86 loader. No native game window was opened by these packaging checks.

- Eight distribution boundary tests pass: default omission, independent flags,
  original/factory setting selection, texture path/hash validation, private
  metadata removal, refusal to overwrite changed preferences, junction rejection,
  pinned Python checksum and archive inventory checks.
- `tools/check_portable.py` passes for both extracted variants: embedded imports,
  all three server data resources, CLI help/account listing and both services on
  ephemeral ports. Disposable saves/logs stay outside the extracted app; package
  files remain unchanged.
- The full package passes `distribution.app check` and `play --check`, including
  native loader and all HD texture checks, with unchanged contents and integrity
  labels. Neither command launches the game or loads its DLLs.
- A fresh default package imported the local originals and HD manifest, used
  the factory Viewer option template, then passed loader preflight. Source DLLs
  and existing Viewer preferences remained unchanged; no account/save/settings
  directories were copied.
- ZIP inventory and every recorded size/SHA256 match `package-manifest.json`.
  The default has no `client/runtime/` files. Neither ZIP contains prior saves,
  logs or wrapper settings. The exported HD manifest has no build-machine paths.
- New Python sources pass Ruff. The server's portable launcher changes pass
  32 focused tests, including real encrypted headless peers.

These checks establish packaging and setup behavior, not native rendering or
gameplay from a freshly extracted copy. Browser runtime evidence is recorded
separately in the browser integration documentation.

## Subsystem directory layout, 2026-09-27

Source now lives in `src/app`, `config`, `graphics`, `input`, `platform`,
`textures` and `ui`, with matching C++ test directories. Python asset-tool tests
live in `tests/tools` and remain discoverable from `tests`. The root build file
keeps toolchain and project options; dependency declarations, loader targets and
test targets have their own CMake files. The loader retains its existing
`<build>/RelWithDebInfo/tmloader.exe` output path.

- A fresh MSVC x86 C++23 build in `build-layout/` passes. **24/24 CTest cases
  pass**, with no skipped cases; the asset-processing environment passes all
  **18 Python tests** after their move.
- A separate `BUILD_TESTING=OFF` build passes in `build-layout-loader-only/`
  without configuring Catch2. Both build modes retain the executable location
  and pass the PE compatibility checks.
- Comparing all 51 C++ files to the previous revision finds no changes outside
  include directives and formatting. The three Python test changes only adjust
  their import root. Dependency versions/hashes and both style configurations
  remain identical.
- All 51 C++ files pass clang-format 22. Read-only PE checks confirm x86 PE32,
  the GUI subsystem, `asInvoker` and the legacy compatibility flags.
- The deployed loader, local INI and pre-existing IDE files retain their
  baseline hashes. No native game session was started for this organization
  change.

Local evidence is in ignored `out/layout-build.log`, `out/layout-ctest.log`,
`out/layout-loader-only-build.log`, `out/layout-baseline.json` and
`out/verify_layout.py`.

## Const, trailing returns and simplification, 2026-09-27

Non-void functions now use trailing return types throughout first-party C++,
including Windows callbacks, COM overrides and native test doubles. Stable local
values and pointer bindings are const; output parameters, advancing cursors and
ownership transfers retain their required mutability. The persisted clang-tidy
rules enforce these conventions without changing pointed-to types.

The simplification pass replaces config's map of boolean markers with set
insertion, avoids a redundant trim copy, decodes the saved window-settings
version once, and shares optional boolean parsing. The settings panel separates
window lifetime from rendering and uses WRL COM owners for its graphics objects.
Virtual registry responses share their type/size/buffer handling. Callback
calling conventions, initialization order and graphics cleanup order remain.

- The isolated MSVC x86 C++23 RelWithDebInfo build passes without compiler
  warnings. **24/24 CTest cases pass**, with no skips. Three added cases cover
  registry buffer contracts and malformed/partial saved preferences.
- All **37 production and test translation units** pass the persisted
  clang-tidy checks with warnings treated as errors and no first-party
  diagnostics. All **51 C++ files** pass clang-format 22 verification.
- A standalone comparison against the committed settings parser covers
  **30,000 combinations** of versions, dimensions and optional field values.
  All parsed fields and stream states match, including malformed inputs.
- Read-only PE verification passes for the isolated executable. Hashes confirm
  the deployed loader, local INI and pre-existing IDE files remain unchanged.

Evidence is in ignored `out/cleanup-{build,ctest,format-check}.log`,
`out/const-return-audit.json`, `out/audit-*.log` and
`out/window-settings-compatibility.log`. No game or settings window was launched;
the panel lifecycle extraction has build, static-analysis and headless layout
coverage, rather than a new native interaction check.

## LSB C++ style and implementation boundaries, 2026-09-27

The loader now uses the sibling LandSandBoat repository's exact clang-format
22 configuration and the user's `lsb-cpp-style` conventions, documented in
[C++ style](CPP_STYLE.md). The formatter covers 51 first-party source and test
files. Class extraction separates DirectInput, texture attachment and screenshot
resource declarations from their implementations; private constants and related
module state have implementation-local ownership. Shared ABI constants remain
in their declaring headers.

- The isolated x86 RelWithDebInfo build in `build-modernize/` passes without
  compiler warnings. The actual compiler log confirms `/std:c++23preview` and
  `/MT`; the existing C++23 requirement and Windows calling conventions remain.
- **21/21 CTest cases pass**, with no skipped cases. These include COM dispatch,
  input transitions, graphics pixels, screenshot locking, texture reference
  lifetimes, settings and audio initialization. Input tests configure callbacks
  through the same interface used by the production fallback.
- The CMake `format-check` target passes all 51 files using clang-format 22.1.4.
  The configuration is byte-identical to LSB's `.clang-format` (SHA256
  `26e3de9a292f8f22cfae82345cfbf59baf3aea18eba17462b3f2991671981a8f`).
- Each extracted header compiles independently under x86 MSVC C++23 with
  `/W4 /WX`: `input_bridge.h`, `texture_replacement.h` and
  `screenshot_capture.h`. The Windows SDK dependency groups retain the include
  order required by BCrypt, DirectSound and Winsock.
- The read-only PE check passes for the isolated executable: x86 PE32, GUI
  subsystem, `asInvoker` manifest and preserved legacy compatibility flags.
- A review comparing both revisions after identical formatting isolates the
  structural edits from whitespace changes. GUIDs, supported DLL hashes, shader
  constants, COM method signatures and screenshot cleanup order are preserved.
  The deployed executable, local INI and pre-existing IDE files retain their
  baseline hashes.

Evidence is retained in ignored `out/style-{build,ctest,format-check}.log`,
`out/style-header-check.log` and `out/style-semantic-diff.txt`. This is a source
cleanup and headless regression check; no game was launched and the deployed
loader was not replaced. Formatting applies throughout first-party C++, while
semantic style modernization is limited to the refactored boundaries.

## Title, version, PlayOnline and cursor refinement, 2026-09-27

The selected local pack is now `runtime/textures-hd-v2/pack.json`. It contains
503 entries for 443 distinct originals, with 157,283,070 bytes of replacement
PNGs. **14 entries changed and 489 are byte-identical to the previous HD pack**:
five title tiles, on_title, and both aliases of cursor/friend2/friend3/friend4.
The sixth title tile was already the same D output. All seven original game
archive hashes still match the extraction manifest.

- The six title tiles reassemble byte-for-byte to the reviewed complete D image
  at 2560x1792. Removing the first pass's filtered-logo overlay sharpens the
  title and subtitle; no higher-resolution original artwork is claimed.
- `title_glyphs.py` rebuilds 35 audited text/icon regions. Original version
  constructor `101480F0` was executed through its final sprite registrations,
  producing the nine source/destination records for `Ver.1.06.011`. Offline
  composition uses those records; toolbar/status previews use the original
  sampled dimensions, including the extra border texel. Unaudited atlas pixels,
  beta/suffix cells, unknown-count markers and neighboring sprites remain intact.
- `cursor_art.py` reconstructs the three separate glove poses in their original
  40x40 cells. Source aliases produce identical pixels. Curves, transparent
  gutters and native ink bounds were inspected in the offline review.
- All **18 focused Python tests pass**: three cursor tests, seven title/control
  glyph tests and eight texture extraction/containment tests. The new code and
  integration pass Ruff E4/E7/E9/F and whitespace checks. A complete repeated UI
  build produces the same hashes for all 211 UI/portrait outputs, confirming
  that cached reconstruction does not accumulate changes.
- The existing deployed loader's read-only `--check` accepts all 443 indexed
  source images, physical dimensions and file hashes. `Play.cmd --check` passes
  with the updated INI and starts neither services nor game. No C++ code or
  executable changed, and no new whole CTest run is claimed for this asset pass.

Logs are `out/ui-refinement-{build,repeat-build,assembly,validation}.log`,
`out/ui-refinement-loader-stdout.log` and `out/ui-refinement-local-play-check.log`.
The detailed changed-image/archive audit is `out/ui-refinement-validation.json`.
Manifest SHA256:
`3e143f79cba6ae75643dce12dd8401a46a99f2ee3fc6b0fb01a21aec79f91188`.
Reviewed before/after sheet: `out/ui-refinement-before-after.png`; complete
version/toolbar layout: `out/texture-lab/variants/title-control-review/native-layout-comparison.png`.
These are offline comparisons, not new game screenshots. Native visual review
remains a user check after relaunch. Current window/rendering preferences and
all original archives remain unchanged; no desktop or input control was used.
See [treatment and reproduction](title-ui-refinement.md).

## C++23, Catch2 and Windows resource ownership, 2026-09-27

Built in the isolated `build-modernize/` directory with CMake 4.4.2, MSVC
19.51.36248 and the x86 Windows SDK 10.0.26100. The compiler command uses
`/std:c++23preview` and `/MT`. The deployed loader was not replaced.

- **21/21 CTest cases pass**, with no skipped audio or graphics checks. All
  15 native test executables use Catch2 3.16.0; CTest discovers named cases and
  tags. Existing assertions remain active in RelWithDebInfo without `NDEBUG`
  overrides. Graphics/audio absence uses Catch2 `SKIP` and CTest's matching
  return code. The CLI help check remains a separate CTest case.
- COM numerical dispatch, calling conventions, native synthetic call-site
  execution, DirectInput mapping, window/input policies, guarded texture hooks,
  rendering pixels, texture lifetime and all three settings tabs pass.
- C++23 UTF-8 changes are handled explicitly: PNG status text remains UTF-8,
  and JSON texture paths use `char8_t` filesystem construction. Added coverage
  for accented/Japanese texture filenames; screenshot writes and its disposable
  low-integrity child now use a non-ASCII fixture directory.
- Microsoft WIL owns the CNG hash/algorithm handles, token/SID allocations and
  temporary output-validation file handles. Cleanup uses the correct deleters;
  error handling, path boundaries and legacy module/object lifetime are retained.
- PE inspection confirms x86 PE32, GUI subsystem, `asInvoker` and the previous
  DEP/ASLR compatibility flags. No game, desktop/input control, live save or
  original asset changes are part of these checks.
- All 21 production translation units pass the checked-in clang-tidy rules for
  C-style casts, `NULL` and `typedef`, including project headers and excluding
  dependency sources. The audit uses Clang's C++23-capable latest mode; the
  shipping compiler remains MSVC. A separate `BUILD_TESTING=OFF` configuration
  confirms that Catch2 is omitted.

CMake now requires 3.29 for WIL's official integration; MSVC 19.43+ provides
the selected C++23 preview mode. This is compiler-supported C++23, not a claim
that MSVC has finalized the complete C++23 ABI. The original game boundaries
remain explicitly 32-bit Windows types and calling conventions.

The project sources build without warnings; the pinned Catch2 sources emit
MSVC C4324 alignment-padding warnings. Third-party code was not edited for style.
See [library choices](LIBRARY_REVIEW.md) and [licenses](THIRD_PARTY.md).

## Automatic resolution, independent fonts and screenshots, 2026-09-27

The wrapper now chooses rendering resolution from physical monitor dimensions,
offers independent artwork/font toggles, and saves user-requested F12 PNGs.
Manual resolution choices remain available. Preferences v5 preserve older
manual settings and the previous HD choice for both new categories.

- The x86 RelWithDebInfo build succeeds; the changed-source rebuild is warning-free.
  **16/16 CTest cases pass**, with no skipped graphics or audio checks.
- Actual D3D8 colored/textured output matches the independent reference with
  zero RGB differences at all eight presets, including new 720p and 1080p heights.
- Monitor selection covers widescreen, ultrawide, portrait, 4K and missing-display
  fallbacks. Settings tests cover migration, independent flags and manual override.
- Font/artwork checks cover all four combinations, exact atlas classification,
  original metadata/bindings, COM lifetime and device Reset. The installed
  503-entry pack has 42 font atlases and no cross-category source-hash conflicts.
- Screenshot tests encode and decode actual PNGs through WIC, verifying exact
  colors, opaque output, row pitch, filename collisions, disk-write failures,
  resource cleanup and request ordering. A disposable low-integrity child verifies
  PNG writes in a low-labeled fixture and denial in a medium-labeled control.
- F12 key repeats/keyup are consumed without forwarding to the game; keyboard
  samples reserve F10/F12 and still deliver ordinary keys.
- All three ImGui tabs fit at 100/150/200% scaling, including long capture status.
  Short panels now expose a vertical scrollbar so the final controls stay reachable.
- PE/asInvoker verification, installed-pack hash validation and the sibling
  launcher's read-only `Play.cmd --check` pass. No game or services were launched.

The prepared primary account now selects Automatic and both enhanced artwork
and fonts, preserving its window size/mode and stored 4.5x manual choice.
Native visual/input review, especially Windows DPI scaling, remains a user check.
The tests create no visible windows, perform no desktop capture and inject no input.

## HD artwork and 4K rendering, 2026-09-27

The loader now substitutes separate HD resources while keeping the native
texture objects and logical dimensions intact. Seven verified native upload
CALL sites are guarded before patching. Draws temporarily bind the matching
HD texture and restore the native binding afterwards. Original PNG hashes
ignore only archive alignment padding after IEND. Invalid/missing replacements
fall back to the original texture. No original asset archive is rewritten.

- **15/15 CTest cases passed**, including actual Direct3D8 checks under low
  integrity. No game code, visible window, Present, focus or input control is
  used by those graphics tests.
- Colored/textured geometry matches the independent CPU-scaled reference at
  all six resolutions, including **2880x2160 (4.5x)** and **3840x2880 (6x)**.
  All twelve comparisons produced **zero RGB differences** on this machine.
- Replacement checks cover original GetLevelDesc preservation, HD detail,
  partial alpha, padded non-power-of-two UV crops, toggle/fallback, restored
  native bindings and COM ownership. Managed replacement survival and two
  complete DEFAULT-resource release/reset/reload cycles pass.
- A separate font-shaped fixture exercises the original 18px cell layout,
  17-texel UV spans on 16-unit glyph quads, alpha floor 1, linear filtering,
  colored/translucent text and a shadow pass. HD substitution plus the scale
  shader matches independently scaled/directly bound rendering with zero RGB
  differences at both 1440p and 2160p. This validates the drawing contract;
  it does not identify the original typeface or verify every native text effect.
- Synthetic x86 upload calls verify the fifteen-argument stdcall adapter,
  original return values, byte guards and rollback. Manifest tests cover
  source/replacement hashes, containment, dimensions and corrupt files.
- Saved v1-v3 preferences remain readable; v4 supports fractional scales.
  Window-message tests save 4.5x and reject invalid preset indices. The ImGui
  layout remains within its panel at 100/150/200% DPI.
- Review found a repeated-device window-subclass recursion hazard. Window
  initialization is now idempotent for an already adapted HWND; a regression
  check verifies that the real native forwarding procedure remains intact.

The complete native game still requires a user-run visual check. In particular,
headless GPU tests do not establish that every screen, animated sprite or font
effect looks correct. After launch, the log should report the active resolution,
indexed source-image count and individual HD uploads; F10 provides an immediate
original/HD comparison. Remaining visual review items are recorded with the
asset pack documentation.

The installed `runtime/textures-hd/pack.json` combines **503 entries covering
443 distinct source PNGs**, with 157,610,541 bytes of replacement PNG data.
The assembler verified every source/output hash, decode and 4x dimension;
the native loader's read-only check verified its complete unique-image index.
All seven original archive hashes still match the extraction manifest.
The rebuilt executable passes PE32/asInvoker checks, and the sibling launcher's
`Play.cmd --check` passes without starting services or the game. The prepared
account selects 4.5x, 4:3 proportions and HD artwork; window mode is preserved.

## Higher internal resolution, 2026-09-27

The user confirmed sound works after the DirectSound 8 correction and requested
sharper internal rendering. The new F10 setting selects 1x/2x/3x/4x rendering,
applied at the next launch, independently of window size. Existing preferences
retain 1x. Original textures are unchanged.

- The separate Direct3D 8 graphics check draws colored and textured geometry
  through an actual dynamic vertex buffer at all four resolutions, with varied
  reciprocal W, depth, alpha, and partially offscreen/subpixel coordinates.
  It compares the scale shader against independently CPU-scaled fixed-function
  rendering. All eight comparisons produced **zero RGB differences** on this
  machine. Back-buffer dimensions are checked before pixels are read.
- This check creates no window, calls no Present/focus/input API, and loads no
  game code. It runs under the same low-integrity process setting as the loader.
  Its initial failure exposed D3D8's incompatible XYZRHW buffer tag; the
  production CreateVertexBuffer adapter now clears that tag at higher scales.
- Unit checks cover shader/buffer dispatch, back-buffer dimensions, reset
  recreation, source rectangle scaling, v1 preferences and invalid scale values.
  The full ImGui panel produces draw lists without overflow at 100/150/200% DPI.
- **12/12 CTest cases passed**. The x86 RelWithDebInfo build completed without
  warnings. PE/asInvoker validation and the launcher's read-only setup check
  verify the rebuilt executable and prepared runtime.

Native check: launch the updated loader, open F10, select 2x or higher, close
normally, and relaunch. Enlarge/maximize the window and confirm title/menu,
cursor, cards, board and transitions fill the same layout correctly. The panel
and log should report the chosen active resolution. Full native higher-resolution
presentation is pending this user check; the graphics test is not a game run.

## DirectSound 8 correction, 2026-09-27

The user confirmed the desktop behavior works but audio remained silent. Their
`20260927-074040-b6ca9e` session initialized the sound engine successfully and
saved both Se=3 and Bgm=3 (High). The audio assets were present. The loader's
diagnostic hook had incorrectly replaced DirectSoundCreate8 with DirectSoundCreate.

The original protected DLL imports dsound ordinal 11 (DirectSoundCreate8).
Decoded music and effect creation paths both query IID_IDirectSoundBuffer8
(`6825A449-7524-4D82-920F-50E36AB3AB1E`). A silent Windows device regression
check reproduced the error before the fix: factory and buffer creation returned
S_OK, but that QueryInterface returned **80004002 / E_NOINTERFACE**. The corrected
factory and SDK signature preserve DirectSound 8.

- **11/11 CTest cases pass**, including actual Windows music/effect buffer
  creation, required interface acquisition, Lock/Unlock and stopped status.
  The passing device test uses the same low-integrity process setting as the
  loader, creates no windows and never requests playback. On machines without
  an audio driver, CTest explicitly skips this device-dependent case.
- The x86 RelWithDebInfo build, PE/asInvoker verification and sibling
  `Play.cmd --check` passed. The launcher resolves the rebuilt executable.
- No native game was launched or controlled, and no save/assets were changed.

The interface failure was reproduced and corrected. The user subsequently
confirmed sound works. The buffer check alone does not establish speaker output
or validate every native decoder path.

## Desktop focus/cursor correction, 2026-09-27

The user's next run revealed persistent foreground/input grabbing and an
invisible settings cursor. The previous window-command checks did not cover
the game's global keyboard hook, per-frame cursor synchronization or custom
focus-reclaim message. Those legacy paths are now disabled in the loader.

- **10/10 CTest cases pass**, including game-import policy and guarded in-memory
  cursor patch checks. Window tests now cover the suppressed focus-reclaim
  message, cursor visibility in settings/inactive game/outside content, and
  delegation of border cursors to Windows. ImGui tests use the production panel
  configuration and check that cursor updates remain enabled.
- Eight original-code cursor cases were executed in Unicorn with fake OS APIs.
  The old code warped or clamped the pointer; the patched block preserved the
  software cursor and stack with no OS calls. Results are retained in ignored
  `build/desktop-original-code-check.json`.
- User-run log `20260927-073257-c5ea12/player-1.log` records DirectSoundCreate=0,
  native sound initialized=1, and GameStart returned=0 after a close request.
  These establish initialization/exit, not audible playback or full UI parity.

No running game was manipulated or relaunched by the agent. The new native
check is F10 with a visible pointer, Alt+Tab/Windows shortcuts, settings controls
without focus theft, and pointer alignment after returning to the resized game.

## Desktop usability update, 2026-09-27

The user exercised the integrated build and reported silent audio, no resizing
and no window close. Static inspection of the decoded supported client confirmed
the SOUND startup gate and its fullscreen window procedure's suppression of
WM_SYSCOMMAND. The new loader enables native audio, restores desktop controls,
scales presentation/input together, and adds an F10 Dear ImGui panel.

- x86 RelWithDebInfo build passed without compiler warnings.
- **9/9 CTest cases passed.** New checks exercise actual window-message routing
  (including X/Alt+F4 exit requests), virtual SOUND registry semantics, D3D8
  presentation/reset arguments, shared viewport geometry and settings parsing.
- Existing input tests now cover letterboxing, discarded black-bar clicks and
  isolation of the settings window from game input. Path tests cover the new
  settings output tree and hard-linked asset aliases.
- The real ImGui panel built nonempty draw lists without scroll overflow at
  100%, 150% and 200% scale, without creating a window or graphics device.
- PE/asInvoker verification and the integrated read-only setup check passed.

No native game or settings window was launched by the agent. Audible output,
actual GPU presentation, live settings interaction, and native teardown still
need the user checks in the sibling server's NATIVE_TEST_CHECKLIST.md. A passing
mock/regression check is not a claim that those native checks have passed.

## Integration setup, 2026-09-27

The sibling `tmserver` now supplies `Play.cmd`, coordinating both loopback
services and one to three user-operated clients. It passes selected ports and
account IDs explicitly. `tools/prepare_runtime.py` prepared the ignored local
`runtime/` copy and `tmloader.ini`; 133 files matched the original source.

The existing C++ executable rebuilt successfully; all four CTest cases and the
read-only PE/asInvoker verification passed again. Its actual `--check` command
accepted the prepared runtime and each generated account/endpoint command line.
The agent did not launch the game. Native presentation remains awaiting the
user's [integrated checklist](../../tmserver/docs/NATIVE_TEST_CHECKLIST.md).

## Original refactor checkpoint

Checked on2026-09-26 using Visual Studio18 2026, MSVC19.51.36248 and
Windows SDK10.0.26100.0. Configuration: x86 RelWithDebInfo, static C++ runtime.

- `cmake --build build --config RelWithDebInfo --parallel 4`: succeeded with no
  warnings in the final changed-source build.
- `ctest --test-dir build -C RelWithDebInfo --output-on-failure`: **4/4 passed**.
  Input tests use fake pointer/focus/key APIs and do not interact with the
  desktop. They cover focus/window-entry/acquisition discontinuities, scaled
  movement, absolute virtual-cursor alignment, keyboard-animation handoff,
  held/new clicks and the one-sample short-click latch.
- Config tests cover defaults and CLI/config precedence, unsupported/missing
  fields, endpoint syntax/ports, complete ppNNN alias family, path boundaries,
  reserved profile-port ambiguity, credential shape and a known SHA256 vector.
  Temporary test config files are created only inside the build directory and
  removed by the test. Additional fixtures verify that data/gW201.dat,
  pub/home00/option.bin, DLL paths, directory traversal and hard-linked capture
  aliases are rejected without changing any file's contents.
- The COM ABI regression test uses a full named mock vtable, independent of
  numerical loader indices, to verify SetAreaCode27, SetParamInit5 and
  GetCommonFunctionTable7, their arguments/output pointer and the launch
  descriptor ABI. It demonstrates that the old slot25 reaches the wrong
  method. No native DLL or COM server is loaded for this test.
- `python -B tools/verify_build.py`: x86 PE32, Windows GUI subsystem,
  embedded asInvoker/uiAccess=false manifest and preserved legacy DEP/ASLR
  compatibility flags verified by reading the built executable.
- The executable's `--check` mode returned0 against the existing external
  user-owned runtime. It verified both original DLL hashes and representative
  required asset files. It did not call LoadLibrary, initialize a game session,
  create a window, connect to a server or modify runtime files.

No game was launched, no desktop input/window control was used and no existing
process was terminated. Source DLLs/assets were not copied into this repository.
Existing harness/server/repositories were not edited. The repository has no
remote and nothing was pushed.

## What remains unverified

The new module boundaries, config paths, direct startup ordering and graphics
hooks still need a later user-authorized native run. Headless tests cannot
prove a rendered title, room entry, a match, post-match reconnection or shutdown.
The preserved short-click latch and pp001 alias fix were built after the
previous native run and have no subsequent native confirmation. Custom remote
IPv4 endpoints/ports are configurable but have no native validation. This first
pass intentionally keeps the previously working fixed-offset mechanisms and
documents their limits rather than declaring the loader fully stable.
