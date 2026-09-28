"""Explicit asset selection, shared by packaging and the local import command."""

import hashlib
import json
import re
import struct
from pathlib import Path, PurePosixPath

DLL_HASHES = {
    "TM_native.dll": "29c965a835fad561a94860262f780bc087b7bcc89be9e3e89da8f08d57f52003",
    "polcore_native.dll": "73b1864bf522d3aa6ec28e9f4bb226fcee956f7f4848a238c9c2d756dc703b06",
}
DEFAULT_OPTIONS_HASH = (
    "b46e4c6262ff149ba18707145e2f51cf0dddf374ae198fa36539395dc4c2ff01"
)
TABLES = set(
    "Aucti CardPri CardPrm Cardsel Cardsh Chatmess CoPrm Error Friend Lobby "
    "Nuisance Option PackPrm Playdat Playmes PlPrm Ranking RankingPrize TMess "
    "Title Trade filter kiyaku rules rules_b".lower().split()
)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def unlinked_path(path):
    """Reject links/junctions, including ancestors of paths that do not yet exist."""
    path = Path(path).absolute()
    for part in (path, *path.parents):
        if part.is_symlink() or part.is_junction():
            raise ValueError(f"Linked paths are not accepted: {path}")
    return path


def regular_file(path):
    path = unlinked_path(path)
    if not path.is_file():
        raise ValueError(f"Missing file: {path}")
    return path


def relative_name(value):
    if not isinstance(value, str) or "\\" in value or ":" in value:
        raise ValueError(
            "Archive and texture paths must use relative forward-slash names"
        )
    path = PurePosixPath(value)
    if (
        not value
        or path.is_absolute()
        or any(p in ("", ".", "..") for p in value.split("/"))
    ):
        raise ValueError(f"Unsafe relative path: {value}")
    return path.as_posix()


def verified_file(path, expected):
    data = regular_file(path).read_bytes()
    if digest(data) != expected.lower():
        raise ValueError(f"Unsupported or damaged file: {path}")
    return data


def dlls(source):
    return {
        name: verified_file(Path(source) / name, expected)
        for name, expected in DLL_HASHES.items()
    }


def installed_dlls(game, viewer):
    paths = {
        "TM_native.dll": Path(game) / "TM.dll",
        "polcore_native.dll": Path(viewer) / "viewer/com/polcore.dll",
    }
    return {name: verified_file(path, DLL_HASHES[name]) for name, path in paths.items()}


def original_name(name):
    path = PurePosixPath(name.lower())
    if len(path.parts) == 1:
        return (
            path.stem in TABLES and path.suffix in (".bin", ".txt")
        ) or re.fullmatch(r"gw\d{3}\.dat", path.name) is not None
    return (
        name.lower() == "utf/env.dat"
        or re.fullmatch(r"win/sound/music/music\d+\.bgw", name.lower()) is not None
        or re.fullmatch(r"win/sound/se/se\d+\.spw", name.lower()) is not None
    )


def originals(source, defaults, environment=None):
    source = Path(source)
    result = {}
    for path in sorted((source / "data").rglob("*")):
        name = path.relative_to(source / "data").as_posix()
        if original_name(name):
            result[f"data/{name}"] = regular_file(path).read_bytes()
    if environment:
        result["data/utf/env.dat"] = regular_file(environment).read_bytes()
    for name in ("data/gW201.dat", "data/utf/env.dat"):
        if name not in result:
            raise ValueError(f"Missing original asset: {source / name}")
    # This is the shipped Viewer template, not pub/home00 from an active account.
    result["pub/home00/option.bin"] = verified_file(defaults, DEFAULT_OPTIONS_HASH)
    return result


def textures(manifest):
    manifest = regular_file(manifest)
    document = json.loads(manifest.read_text(encoding="utf-8"))
    entries = document.get("textures")
    if (
        document.get("version") != 1
        or not isinstance(entries, list)
        or len(entries) > 2048
    ):
        raise ValueError("Unsupported texture pack manifest")
    result, records = {}, []
    fields = ("id", "source_sha256", "sha256", "file", "logical_size", "physical_size")
    for entry in entries:
        if not isinstance(entry, dict) or any(key not in entry for key in fields):
            raise ValueError("Incomplete texture record")
        record = {key: entry[key] for key in fields}
        name = relative_name(record["file"])
        if PurePosixPath(name).suffix.lower() != ".png":
            raise ValueError("Texture replacements must be PNG files")
        for key in ("source_sha256", "sha256"):
            if not isinstance(record[key], str) or not re.fullmatch(
                r"[0-9a-fA-F]{64}", record[key]
            ):
                raise ValueError(f"Invalid texture {key}")
        logical, physical = record["logical_size"], record["physical_size"]
        if any(
            not isinstance(size, list)
            or len(size) != 2
            or any(type(n) is not int or not 1 <= n <= 8192 for n in size)
            for size in (logical, physical)
        ):
            raise ValueError("Invalid texture dimensions")
        if not any(physical == [n * scale for n in logical] for scale in (2, 3, 4)):
            raise ValueError("Texture scale must be uniformly 2x, 3x or 4x")
        data = verified_file(manifest.parent / name, record["sha256"])
        if (
            len(data) < 33
            or data[:8] != b"\x89PNG\r\n\x1a\n"
            or data[12:16] != b"IHDR"
            or list(struct.unpack_from(">II", data, 16)) != physical
        ):
            raise ValueError("Texture PNG dimensions differ from the manifest")
        result[f"textures-hd/{name}"] = data
        records.append(record)
    # Export only engine fields: build-machine paths and authoring metadata stay local.
    result["textures-hd/pack.json"] = (
        json.dumps({"version": 1, "textures": records}, indent=2) + "\n"
    ).encode()
    return result


def write_files(root, files):
    """Write a prevalidated selection into a fresh directory."""
    root = unlinked_path(root)
    root.mkdir(parents=True, exist_ok=False)
    for name, data in files.items():
        target = root / relative_name(name)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
