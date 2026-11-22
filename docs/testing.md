# Testing and validation

Automated tests run headless: they never launch the game, contact a server,
open visible windows or inject desktop input. Behavior that depends on the
running game is covered by the manual checks below.

## Running the tests

```powershell
cmake -S . -B build -A Win32
cmake --build build --config RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo --output-on-failure
python -m unittest discover -s tests/tools -v
```

CTest discovers each Catch2 case individually and adds its tags as labels, so
`-L hosted` or `-LE graphics-device` select or exclude a group.
Assertions stay active in optimized builds. Tests that need an audio or
graphics device report as skipped when none is available. Configure with
`-DBUILD_TESTING=OFF` for a loader-only build; Catch2 is then not fetched.

The Python tests need the packages in `tools/ci_requirements.txt` (NumPy,
Pillow, OpenCV). The loader does not depend on them.

## C++ tests

| Area | Tests | Coverage |
|---|---|---|
| app | `abi_tests`, `runtime_tests` | COM slot dispatch against a fully named mock Viewer vtable (including the SetAreaCode slot 27 versus 25 regression), launch descriptor layout, low-integrity labels on writable folders |
| hosted | `hosted_tests` | PKCE (RFC 7636 vector), the loopback sign-in callback (code, error, other paths, cancellation, timeout), server/token/play-session responses and error codes, origin rules, Credential Manager round trip. The hidden `[.gateway]` case runs against a live gateway through `tests/hosted/gateway_harness.py` (see [hosted play](hosted.md)) |
| config | `config_tests` | Defaults, CLI-over-file precedence, duplicate and unknown keys, endpoints and ports, `ppNNN` hostname aliases, path boundaries, account ID and credential shape, SHA256 |
| graphics | `graphics_tests`, `render_scale_tests`, `screenshot_tests` | Device hooks, presentation and reset, scale shader and vertex-buffer dispatch, pixel-exact comparison of scaled rendering against a CPU-scaled reference at every preset, automatic resolution selection, PNG capture, row pitch, filename collisions and low-integrity writes |
| input | `input_bridge_tests` | Focus and acquisition transitions, scaled and letterboxed mouse mapping, short-click latch |
| platform | `audio_tests`, `desktop_tests`, `patch_tests`, `virtual_registry_tests` | DirectSound 8 buffer creation with `IDirectSoundBuffer8`, import replacement policy, guarded cursor patch, call redirection, registry values and buffer contracts |
| textures | `texture_pack_tests`, `texture_replacements_tests` | Manifest hashes, containment and dimensions, HD binding and restoration, font atlas classification, COM lifetime across reset |
| ui | `settings_ui_tests`, `window_messages_tests`, `window_settings_tests` | Settings panel layout at 100, 150 and 200% scaling, window message routing (close, Alt+F4, focus reclaim, cursor), JSON preference parsing and fallbacks |

The hosted and textures tests are built only when their feature is
enabled (see [optional features](architecture.md#optional-features)).

Tests tagged `graphics-device` render through a real Direct3D 8 device and are
excluded from hosted CI; run them locally. The audio test needs a Windows audio
driver and never starts playback.

## Python tests

`tests/tools/` covers the texture extraction tools, cursor and title glyph
reconstruction, and release packaging (archive layout and loader header
checks). `python tools/build_release.py --loader build/RelWithDebInfo/tmloader.exe`
additionally builds both archives and runs `tmloader.exe --check` in the
extracted full archive.

## Manual checks

Run these against a real server after changes that affect the game at runtime.

1. The title screen appears, login succeeds and a room loads.
2. A CPU match and an online match complete, and the client reconnects to the
   room afterward.
3. Resize, maximize, Alt+Enter, Alt+Tab and Windows shortcuts work; the game
   does not steal focus; Alt+F4 and the close button quit.
4. The pointer stays aligned after resizing and in fullscreen, clicks in black
   bars are ignored, and short clicks register.
5. F10 opens the settings panel with a visible cursor; clicks in the panel
   never reach the game.
6. Select a higher resolution, relaunch, and check the title, menus, cursor,
   cards, board and transitions. The panel and log report the active
   resolution. Repeat with Windows display scaling above 100%.
7. Toggle enhanced artwork and fonts independently in the collection, shop and
   a match; the log reports indexed images and uploads.
8. F12 saves a PNG to `logs/screenshots`; the Screenshots tab reports the path.
9. Music and effects are audible and follow the in-game volume settings.

## Limitations

- Headless graphics tests draw synthetic geometry. Screens, animations and
  text effects in the running game are only covered by the manual checks.
- Custom server addresses and ports are configurable but have seen far less
  use than the default loopback setup.
