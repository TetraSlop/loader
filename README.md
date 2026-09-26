# tmloader

A standalone 32-bit host for the original Tetra Master PC client, arranged into
small loader modules in the style of xiloader. It opens the original online
startup and connects to a separately running compatible game/profile server.

This is a compiled first refactor of the working local harness. **It has not
been launched with the game after the refactor.** The earlier harness reached
the title, room and a completed computer match. That runtime evidence does not
prove parity of this new executable; see [validation](docs/VALIDATION.md).

## Build and headless checks

Requires CMake 3.24+ and Visual Studio with the Desktop C++ workload and an x86
Windows SDK toolchain. There are no downloaded build dependencies.

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
builds. The current loader does not discover installed files or prepare/copy
assets. Legacy runtime paths must fit MAX_PATH and the client uses ANSI file
APIs internally.

Copy `tmloader.example.ini` to the ignored `tmloader.ini`, then set runtime-dir
to the external directory. Paths for game/core/log are relative to runtime-dir
unless absolute. A relative runtime-dir in a config is resolved beside that
config. Command-line settings override config entries.

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
- Process-local registry configuration, empty LOADDIR prefix, windowed640x480
  Direct3D8 rendering, first-frame activation and normal window close.
- Native DirectInput first, then the existing focus-aware keyboard/mouse
  fallback. Pointer alignment and short-click latch are retained.
- The legacy process keeps the existing reduced-integrity/DEP compatibility
  settings. Runtime writable data must already permit low-integrity writes;
  this loader does not change directory ACLs or Windows UAC settings. File
  interception is best-effort compatibility protection, not a complete sandbox.

See [architecture](docs/ARCHITECTURE.md) for module responsibilities and known
debt. xiloader was a read-only architecture reference; this is not an upstream
fork and has no remote configured.
