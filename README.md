# tmloader

tmloader is a standalone 32-bit Windows host for the original Tetra Master PC
client. It loads the game DLL (`TM_native.dll`) and the Viewer DLL
(`polcore_native.dll`) directly, without PlayOnline, and connects them to a
server. It does not include one; you need a running
[tmserver](https://github.com/TetraSlop/server) to play.
[TetraPackets](https://github.com/TetraSlop/packets) documents the protocol
between the client and the server.

tmloader is an unofficial fan project. It is not affiliated with, endorsed by
or sponsored by Square Enix.

## Features

- Windowed, resizable and borderless fullscreen play with normal desktop
  controls (Alt+Tab, Alt+F4, minimize and maximize).
- Higher internal rendering resolutions, from the original 640x480 up to
  3840x2880, with the original 4:3 layout preserved.
- An F10 settings panel and F12 PNG screenshots at the internal resolution.
- Sound through the game's own DirectSound 8 engine.
- Input without a global keyboard hook, pointer warping or focus stealing.
- Pinned DLL hashes and a read-only `--check` mode that validates a setup
  without launching the game.

## Requirements

- Windows 10 or 11. The loader is a 32-bit (x86) process.
- Your own PlayOnline install of Tetra Master, with the game DLL, Viewer DLL
  and game data listed in [`retail-files.txt`](retail-files.txt). Only the DLL
  builds whose SHA256 hashes are pinned in `src/config/validation.cpp` are
  accepted. See [game files](#game-files).
- A reachable tmserver, by IPv4 address or host name, and an account on it
  (see [accounts](#accounts)).

Building from source also needs CMake 3.29+, Visual Studio 2022 17.13+
(MSVC 19.43+) with the Desktop C++ workload and an x86 Windows SDK.

## Quick start

### Release download

[Releases](https://github.com/TetraSlop/loader/releases) provide
`tmloader-VERSION-windows-x86.zip`: `tmloader.exe`, a sample `tmloader.ini`,
`retail-files.txt` and license notices. It contains no game files.

1. Extract the archive to a short, writable path such as `C:\Games\TetraMaster`.
2. Create `runtime/` beside `tmloader.exe` from your own install (see
   [game files](#game-files)).
3. Run `tmloader.exe`. On first start it asks for a username, password and
   server (see [accounts](#accounts)). `tmloader.exe --check` validates the
   setup without starting the game.

See [distribution](docs/distribution.md) for the layout and how the archive is
built.

### Build from source

```powershell
cmake -S . -B build -A Win32
cmake --build build --config Release
.\build\Release\tmloader.exe --config .\tmloader.ini --check
```

`py tools/prepare_runtime.py` builds the ignored `runtime/` folder from your
own install and writes a `tmloader.ini` for a local server (see
[game files](#game-files) and [a local server](#a-local-server)). Start the
server, then run the loader without `--check`.

## Game files

tmloader does not include any game files. They come from your own PlayOnline
install of Tetra Master, and [`retail-files.txt`](retail-files.txt) lists each
one the runtime folder needs, with its SHA256 and where it comes from in a
standard install.

`tmloader.exe --check` reads `retail-files.txt` from beside `tmloader.exe` and
hashes every listed file in the runtime folder. It names each file that is
missing or has the wrong hash, and fails if there are any. A changed
`pub/home00/option.bin` is only a warning, because the Viewer rewrites it when
settings are saved. Without `retail-files.txt` beside it, `--check` verifies only
the two DLL hashes and the files the loader opens itself. Release archives and
source builds both put the list beside the executable. Outside the loader, run
`sha256sum -c ../retail-files.txt` from the runtime folder.

In a source checkout, `tools/prepare_runtime.py` copies them for you. Pass the
Tetra Master game folder and the PlayOnline Viewer folder:

```powershell
py tools/prepare_runtime.py "C:\...\PlayOnline\SquareEnix\TetraMaster" --viewer "C:\...\PlayOnline\SquareEnix\PlayOnlineViewer"
```

or a folder already laid out as a runtime:

```powershell
py tools/prepare_runtime.py D:\old-runtime
```

It copies only the listed files into `runtime/`, checks each
against its hash, and refuses other DLL builds or damaged files. The Viewer
settings file `pub/home00/option.bin` is accepted with any content, because the
Viewer rewrites it when settings are saved.

## Accounts

You sign in with a username and password; the server creates the account the
first time it sees a username. Before the game starts, the loader connects to
the server's game port, exchanges a session key with it, and sends the username
and password with tmserver's `LOGIN` command. The server answers with the
account's POL ID and a one-time game password, which the loader hands to the
game in place of yours. Neither is saved.

- **First start.** With no `username` or `password` in `tmloader.ini`, a small
  window asks for the username, password and server. Once the server accepts
  them, the loader saves them to `tmloader.ini` and starts the game.
- **New accounts.** A username the server has never seen becomes a new account
  with that password, and it is also your display name. Usernames ignore case,
  so `Player` and `player` are one account. A mistyped username creates a
  second account instead of failing, so check it. The window and the settings
  panel say whether the account was created or signed in.
- **Refusals.** A wrong password (which is also what you get for a username
  that belongs to someone else), a locked account, an account already playing
  elsewhere, too many attempts or a busy server reopen the window with the
  reason.
- **Format.** A username is 1 to 15 letters, digits, `_`, `.` or `-`. A
  password is 1 to 15 printable ASCII characters; spaces are allowed. Other
  values are rejected when the config is read, because the game cannot use
  longer passwords.

The key exchange does not authenticate the server, so anyone who can intercept
the connection can read the password. Use a password you use nowhere else.

### A local server

A tmserver on the same machine also has built-in test accounts that skip
`LOGIN`. To use one, set `local-account=true` with a loopback server:

```ini
runtime-dir=runtime
server=127.0.0.1
local-account=true
```

This logs in as POL ID `EEEEEEEF` with password `tm-local-test`; set `pol-id`
and `password` to use another built-in account. `tools/prepare_runtime.py` writes
this config.

## Configuration

Settings come from a `key=value` file: `--config FILE`, or `tmloader.ini` next
to the executable when `--config` is omitted. Command-line options override it.
Paths are relative to `runtime-dir` unless absolute, and a relative
`runtime-dir` in a config file is resolved beside that file.

| Option | Default | Description |
|---|---|---|
| `--config FILE` | | Read settings from a `key=value` file |
| `--runtime-dir DIR` | required | Runtime directory holding the DLLs and data |
| `--server HOST` | `127.0.0.1` | Server IPv4 address or host name |
| `--game-port PORT` | `51241` | Game port |
| `--profile-port PORT` | `51220` | Profile port |
| `--username NAME` | | Account username; asked for when unset |
| `--password TEXT` | | Account password, 1 to 15 characters; asked for when unset |
| `--local-account` | off | Use a local server's built-in test account, without `LOGIN` (see [a local server](#a-local-server)) |
| `--pol-id ID` | | A built-in test account's POL ID, with `--local-account` |
| `--diagnostics` | off | Extra logging, and screenshots at fixed startup frames |
| `--check` | | Validate settings, DLL hashes and every file in `retail-files.txt`, then exit |
| `--help`, `-h` | | Show help |
| `--version` | | Show the version |

`tmloader.ini` holds the password in plain text, so keep it out of version
control. The loader never logs passwords.

The runtime directory contains:

```text
TM_native.dll            game DLL
polcore_native.dll       Viewer DLL
data/                    game data
data/utf/env.dat         Viewer environment data
pub/home00/option.bin    Viewer settings
```

Logs, screenshots and per-account preferences are written under
`runtime-dir/logs` and `runtime-dir/settings`. If the game crashes, the loader
also writes a minidump to `runtime-dir/logs/tmloader-crash.dmp`, replacing the
previous one.

## Controls

| Key | Action |
|---|---|
| F10 | Open or close the settings panel (Escape also closes it) |
| F12 | Save a PNG screenshot to `logs/screenshots` |
| Alt+Enter | Toggle borderless fullscreen |
| Alt+F4, title-bar close | Quit the game |

The settings panel selects the rendering resolution (automatic or one of eight
presets, applied at next launch) and 4:3 aspect preservation. Preferences are
saved per account. Music and effect volumes stay in the game's own Options menu.

## Project layout

```text
src/
  app/        entry point, loader, COM and Viewer ABI, logging, shared runtime
  config/     CLI and config parsing, endpoint and path rules, DLL validation
  login/      the game port's key exchange and LOGIN
  graphics/   Direct3D 8 hooks, render scaling, screenshots
  input/      DirectInput hook and mouse bridge
  platform/   file and registry boundary, network, desktop, audio, patching
  ui/         game window, preferences, ImGui settings panel, account window
tests/        Catch2 tests mirroring src/; Python tool tests in tests/tools
tools/        packaging, release and formatting scripts
docs/         documentation
```

## Documentation

| Document | Description |
|---|---|
| [Architecture](docs/architecture.md) | Startup sequence, hooks per subsystem and reverse-engineered offsets |
| [Testing](docs/testing.md) | Automated test coverage and manual in-game checks |
| [C++ style](docs/cpp-style.md) | Formatting and coding conventions |
| [Distribution](docs/distribution.md) | Building and verifying the client archives |
| [Releasing](docs/releasing.md) | Release automation and archive contents |
| [Third-party code](docs/third-party.md) | Dependencies and licenses |

## License

Copyright (C) 2026 sruon

tmloader is free software: you can redistribute it and/or modify it under the
terms of the [GNU General Public License v3.0](LICENSE), with the additional
permission in [LICENSE-EXCEPTION.txt](LICENSE-EXCEPTION.txt). That permission,
given under section 7 of the GPL, lets tmloader load and run with the
proprietary `TM_native.dll` and `polcore_native.dll`.

The license covers the loader source and tools only. The game files you supply
are not covered and remain the property of their respective owners. Third-party
library notices are listed in [third-party code](docs/third-party.md).
