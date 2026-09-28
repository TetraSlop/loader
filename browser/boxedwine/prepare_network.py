"""Stage the separate network runtime; never copy the old game's assets or saves."""

import argparse
import hashlib
import json
import shutil
import zipfile
from pathlib import Path

from runtime_paths import runtime_root

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument(
    "--cache-dir",
    required=True,
    type=Path,
    help="External BoxedWine source/toolchain/Wine archive cache",
)
parser.add_argument("--output", type=Path, default=runtime_root() / "online")
args = parser.parse_args()
BASE = args.cache_dir.resolve()
COMMIT = "05e033b115b8c64ae14cc3836d5246ab8d927fbf"
source = BASE / "source" / ("Boxedwine-" + COMMIT) / "project/emscripten/Build/Network"
target = args.output.resolve()
target.mkdir(parents=True, exist_ok=True)
names = ("boxedwine.js", "boxedwine.wasm", "boxedwine.css", "boxedwine-shell.js")
for name in names:
    shutil.copyfile(source / name, target / name)
for name in ("TinyCore15Wine11.0.zip",):
    if not (target / name).exists():
        try:
            (target / name).hardlink_to(BASE / name)
        except OSError:
            shutil.copyfile(BASE / name, target / name)
with zipfile.ZipFile(
    target / "wine-prefix-ready.zip", "w", zipfile.ZIP_DEFLATED
) as prefix:
    prefix.writestr("home/username/.wine/.update-timestamp", b"disable\n")
html = (source / "boxedwine.html").read_text(encoding="utf8")
needle = "<script src=boxedwine-shell.js></script>"
if needle not in html:
    needle = '<script src="boxedwine-shell.js"></script>'
assert needle in html
script = """
<script>
const probe = new URLSearchParams(location.search).get('probe') === '1';
const check = new URLSearchParams(location.search).get('check') === '1';
const originalArt = new URLSearchParams(location.search).get('artwork') !== 'hd';
Config.urlParams = 'root=wine-prefix-ready&overlay=TinyCore15Wine11.0&sound=false&storage=memory&app='
  + (probe ? 'socket-probe' : 'portable-client') + '&p='
  + (probe ? 'socket_probe.exe' : 'tmloader.exe') + '&args='
  + encodeURIComponent(probe ? '' : '--config tmloader.ini' + (check ? ' --check' : '')
      + (originalArt ? ' --texture-pack textures/pack.json' : ''));
Module.websocket = {url: 'ws://', subprotocol: 'binary'};
const wsBase = location.origin.replace(/^http/, 'ws');
Module.tmSocketRoutes = {51241: wsBase + '/tm/game', 51220: wsBase + '/tm/profile'};
Module.printErr = Module.print;
document.getElementById('output').style.display = 'block';
Module.print('BoxedWine network experiment: fixed game/profile WebSocket routes; temporary filesystem.');
</script>
"""
html = html.replace(needle, needle + script, 1).replace(
    "<!doctypehtml>", "<!doctype html>"
)
html = html.replace("<title>BoxedWine</title>", "<title>Tetra Master</title>")
html = html.replace(
    "<body>",
    """<body><div style="padding:12px;font:16px system-ui">
<strong>Tetra Master</strong><br>
<a href="?">Play / restart</a> | Artwork: <a href="?artwork=original">Original</a> | <a href="?artwork=hd">HD (experimental)</a>
</div>""",
    1,
)
html = html.replace(
    "<script src=boxedwine.js async></script>",
    """<script>
const details = document.createElement('details');
details.innerHTML = '<summary>Diagnostics</summary>';
details.append(document.getElementById('controls'), document.getElementById('console'));
document.body.append(details);
if (probe || check) details.open = true;
async function launchGame() {
  try {
    if (!probe) {
      const response = await fetch('client-manifest.json');
      if (!response.ok) throw new Error('Import a portable client package first.');
      const manifest = await response.json();
      if (!manifest.has_originals) throw new Error('Original game files are required. Import a package with your locally supplied game files.');
    }
    const script = document.createElement('script');
    script.src = 'boxedwine.js';
    document.body.append(script);
  } catch (error) {
    document.getElementById('status').textContent = error.message;
    document.getElementById('spinner').style.display = 'none';
    document.getElementById('progress').style.display = 'none';
  }
}
launchGame();
</script>""",
)
(target / "index.html").write_text(html, encoding="utf8")
(target / "build-manifest.json").write_text(
    json.dumps(
        {
            "source_commit": COMMIT,
            "target": "WASM JIT + BOXEDWINE_BROWSER_NETWORK",
            "patches": [
                "Link C++ objects using em++",
                "Fixed-route Emscripten stream sockets",
                "Single-thread socket readiness polling",
                "Virtual buffer options and native asynchronous SO_ERROR",
            ],
            "patch_sha256": hashlib.sha256(
                Path(__file__).with_name("browser-network.patch").read_bytes()
            ).hexdigest(),
            "files": {
                name: hashlib.sha256((target / name).read_bytes()).hexdigest()
                for name in names
            },
        },
        indent=2,
    ),
    encoding="utf8",
)
print(target)
