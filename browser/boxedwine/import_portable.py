"""Convert only the portable package's client subtree into a BoxedWine app ZIP."""

import argparse
import hashlib
import json
import zipfile
from pathlib import Path, PurePosixPath

from runtime_paths import runtime_root


def import_client(package, destination):
    package, destination = Path(package), Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    prefix = "TetraMaster/client/"
    rows = []
    names = set()
    allowed = ("runtime/data/", "runtime/textures-hd/")
    exact = {
        "tmloader.exe",
        "tmloader.ini",
        "runtime/TM_native.dll",
        "runtime/polcore_native.dll",
        "runtime/pub/home00/option.bin",
    }
    with zipfile.ZipFile(package) as source:
        for info in source.infolist():
            if not info.filename.startswith(prefix) or info.is_dir():
                continue
            name = info.filename[len(prefix) :]
            path = PurePosixPath(name)
            if (
                path.is_absolute()
                or ".." in path.parts
                or "\\" in name
                or ":" in name
                or name.casefold() in names
            ):
                raise ValueError("Unsafe or duplicate client archive path: " + name)
            names.add(name.casefold())
            if name not in exact and not name.startswith(allowed):
                continue  # Never import logs, captures, profiles or other runtime state.
            rows.append((name, info))
        if not {"tmloader.exe", "tmloader.ini"} <= {name for name, _ in rows}:
            raise ValueError(
                "Package is missing TetraMaster/client/tmloader.exe or tmloader.ini"
            )
        archive_path = destination / "portable-client.zip"
        manifest_rows = []
        with zipfile.ZipFile(archive_path, "w", zipfile.ZIP_DEFLATED) as target:
            for name, info in rows:
                data = source.read(info)
                target.writestr(name, data)
                manifest_rows.append(
                    {
                        "path": name,
                        "bytes": len(data),
                        "sha256": hashlib.sha256(data).hexdigest(),
                    }
                )
    imported = {row["path"] for row in manifest_rows}
    required = {
        "runtime/tm_native.dll",
        "runtime/polcore_native.dll",
        "runtime/pub/home00/option.bin",
        "runtime/data/utf/env.dat",
        "runtime/data/gw201.dat",
    }
    has_originals = required <= {name.casefold() for name in imported}
    with package.open("rb") as stream:
        package_hash = hashlib.file_digest(stream, "sha256").hexdigest()
    manifest = {
        "package": package.name,
        "package_sha256": package_hash,
        "has_originals": has_originals,
        "files": manifest_rows,
    }
    (destination / "client-manifest.json").write_text(
        json.dumps(manifest, indent=2), encoding="utf8"
    )
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", type=Path)
    parser.add_argument("--output", type=Path, default=runtime_root() / "online")
    args = parser.parse_args()
    result = import_client(args.package, args.output)
    print(
        f"Imported {len(result['files'])} client files; original game files: {result['has_originals']}"
    )
