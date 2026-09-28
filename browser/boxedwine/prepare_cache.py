"""Fetch pinned public runtime inputs and apply the separately maintained patch."""

import argparse
import hashlib
import subprocess
import urllib.request
import zipfile
from pathlib import Path

COMMIT = "05e033b115b8c64ae14cc3836d5246ab8d927fbf"
INPUTS = (
    (
        "boxedwine-source.zip",
        f"https://codeload.github.com/danoon2/Boxedwine/zip/{COMMIT}",
        "939a72a208d2f3b938c16b70b6a54e139d62cdc926e3e579e846981850b7d3e1",
    ),
    (
        "TinyCore15Wine11.0.zip",
        "https://www.boxedwine.org/v2/11/TinyCore15Wine11.0.zip",
        "ad98ff608f04f6129e4e4159a6d3f1f357e70f56e341a73bb2aa9c1404fe6e13",
    ),
)


def prepare(cache):
    cache = cache.resolve()
    cache.mkdir(parents=True, exist_ok=True)
    for filename, url, expected in INPUTS:
        path = cache / filename
        if not path.exists():
            print(f"Downloading {filename}", flush=True)
            urllib.request.urlretrieve(url, path)
        with path.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        if digest != expected:
            raise ValueError(f"Checksum mismatch: {path}")
    source = cache / "source" / ("Boxedwine-" + COMMIT)
    if not source.exists():
        with zipfile.ZipFile(cache / "boxedwine-source.zip") as archive:
            archive.extractall(cache / "source")  # Exact archive hash verified above.
    patch = Path(__file__).with_name("browser-network.patch").resolve()
    command = ["git", "apply", "--check", str(patch)]
    check = subprocess.run(
        command, cwd=source, capture_output=True, text=True, check=False
    )
    if check.returncode == 0:
        subprocess.run(["git", "apply", str(patch)], cwd=source, check=True)
    else:
        reverse = subprocess.run(
            ["git", "apply", "--reverse", "--check", str(patch)],
            cwd=source,
            capture_output=True,
            text=True,
            check=False,
        )
        if reverse.returncode:
            raise RuntimeError(
                "Source differs from the pinned runtime and patch: " + check.stderr
            )
    print(f"Ready to build: {source}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache-dir", type=Path, required=True)
    prepare(parser.parse_args().cache_dir)
