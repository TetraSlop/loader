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
`-L config` or `-LE graphics-device` select or exclude a group.
Assertions stay active in optimized builds. Tests that need an audio or
graphics device report as skipped when none is available. Configure with
`-DBUILD_TESTING=OFF` for a loader-only build; Catch2 is then not fetched.

The Python tests use only the standard library.

## C++ tests

| Area | Tests | Coverage |
|---|---|---|
| app | `abi_tests`, `runtime_tests` | COM slot dispatch against a fully named mock Viewer vtable (including the SetAreaCode slot 27 versus 25 regression), launch descriptor layout, low-integrity labels on writable folders |
| config | `config_tests` | Defaults, CLI-over-file precedence, duplicate and unknown keys, endpoints and ports, `ppNNN` hostname aliases, path boundaries, account ID, password and server rules, the local-account defaults, saving account values back through CLI11's quoting, SHA256, `retail-files.txt` parsing and the missing, wrong-hash and changed-settings reports |
| graphics | `graphics_tests`, `render_scale_tests`, `screenshot_tests` | Device hooks, presentation and reset, scale shader and vertex-buffer dispatch, pixel-exact comparison of scaled rendering against a CPU-scaled reference at every preset, automatic resolution selection, PNG capture, row pitch, filename collisions and low-integrity writes |
| input | `input_bridge_tests` | Focus and acquisition transitions, scaled and letterboxed mouse mapping, short-click latch |
| login | `login_tests` | Base64, checksums, MD5, Blowfish, crypt64 framing and RSA against vectors computed with tmserver's `wire.py`; RSA key generation; `LOGIN` against an in-process server (created, signed in, 901 codes, mismatched or malformed replies, dropped connections); a readable message for every 901 code |
| platform | `audio_tests`, `desktop_tests`, `patch_tests`, `virtual_registry_tests` | DirectSound 8 buffer creation with `IDirectSoundBuffer8`, import replacement policy, guarded cursor patch, code writes on read-only pages, registry values and buffer contracts |
| ui | `settings_ui_tests`, `window_messages_tests`, `window_settings_tests` | Settings panel layout at 100, 150 and 200% scaling, window message routing (close, Alt+F4, focus reclaim, cursor), JSON preference parsing and fallbacks |

Tests tagged `graphics-device` render through a real Direct3D 8 device. CI
skips them; run them locally. The audio test needs a Windows audio driver and
never starts playback.

The hidden `[.live]` case logs in to a real server, for example a local
tmserver:

```powershell
$env:TMLOADER_LOGIN_SERVER = '127.0.0.1:51241'
$env:TMLOADER_LOGIN_POL_ID = 'ABCDEFGH'
$env:TMLOADER_LOGIN_PASSWORD = 'a password'
.\build\tests\Release\login_tests.exe "[.live]"
```

## Python tests

`tests/tools/` covers release packaging (archive layout and loader header
checks), `retail-files.txt` against the pinned DLL hashes, and
`tools/prepare_runtime.py`. `python tools/build_release.py --loader build/RelWithDebInfo/tmloader.exe`
also builds the archive and runs its `tmloader.exe --check` against
placeholder DLLs.

## Manual checks

Run these against a real server after changes that affect the game at runtime.

1. With no account in `tmloader.ini`, the account window opens. A new POL ID
   reports "created", the same one again "signed in", and a wrong password,
   a reserved POL ID or an unreachable server show their own messages. The
   account is saved to `tmloader.ini` only after the server accepts it.
2. The title screen appears, login succeeds and a room loads, both with a
   `LOGIN` account and with `local-account=true` on a local server.
3. A CPU match and an online match complete, and the client reconnects to the
   room afterward.
4. Resize, maximize, Alt+Enter, Alt+Tab and Windows shortcuts work; the game
   does not steal focus; Alt+F4 and the close button quit.
5. The pointer stays aligned after resizing and in fullscreen, clicks in black
   bars are ignored, and short clicks register.
6. F10 opens the settings panel with a visible cursor; clicks in the panel
   never reach the game.
7. Select a higher resolution, relaunch, and check the title, menus, cursor,
   cards, board and transitions. The panel and log report the active
   resolution. Repeat with Windows display scaling above 100%.
8. F12 saves a PNG to `logs/screenshots`; the Screenshots tab reports the path.
9. Music and effects are audible and follow the in-game volume settings.

## Limitations

- Headless graphics tests draw synthetic geometry. Screens, animations and
  text effects in the running game are only covered by the manual checks.
- Custom server addresses and ports are configurable but have seen far less
  use than the default loopback setup.
