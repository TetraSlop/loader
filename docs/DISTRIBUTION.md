# Client-only Windows distribution

`tools/package_app.py` builds an unzip-and-run tmloader client. **Neither tmserver
nor Python is included.** The server is built/deployed separately, and Play
only opens the client against the configured endpoint. Windows PowerShell
handles optional asset import and setup; recipients need no developer tools.

```powershell
py -3 tools/package_app.py --loader build-browser/Release/tmloader.exe
```

Output: `out/dist/TetraMaster-portable.zip`. Existing output files are never
overwritten. Use `--output` for another filename. The build needs Python 3.13+
and a compiled x86 loader, but no tmserver checkout or downloaded interpreter.

Connection defaults can be selected when packaging:

```powershell
py -3 tools/package_app.py --loader build-browser/Release/tmloader.exe `
  --server-address 203.0.113.10 --game-port 51241 --profile-port 51220 `
  --output out/dist/TetraMaster-client.zip
```

The address above is a documentation example. Use the deployed server's numeric
IPv4 address; the loader currently does not accept DNS names. Without these
options the INI has loopback/test defaults. Recipients use **Connection
Settings.cmd** to edit their endpoint and operator-provided credentials.
Passwords/current personal INIs are not taken from the build machine.

## Optional assets

Original client DLLs, original media and HD replacements are independently
opt-in. Supplying a runtime path alone includes nothing:

```powershell
py -3 tools/package_app.py --loader build-browser/Release/tmloader.exe `
  --output out/dist/TetraMaster-local-full.zip `
  --runtime runtime --include-dlls --include-originals `
  --viewer-defaults 'C:/Program Files (x86)/PlayOnline/SquareEnix/PlayOnlineViewer/default/pub/home/option.bin' `
  --include-upgraded runtime/textures-hd-v2/pack.json
```

- `--include-dlls`: exactly the two supported, hash-pinned game/Viewer DLLs.
- `--include-originals`: known data tables, graphics archives, audio and Viewer
  environment data, plus the hash-pinned factory Viewer option template.
- `--include-upgraded PACK_JSON`: referenced PNGs and engine manifest fields.
  Private build paths and unused authoring files are stripped.

Even the full bundle is client-only. No accounts, server catalogue/protocol data,
server executable/code, server startup script or Python runtime are selected.
Saves, logs, private preferences and AI models are omitted. Optional bundles do
not grant permission to redistribute proprietary assets.

## Layout and first run

```text
TetraMaster/
  Play.cmd, Check Setup.cmd, Import Game Files.cmd, Connection Settings.cmd
  client/tmloader.exe, tmloader.ini, runtime/ (optional client assets)
  app/client.ps1, assets.ps1, asset-policy.json
  licenses/
  package-manifest.json
```

Import local assets if needed, enter connection details, check setup, then Play.
See [the packaged instructions](PORTABLE_README.txt). Check Setup is read-only;
Play adjusts only this app's writable runtime directories before opening the
game. No script starts a server. Script execution policy is limited to the
helper process and does not alter Windows policy settings.

The importer uses the builder's exported hash/selection policy, checks the
complete selection before writes, validates staged originals/textures with
tmloader, and refuses changed destination files or linked paths. It imports
factory Viewer defaults rather than private account preferences.

## Validation

```powershell
py -3 -m unittest tests.tools.test_distribution
py -3 tools/check_portable.py --root 'out/extracted/TetraMaster'
```

The checker verifies the exact manifest, rejects server/Python/save payloads,
and runs the native read-only setup check when originals are included. It
does not open the game, contact services or start a backend. File hashes are
authoritative even when a checkout has uncommitted edits.

The [browser integration](BROWSER.md) consumes this same client subtree. Its
WebSocket gateway connects to an explicitly separate tmserver deployment.
