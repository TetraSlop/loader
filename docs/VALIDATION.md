# Refactor validation

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
