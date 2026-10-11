# Distribution

`tools/build_release.py` builds two archives. Neither contains a server; the
loader connects to the server named in `tmloader.ini`.

| Archive | Contents |
|---|---|
| `tmloader-VERSION-windows-x86.zip` | `tmloader.exe`, a sample `tmloader.ini` and license notices |
| `tmloader-VERSION-windows-x86-full.zip` | The same, plus `runtime/`: the supported game and Viewer DLLs, original data and audio, factory Viewer settings and the HD texture pack |

## Layout

```text
TetraMaster/
  tmloader.exe
  tmloader.ini                 read automatically from the executable's folder
  LICENSE.txt                  GPL-3.0
  THIRD_PARTY_NOTICES.txt      CLI11, Dear ImGui, nlohmann/json, WIL
  runtime/                     full archive only
    TM_native.dll, polcore_native.dll
    data/                      original tables, graphics and audio
    pub/home00/option.bin      factory Viewer settings
    textures/                  HD texture pack (toggled in F10)
```

Edit `tmloader.ini` with the server address, ports and account from the server
operator, then run `tmloader.exe`. `tmloader.exe --check` validates the setup
without starting the game.

With the minimal archive, create `runtime/` with the same layout from an
original install: `TM.dll` from the game folder renamed to `TM_native.dll`,
the game's `data\` folder, `viewer\com\polcore.dll` from the PlayOnline Viewer
renamed to `polcore_native.dll`, the Viewer's `data\utf\env.dat`, and its
`default\pub\home\option.bin` as `pub\home00\option.bin`. `--check` verifies
the DLL hashes and required files.

The game runs at low integrity. On launch the loader labels `runtime/pub`,
`runtime/logs` and `runtime/settings` as writable at that level; everything
else stays read-only to the game.

## Building

```powershell
python tools/build_release.py --loader build/Release/tmloader.exe
```

Output goes to `out/release/` (`--output` for another folder); existing
archives are never overwritten. The script:

1. Checks the loader is a PE32 x86 GUI executable linked without ASLR and DEP,
   with an `asInvoker` manifest.
2. Writes both archives with fixed timestamps.
3. Extracts the full archive and runs its `tmloader.exe --check`, failing if
   the check fails or changes any extracted file.
4. Writes `SHA256SUMS.txt` and `validation.json`.

Including game files does not grant permission to redistribute them.
