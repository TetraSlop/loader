"""Run tmloader's hidden [.gateway] tests against a real tmserver gateway and a fake Supabase.

Local development only. Without --tests it keeps the servers running so the real client
can be played through them. Needs a tmserver checkout with its auth extra installed, by
default ../tmserver beside this one, and skips when there is none. Run it with that
checkout's Python, for example:

    ..\\tmserver\\.venv\\Scripts\\python tests/hosted/gateway_harness.py --tmserver ..\\tmserver
        --tests build\\RelWithDebInfo\\hosted_tests.exe   (path of the built test binary)

The fake Supabase issues real PKCE codes and rotating refresh tokens over HTTP loopback;
tmserver's HTTPS-only project check is relaxed for 127.0.0.1 in its process only.
"""

import argparse
import asyncio
import base64
import hashlib
import json
import os
import re
import secrets
import socket
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

DEFAULT_TMSERVER = Path(__file__).resolve().parents[3] / "tmserver"
SUBJECT = "11111111-1111-4111-8111-111111111111"
PUBLISHABLE_KEY = "sb_publishable_test"

# Runs inside the tmserver subprocess: accept an http://127.0.0.1 project, then start normally.
BOOTSTRAP = """
import sys
import tmserver.auth.config as config
import tmserver.auth.provider as provider
original = provider.validate_project_url
def loopback(value):
    if value.startswith("http://127.0.0.1:"):
        return value.rstrip("/") + "/auth/v1"
    return original(value)
provider.validate_project_url = config.validate_project_url = loopback
import tmserver.accounts.identities as identities
from pydantic import field_validator
class LoopbackIdentityLink(identities.IdentityLink):
    @field_validator("issuer")
    @classmethod
    def canonical_issuer(cls, value):
        return value
identities.IdentityLink = LoopbackIdentityLink
from tmserver.cli.serve import main
sys.argv = ["tmserver", *sys.argv[1:]]
main()
"""


def fake_supabase():
    from starlette.applications import Starlette
    from starlette.responses import JSONResponse, Response
    from starlette.routing import Route

    challenges, refresh_tokens, access_tokens = {}, set(), set()

    def issue():
        access, refresh = secrets.token_urlsafe(24), secrets.token_urlsafe(24)
        access_tokens.add(access)
        refresh_tokens.add(refresh)
        return JSONResponse(
            {
                "access_token": access,
                "refresh_token": refresh,
                "expires_in": 3600,
                "token_type": "bearer",
                "user": {"id": SUBJECT},
            }
        )

    async def authorize(request):
        query = request.query_params
        if query.get("provider") != "discord" or query.get("code_challenge_method") != "s256":
            return JSONResponse({"error_code": "bad_request"}, 400)
        code = secrets.token_urlsafe(16)
        challenges[code] = query["code_challenge"]
        location = f"{query['redirect_to']}?code={code}"
        # The body repeats the target so the scripted test browser can follow it.
        return Response(location, 302, headers={"Location": location})

    async def token(request):
        if request.headers.get("apikey") != PUBLISHABLE_KEY:
            return JSONResponse({"error_code": "no_api_key"}, 401)
        body = await request.json()
        grant = request.query_params.get("grant_type")
        if grant == "pkce":
            verifier = body["code_verifier"].encode()
            digest = base64.urlsafe_b64encode(hashlib.sha256(verifier).digest()).rstrip(b"=").decode()
            if challenges.pop(body["auth_code"], None) != digest:
                return JSONResponse({"error_code": "bad_code_verifier"}, 400)
            return issue()
        if grant == "refresh_token" and body["refresh_token"] in refresh_tokens:
            refresh_tokens.discard(body["refresh_token"])  # Refresh tokens rotate.
            return issue()
        return JSONResponse({"error_code": "refresh_token_not_found"}, 400)

    async def user(request):
        access = request.headers.get("authorization", "").removeprefix("Bearer ")
        if access in access_tokens:
            return JSONResponse({"id": SUBJECT})
        return JSONResponse({"msg": "invalid JWT"}, 401)

    return Starlette(
        routes=[
            Route("/auth/v1/authorize", authorize),
            Route("/auth/v1/token", token, methods=["POST"]),
            Route("/auth/v1/user", user),
        ]
    )


def serve_in_thread(app) -> int:
    import uvicorn

    listener = socket.socket()
    listener.bind(("127.0.0.1", 0))
    server = uvicorn.Server(uvicorn.Config(app, log_config=None, access_log=False, lifespan="off"))
    threading.Thread(target=lambda: asyncio.run(server.serve(sockets=[listener])), daemon=True).start()
    return listener.getsockname()[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        "--tmserver", type=Path, default=DEFAULT_TMSERVER, help="tmserver checkout (default: %(default)s)"
    )
    parser.add_argument("--tests", type=Path, help="built hosted_tests.exe; without it, serve for manual play")
    args = parser.parse_args()
    if not (args.tmserver / "src/tmserver").is_dir():
        print(f"Skipped: no tmserver checkout at {args.tmserver}")
        return 0

    supabase = f"http://127.0.0.1:{serve_in_thread(fake_supabase())}"
    with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as directory:
        config = Path(directory) / "auth.json"
        config.write_text(
            json.dumps(
                {
                    "supabase_url": supabase,
                    "publishable_key": PUBLISHABLE_KEY,
                    "public_origin": "https://tm.example.invalid",
                    "bindings": {SUBJECT: "EEEEEEEF"},
                }
            )
        )
        # tmserver runs the original rules through a Win32 bridge, so it needs its 32-bit Python.
        tmserver = args.tmserver.resolve()
        python = tmserver / "build/python-x86/python.exe"
        command = [str(python if python.exists() else sys.executable), "-c", BOOTSTRAP]
        command += ["--state", str(Path(directory) / "state.json"), "--game-port", "0", "--profile-port", "0"]
        command += ["--auth-config", str(config), "--auth-port", "0", "--auth-insecure-loopback"]
        command += ["--rules-image", str(tmserver / "var/native/TM_emulated_mapped.dll")]
        command += ["--rules-bridge", str(tmserver / "var/native/tm_original_rules.dll")]
        server = subprocess.Popen(
            command, cwd=args.tmserver, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
        )
        try:
            origin = None
            for line in server.stdout:
                print(f"[tmserver] {line.rstrip()}")
                if match := re.search(r"Launcher gateway ready on (127\.0\.0\.1:\d+)", line):
                    origin = f"http://{match[1]}"
                    break
            if origin is None:
                print("Error: tmserver did not start its gateway", file=sys.stderr)
                return 1
            threading.Thread(
                target=lambda: [print(f"[tmserver] {line.rstrip()}") for line in server.stdout], daemon=True
            ).start()
            if args.tests is None:
                print(f"Gateway ready. Run: tmloader.exe --hosted-server {origin} --hosted-insecure-loopback")
                print("Sign-in is approved automatically. Press Ctrl+C to stop.")
                server.wait()
                return 0
            environment = dict(os.environ, TMLOADER_GATEWAY_ORIGIN=origin)
            return subprocess.run([str(args.tests), "[.gateway]"], env=environment, check=False).returncode
        finally:
            # The venv python.exe is a launcher; end the interpreter it started too.
            subprocess.run(["taskkill", "/T", "/F", "/PID", str(server.pid)], capture_output=True, check=False)
            server.wait(10)


if __name__ == "__main__":
    sys.exit(main())
