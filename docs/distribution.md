# Distribution

`tools/build_release.py` builds one archive, `tmloader-VERSION-windows-x86.zip`.
It holds no game files and no server; you supply the game files from your own
install, and the loader connects to the server you sign in to.

## Layout

```text
TetraMaster/
  tmloader.exe
  tmloader.ini                 read automatically from the executable's folder
  retail-files.txt             the game files the runtime folder needs, with hashes
  LICENSE.txt                  GPL-3.0
  LICENSE-EXCEPTION.txt        additional permission for the two game DLLs
  THIRD_PARTY_NOTICES.txt      CLI11, Dear ImGui, nlohmann/json, WIL
  runtime/                     created by you, see below
```

Create `runtime/` from your own PlayOnline install of Tetra Master, with the
paths in `retail-files.txt`:

- `TM.dll` from the Tetra Master game folder, renamed to `TM_native.dll`, and
  that folder's `data\` folder.
- `viewer\com\polcore.dll` from the PlayOnline Viewer, renamed to
  `polcore_native.dll`, the Viewer's `data\utf\env.dat`, and its
  `default\pub\home\option.bin` as `pub\home00\option.bin`.

From a source checkout, `py tools/prepare_runtime.py` does this (see the
[README](../README.md#game-files)). `sha256sum -c ../retail-files.txt`, run in
`runtime/`, checks the copy. `tmloader.exe --check` does the same, against the
`retail-files.txt` beside it, and also verifies the settings.

Run `tmloader.exe`. On first start it asks for your POL ID, password and
server, signs in, and saves them to `tmloader.ini`; a new POL ID becomes a new
account (see [accounts](../README.md#accounts)). The shipped `tmloader.ini`
holds no account. Change the ports there if the server operator gives others.

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
2. Writes the archive with fixed timestamps.
3. Extracts it, adds a `runtime/` folder with placeholder DLLs and runs
   `tmloader.exe --check`. The check must fail at the DLL hash check and leave
   the extracted files unchanged.
4. Writes `SHA256SUMS.txt` and `validation.json`.
