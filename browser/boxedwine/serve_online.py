"""Run the local browser host and Python services, without opening any windows."""

import argparse
import asyncio
import os
import sys
from pathlib import Path

from aiohttp import web
from network_gateway import create_app
from runtime_paths import runtime_root


def default_rooms(server_dir):
    """Support both the redistributable's server/ and a source checkout's src/."""
    for root in (server_dir, server_dir.parent):
        path = root / "examples/auto_rooms.example.json"
        if path.is_file():
            return path
    raise ValueError("Cannot find auto_rooms.example.json; pass --rooms explicitly")


async def run(args):
    sys.path.insert(0, str(args.server_dir.resolve()))
    from tmserver.cli.serve import arguments, running_services

    options = [
        "--game-port",
        "0",
        "--profile-port",
        "0",
        "--accounts",
        "3",
        "--state",
        str(args.state),
        "--log-dir",
        str(args.state.parent / "logs"),
    ]
    options += ["--rooms", str(args.rooms or default_rooms(args.server_dir))]
    async with running_services(arguments(options)) as services:
        runner = web.AppRunner(
            create_app(
                args.runtime_dir / "online",
                game=("127.0.0.1", services.game_port),
                profile=("127.0.0.1", services.profile_port),
            )
        )
        try:
            await runner.setup()
            site = web.TCPSite(runner, "127.0.0.1", args.port)
            await site.start()
            port = site._server.sockets[0].getsockname()[1]
            pid = args.runtime_dir / "online-host.pid"
            pid.write_text(str(os.getpid()), encoding="ascii")
            print(f"Tetra Master is ready at http://127.0.0.1:{port}/", flush=True)
            print(f"Browser play data: {args.state}", flush=True)
            try:
                await asyncio.Event().wait()
            finally:
                if pid.read_text(encoding="ascii") == str(os.getpid()):
                    pid.unlink()
        finally:
            await runner.cleanup()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--server-dir",
        required=True,
        type=Path,
        help="Directory containing tmserver: portable package/server or source/src",
    )
    parser.add_argument("--runtime-dir", type=Path, default=runtime_root())
    parser.add_argument("--state", type=Path)
    parser.add_argument("--rooms", type=Path)
    parser.add_argument("--port", type=int, default=8767)
    args = parser.parse_args()
    args.state = args.state or args.runtime_dir / "play/state.json"
    if not (args.server_dir / "tmserver/__init__.py").is_file():
        parser.error("--server-dir must contain the tmserver package")
    if not (args.runtime_dir / "online/index.html").is_file():
        parser.error("Stage the browser runtime before starting the host")
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        pass
