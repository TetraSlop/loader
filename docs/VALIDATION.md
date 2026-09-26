# Refactor validation

Checked on2026-09-26 using Visual Studio18 2026, MSVC19.51.36248 and
Windows SDK10.0.26100.0. Configuration: x86 RelWithDebInfo, static C++ runtime.

- `cmake --build build --config RelWithDebInfo --parallel 4`: succeeded with no
  warnings in the final changed-source build.
- `ctest --test-dir build -C RelWithDebInfo --output-on-failure`: **3/3 passed**.
  Input tests use fake pointer/focus/key APIs and do not interact with the
  desktop. They cover focus/window-entry/acquisition discontinuities, scaled
  movement, absolute virtual-cursor alignment, keyboard-animation handoff,
  held/new clicks and the one-sample short-click latch.
- Config tests cover defaults and CLI/config precedence, unsupported/missing
  fields, endpoint syntax/ports, complete ppNNN alias family, path boundaries,
  reserved profile-port ambiguity, credential shape and a known SHA256 vector.
  Temporary test config files are created only inside the build directory and
  removed by the test.
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
