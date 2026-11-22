# tmserver's game and profile services and its gateway with the loopback dev identity (tmserver
# docs/LOADER_AUTH_SPEC.md, "Local development without Supabase"), in one process as tmserver's own dev-identity
# test runs them: an in-memory store, so neither PostgreSQL nor the x86 rules bridge is needed for a login. For the
# Node runner's scripted Game IRC login: run.mjs --sign-in http://127.0.0.1:8081 --as <name>. A full
# `tmserver --auth-dev-identity` serves the same routes. Serves until killed.
# Usage: TMSERVER=<tmserver checkout> python gateway.py [--port 8081]
import argparse
import asyncio
import os
import sys
import tempfile
from pathlib import Path


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8081)
    args = parser.parse_args()
    root = os.environ.get("TMSERVER")
    if not root:
        sys.exit("set TMSERVER to a tmserver checkout with the dev identity (tmserver main)")
    sys.path[:0] = [str(Path(root) / "src"), root]

    from tests.support.memory_store import MemoryStore
    from tmserver.auth.config import load_auth_config
    from tmserver.auth.server import serving_auth
    from tmserver.network.server import Server
    from tmserver.profiles.service import ProfileServer

    with tempfile.TemporaryDirectory() as folder:
        host = Server(store=MemoryStore())
        profile = ProfileServer(Path(folder) / "profile.jsonl", host)
        await host.start(0)
        await profile.start(0)
        try:
            async with serving_auth(host, profile, load_auth_config(Path(root) / "examples" / "auth-dev.json"),
                                    port=args.port, insecure_loopback=True, dev_identity=True) as auth:
                print(f"gateway http://127.0.0.1:{auth.port} with the dev identity", flush=True)
                await asyncio.Event().wait()
        finally:
            await profile.stop()
            await host.stop()


if __name__ == "__main__":
    asyncio.run(main())
