"""Fixed WebSocket-to-TCP routes for the browser runtime's two game services."""

import argparse
import asyncio
from contextlib import suppress
from pathlib import Path
from urllib.parse import urlsplit

from aiohttp import WSMsgType, web
from runtime_paths import runtime_root


def create_app(static_dir, *, game=("127.0.0.1", 51241), profile=("127.0.0.1", 51220)):
    root = Path(static_dir).resolve()
    routes = {"/tm/game": game, "/tm/profile": profile}

    @web.middleware
    async def local_host(request, handler):
        try:
            host = urlsplit("//" + request.host).hostname
        except ValueError:
            host = None
        if host not in ("127.0.0.1", "localhost", "::1"):
            raise web.HTTPForbidden(text="Local hosts only.")
        return await handler(request)

    app = web.Application(client_max_size=1024 * 1024, middlewares=[local_host])

    async def bridge(request):
        origin = request.headers.get("Origin")
        if origin and origin != str(request.url.origin()):
            raise web.HTTPForbidden(text="Use this host's browser page.")
        ws = web.WebSocketResponse(protocols=("binary",), max_msg_size=1024 * 1024)
        if not ws.can_prepare(request).ok:
            raise web.HTTPBadRequest(text="A binary WebSocket connection is required.")
        try:
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(*routes[request.path]), 10
            )
        except (OSError, asyncio.TimeoutError):
            raise web.HTTPBadGateway(
                text="The configured game service is unavailable."
            ) from None
        tasks = []
        try:
            await ws.prepare(request)

            async def to_tcp():
                async for message in ws:
                    if message.type == WSMsgType.BINARY:
                        writer.write(message.data)
                        await writer.drain()
                    elif message.type == WSMsgType.TEXT:
                        await ws.close(
                            code=1003, message=b"Binary protocol frames required"
                        )
                        return

            async def to_websocket():
                while data := await reader.read(65536):
                    await ws.send_bytes(data)

            tasks = [asyncio.create_task(to_tcp()), asyncio.create_task(to_websocket())]
            done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            for task in done:
                task.result()
        except (OSError, ConnectionError):
            pass  # Either peer may disconnect while its final bytes are in flight.
        finally:
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)
            writer.close()
            with suppress(OSError, asyncio.TimeoutError):
                await asyncio.wait_for(writer.wait_closed(), 5)
            if ws.prepared:
                await ws.close()
        return ws

    async def static(request):
        path = (root / (request.match_info["path"] or "index.html")).resolve()
        if not path.is_relative_to(root) or not path.is_file():
            raise web.HTTPNotFound()
        response = web.FileResponse(path)
        response.headers["Cache-Control"] = "no-cache"
        return response

    async def health(_request):
        return web.json_response(
            {"service": "tm-boxedwine-network", "routes": list(routes)}
        )

    app.router.add_get("/health", health)
    for route in routes:
        app.router.add_get(route, bridge)
    app.router.add_get("/{path:.*}", static)
    return app


def arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8767)
    parser.add_argument("--static-dir", type=Path, default=runtime_root() / "online")
    parser.add_argument(
        "--server",
        required=True,
        help="Separately deployed game/profile server hostname or IP address",
    )
    parser.add_argument("--game-port", type=int, required=True)
    parser.add_argument("--profile-port", type=int, required=True)
    args = parser.parse_args(argv)
    if not args.server.strip():
        parser.error("--server must name the separate game/profile server")
    if not all(1 <= port <= 65535 for port in (args.game_port, args.profile_port)):
        parser.error("game/profile ports must be between 1 and 65535")
    if not 0 <= args.port <= 65535:
        parser.error("browser host port must be between 0 and 65535")
    return args


def main():
    args = arguments()
    web.run_app(
        create_app(
            args.static_dir,
            game=(args.server, args.game_port),
            profile=(args.server, args.profile_port),
        ),
        host="127.0.0.1",
        port=args.port,
    )


if __name__ == "__main__":
    main()
