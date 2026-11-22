# A local tmserver gateway for the browser build's scripted Game IRC login: the game and profile services and the
# HTTP/WebSocket gateway with its browser streams (tmserver docs/LOADER_AUTH_SPEC.md, "Browser clients"), on
# loopback. Sign-in uses a stand-in verifier, as tmserver's own gateway tests do, until tmserver has a loopback
# dev sign-in. Writes the play session the Node runner reads (run.mjs --session) and serves until killed.
# Usage: TMSERVER=<tmserver checkout> python gateway.py session.json [--origin http://localhost:5173]
import argparse
import asyncio
import json
import os
import sys
import tempfile
from pathlib import Path

ACCOUNT = "11111111-1111-4111-8111-111111111111"  # a stand-in identity, bound to the first local POL ID


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("session")
    parser.add_argument("--origin", default="http://localhost:5173")
    args = parser.parse_args()
    root = os.environ.get("TMSERVER")
    if not root:
        sys.exit("set TMSERVER to a tmserver checkout with browser streams (PR #19)")
    sys.path[:0] = [str(Path(root) / "src"), root]

    import httpx
    from tests.support.memory_store import MemoryStore
    from tmserver.auth.config import AuthConfig
    from tmserver.auth.provider import Subject
    from tmserver.auth.web import create_app
    from tmserver.network.http import serving_http
    from tmserver.network.server import Server
    from tmserver.profiles.service import ProfileServer

    class Verifier:
        issuer = "https://example.supabase.co/auth/v1"

        async def verify(self, token):
            return Subject(issuer=self.issuer, subject=token, display_name="Browser Player")

    with tempfile.TemporaryDirectory() as directory:
        host = Server(store=MemoryStore())
        profile = ProfileServer(Path(directory) / "profile.log", host)
        await host.start(0)
        await profile.start(0)
        config = AuthConfig(supabase_url="https://example.supabase.co", publishable_key="sb_publishable_test_only",
                            public_origin="https://tm.example", browser_origins=[args.origin],
                            bindings={ACCOUNT: "EEEEEEEF"})
        apps = []

        def application(port):
            apps.append(create_app(host, profile, config, Verifier(), public_origin=f"http://127.0.0.1:{port}"))
            return apps[-1]

        async with serving_http(application, port=0, websockets=True) as listener:
            base = f"http://127.0.0.1:{listener.port}"
            async with httpx.AsyncClient(base_url=base, trust_env=False) as http:
                page = {"Origin": args.origin}
                ticket = await http.post("/v1/play/tickets", json={"protocol": 1},
                                         headers=page | {"Authorization": "Bearer " + ACCOUNT})
                ticket.raise_for_status()
                session = await http.post("/v1/play/sessions", json={"ticket": ticket.json()["ticket"]}, headers=page)
                session.raise_for_status()
            play = session.json() | {"origin": args.origin}
            Path(args.session).write_text(json.dumps(play, indent=1))
            print(f"gateway {base}: game {play['streams']['game']}, profile {play['streams']['profile']}", flush=True)
            try:
                await asyncio.Event().wait()
            finally:
                apps[-1].state.play_sessions.close()
                await profile.stop()
                await host.stop()


if __name__ == "__main__":
    asyncio.run(main())
