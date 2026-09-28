# Portable Windows distribution

`tools/package_app.py` builds an unzip-and-run app with tmloader, tmserver,
an isolated Python runtime and console launch/setup shortcuts. It does not
install anything, require a compiler, launch the game, or publish/upload files.
Use a Windows x86 Release loader build; the bundled Python server is x64.

## Default package

From this repository with the sibling tmserver checkout:

```powershell
py -3 tools/package_app.py --loader build-browser/Release/tmloader.exe
```

Output: `out/dist/TetraMaster-portable.zip`. An existing output is never replaced;
use `--output` for a new filename. `--server` selects another tmserver checkout.
Default packages omit original client DLLs, original client media, and replacement
images. The server still includes its recovered protocol/catalogue/rule data.
Recipients import their local files with **Import Game Files.cmd**, run
**Check Setup.cmd**, then **Play.cmd**. See [the packaged instructions](PORTABLE_README.txt).

Python 3.13.15's official Windows embeddable x64 ZIP is downloaded once to
`out/package-cache` and checked against its pinned SHA256. `--python-archive`
accepts an offline copy with the same checksum. Its isolated `._pth` includes
only the embedded standard library, `server/` and `app/`; no pip or global site
packages are involved. Source/checksum: [Python release files](https://www.python.org/downloads/release/python-31315/).

## Explicit client components

Each switch is independent. Merely supplying `--runtime` includes nothing.

```powershell
py -3 tools/package_app.py --loader build-browser/Release/tmloader.exe `
  --output out/dist/TetraMaster-local-full.zip `
  --runtime runtime --include-dlls --include-originals `
  --viewer-defaults 'C:/Program Files (x86)/PlayOnline/SquareEnix/PlayOnlineViewer/default/pub/home/option.bin' `
  --include-upgraded runtime/textures-hd-v2/pack.json
```

- `--include-dlls`: exactly the two supported, hash-pinned game/Viewer binaries.
- `--include-originals`: known client data tables, archives, audio and Viewer
  environment data. Requires the hash-pinned factory Viewer option template.
  A live `pub/home00/option.bin` is never used as the packaging input.
- `--include-upgraded PACK_JSON`: only referenced PNGs and the engine fields of
  their manifest; build-machine paths and unused authoring files are stripped.

Original data and HD images remain explicit opt-ins even if they are present
beside the executable. These local bundle options are not permission to
redistribute third-party assets. No existing saves, credentials/configuration,
logs, wrapper preferences, IDE files or AI model weights are selected.
The fresh client config uses the local synthetic test account only.

## Layout and validation

```text
TetraMaster/
  Play.cmd, Check Setup.cmd, Import Game Files.cmd, Start Server.cmd
  client/tmloader.exe, tmloader.ini, runtime/ (optional imported files)
  python/                   pinned application-local interpreter
  server/tmserver/           exact tmserver runtime source/data manifest
  server/examples/          local room configuration
  app/distribution/         small setup/launch module
  licenses/                 linked third-party notices
  package-manifest.json     component selection, revisions, file SHA256s
  user/                     created at first play; never pre-populated
```

Build receipts contain relative paths and content hashes, not source-machine
paths. Revisions identify the checkout commits; hashes describe exact bundled
bytes including any uncommitted changes. The importer validates all selected
files before copying and refuses changed destination files. Only the new app's
runtime copy is labelled for the legacy client's low-integrity writes. The
read-only check neither labels files nor opens the game.

```powershell
py -3 -m unittest tests.tools.test_distribution
py -3 tools/check_portable.py --root 'out/extracted/TetraMaster'
```

The smoke check uses the embedded interpreter, a disposable save and ephemeral
ports. It never launches the native game and checks that the extracted package
is unchanged. For a bundle with all original components, also run its
`python/python.exe -I -B -m distribution.app check` from its root; this includes
the loader's native hash, path and texture checks without opening a window.

The portable ZIP is a local testing distribution. Signing/installer integration,
public account provisioning and a hosted-server deployment are separate work.
