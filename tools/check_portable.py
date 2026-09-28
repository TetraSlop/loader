"""Check an extracted application's embedded server without starting the native game."""

import argparse
import hashlib
import json
import os
import subprocess
import tempfile
from pathlib import Path

PROBE = """
import hashlib
import json
from pathlib import Path
import sys
from importlib.resources import files

import tmserver
from tmserver.economy.service import EconomyService
from tmserver.network.server import Server
from tmserver.storage.json import JsonStateStore

root, state = (Path(value).resolve() for value in sys.argv[1:])
package = root / 'server/tmserver'
assert sys.version_info >= (3, 13), sys.version
assert Path(sys.executable).resolve() == root / 'python/python.exe', sys.executable
assert Path(tmserver.__file__).resolve() == package / '__init__.py', tmserver.__file__
outside = [name for name, module in sys.modules.items()
           if (name == 'tmserver' or name.startswith('tmserver.'))
           and getattr(module, '__file__', None)
           and not Path(module.__file__).resolve().is_relative_to(package)]
assert not outside, outside
resources = {}
for name in ('economy_catalog.json', 'profile_contract.json', 'lobby_commands.tsv'):
    data = files('tmserver').joinpath('data', name).read_bytes()
    assert data, name
    resources[name] = hashlib.sha256(data).hexdigest()
economy = EconomyService(seed=1)
economy.provision('portable-smoke', nn=1, player_id=1, name=b'Portable')
JsonStateStore(state).save(economy.snapshot())
print(json.dumps({'python': sys.version.split()[0], 'package': str(package),
                  'resources': resources}))
"""


def tree_snapshot(root):
    """Compare content and directory membership; reading does not change the bundle."""
    result = {}
    for path in sorted(root.rglob("*")):
        if path.is_symlink() or path.is_junction():
            raise ValueError(
                f"Portable check requires an extracted tree without links: {path}"
            )
        relative = path.relative_to(root).as_posix()
        if path.is_dir():
            result[relative + "/"] = None
        elif path.is_file():
            with path.open("rb") as stream:
                result[relative] = hashlib.file_digest(stream, "sha256").hexdigest()
    return result


def child_environment():
    return {
        key: value
        for key, value in os.environ.items()
        if not key.upper().startswith(("PYTHON", "TM_"))
        and key.upper() not in {"VIRTUAL_ENV", "CONDA_PREFIX", "__COMPAT_LAYER"}
    }


def check(root):
    root = root.resolve(strict=True)
    python = root / "python/python.exe"
    rooms = root / "server/examples/auto_rooms.example.json"
    for path in (python, rooms, root / "server/tmserver/__init__.py"):
        if not path.is_file():
            raise ValueError(f"Missing portable application file: {path}")
    before = tree_snapshot(root)
    try:
        with tempfile.TemporaryDirectory(prefix="tm-portable-check-") as folder:
            temporary = Path(folder).resolve()
            if temporary.is_relative_to(root):
                raise ValueError(
                    "The temporary check directory must be outside the application"
                )
            state = temporary / "server.json"
            logs = temporary / "logs"

            def run(*arguments):
                process = subprocess.run(
                    [str(python), "-I", "-B", *arguments],
                    cwd=temporary,
                    env=child_environment(),
                    check=True,
                    capture_output=True,
                    text=True,
                    encoding="utf-8",
                    timeout=30,
                    **(
                        {"creationflags": subprocess.CREATE_NO_WINDOW}
                        if os.name == "nt"
                        else {}
                    ),
                )
                return process.stdout

            report = json.loads(run("-c", PROBE, str(root), str(state)))
            run("-m", "tmserver", "--help")
            help_text = run("-m", "tmserver.cli.play", "--help")
            if "--loader-config" not in help_text or "--loader-dir" not in help_text:
                raise ValueError("The bundled server lacks portable loader options")
            run("-m", "tmserver.cli.admin", "--help")
            accounts = run("-m", "tmserver", "--list-accounts", "--accounts", "3")
            if len(accounts.strip().splitlines()) != 3:
                raise ValueError("The bundled server did not list three local accounts")
            ready = run(
                "-m",
                "tmserver",
                "--state",
                str(state),
                "--log-dir",
                str(logs),
                "--rooms",
                str(rooms),
                "--accounts",
                "3",
                "--game-port",
                "0",
                "--profile-port",
                "0",
                "--seconds",
                "0.1",
            )
            if "Tetra Master services ready: game 127.0.0.1:" not in ready:
                raise ValueError(
                    "The bundled services did not report loopback readiness"
                )
            if not state.is_file() or not logs.is_dir():
                raise ValueError(
                    "Server state or logs were not created in the temporary directory"
                )
            report.update(cli_help=True, local_accounts=3, ephemeral_services=True)
    finally:
        after = tree_snapshot(root)
        changed = sorted(
            name
            for name in before.keys() | after.keys()
            if name not in before or name not in after or before[name] != after[name]
        )
        if changed:
            raise RuntimeError(f"Portable check changed application files: {changed}")
    return dict(report, application_unchanged=True, native_game_launched=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root", type=Path, required=True, help="extracted application directory"
    )
    try:
        report = check(parser.parse_args().root)
    except subprocess.CalledProcessError as error:
        raise SystemExit(
            error.stderr.strip() or error.stdout.strip() or str(error)
        ) from None
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        raise SystemExit(str(error)) from None
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
