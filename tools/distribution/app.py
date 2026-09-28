"""Commands used by the extracted portable application."""

import argparse
import asyncio
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from distribution import assets


def installed_viewer():
    base = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)"))
    return base / "PlayOnline/SquareEnix/PlayOnlineViewer"


def config_text(upgraded=False):
    return (
        "runtime-dir=runtime\ngame=TM_native.dll\ncore=polcore_native.dll\n"
        "log=logs/tmloader.log\nserver=127.0.0.1\ngame-port=51241\n"
        "profile-port=51220\npol-id=EEEEEEEF\npassword=tm-local-test\n"
        "diagnostics=false\ntexture-pack="
        + ("textures-hd/pack.json" if upgraded else "textures/pack.json")
        + "\n"
    )


def prepare_writable_runtime(root):
    runtime = assets.unlinked_path(root / "client/runtime")
    if os.name == "nt":
        for path in runtime.rglob("*"):
            assets.unlinked_path(path)
            if path.is_file() and path.stat().st_nlink != 1:
                raise ValueError(f"Runtime contains a hard-linked file: {path}")
        # The legacy process lowers its integrity. Label only this app's private copy.
        subprocess.run(
            ["icacls", str(runtime), "/setintegritylevel", "(OI)(CI)L", "/T", "/Q"],
            check=True,
            capture_output=True,
            creationflags=subprocess.CREATE_NO_WINDOW,
        )


def import_files(root, args):
    source = args.source
    if source is None:
        source = Path(
            input("Tetra Master game folder or prepared runtime folder: ").strip(' "')
        )
    viewer = args.viewer_dir or installed_viewer()
    defaults = args.viewer_defaults or viewer / "default/pub/home/option.bin"
    if not defaults.is_file() and args.source is None:
        defaults = Path(
            input("Viewer factory default/pub/home/option.bin path: ").strip(' "')
        )
    if (source / "TM_native.dll").is_file():
        selected = assets.dlls(source) | assets.originals(source, defaults)
    else:
        selected = assets.installed_dlls(source, viewer) | assets.originals(
            source, defaults, viewer / "data/utf/env.dat"
        )
    if args.texture_pack:
        selected.update(assets.textures(args.texture_pack))
    runtime = assets.unlinked_path(root / "client/runtime")
    # Identical bundled components are fine. A changed preference or game file is never overwritten.
    for name, data in selected.items():
        target = assets.unlinked_path(runtime / name)
        if target.exists() and assets.regular_file(target).read_bytes() != data:
            raise ValueError(
                f"Existing file differs; import into a fresh extraction: {target}"
            )
    runtime.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix=".import-", dir=root / "client"
    ) as temporary:
        staged = Path(temporary) / "runtime"
        assets.write_files(staged, selected)
        for name in selected:
            target = runtime / name
            if not target.exists():
                target.parent.mkdir(parents=True, exist_ok=True)
                (staged / name).rename(target)
    if args.texture_pack:
        config = root / "client/tmloader.ini"
        text = config.read_text(encoding="utf-8")
        if text == config_text():
            config.write_text(config_text(True), encoding="utf-8")
        else:
            print(
                "HD pack imported. Set texture-pack=textures-hd/pack.json in client/tmloader.ini."
            )
    prepare_writable_runtime(root)
    print(
        "Imported game files with fresh Viewer settings. Existing saves were preserved."
    )


def play_arguments(root, rest):
    return [
        "--project-dir",
        str(root / "server"),
        "--loader-dir",
        str(root / "client"),
        "--loader",
        str(root / "client/tmloader.exe"),
        "--loader-config",
        str(root / "client/tmloader.ini"),
        "--state",
        str(root / "user/server.json"),
        *rest,
    ]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("play", "check", "import", "server"))
    parser.add_argument(
        "--root", type=Path, default=Path(__file__).resolve().parents[2]
    )
    args, rest = parser.parse_known_args(argv)
    root = args.root.resolve(strict=True)
    try:
        if args.command == "import":
            importer = argparse.ArgumentParser(
                description="Import your local original game files."
            )
            importer.add_argument("--source", type=Path)
            importer.add_argument("--viewer-defaults", type=Path)
            importer.add_argument(
                "--viewer-dir", type=Path, help="PlayOnlineViewer installation folder"
            )
            importer.add_argument("--texture-pack", type=Path)
            import_files(root, importer.parse_args(rest))
            return 0
        if args.command == "server":
            from tmserver.cli.serve import arguments, run

            settings = arguments(
                [
                    "--state",
                    str(root / "user/server.json"),
                    "--log-dir",
                    str(root / "server/var"),
                    "--accounts",
                    "3",
                    "--rooms",
                    str(root / "server/examples/auto_rooms.example.json"),
                    *rest,
                ]
            )
            asyncio.run(run(settings))
            return 0
        if not (root / "client/runtime/TM_native.dll").is_file():
            raise ValueError(
                "Game files are missing. Run Import Game Files.cmd first; see README.txt."
            )
        from tmserver.cli.play import arguments, preflight, run

        settings = arguments(play_arguments(root, rest))
        if args.command == "check":
            settings.check = True
        if not settings.check:
            # Validate before adjusting permissions; --check remains read-only.
            asyncio.run(preflight(settings))
            prepare_writable_runtime(root)
        return asyncio.run(run(settings))
    except (OSError, ValueError, RuntimeError, TimeoutError) as exc:
        print(f"Could not complete setup: {exc}")
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
