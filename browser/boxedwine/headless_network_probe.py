"""Run only the non-game Winsock probe in a disposable headless browser."""

import asyncio
import json
import os
import re
import tempfile
import zipfile
from pathlib import Path

from aiohttp import web
from network_gateway import create_app
from playwright.async_api import async_playwright
from runtime_paths import runtime_root
from tmserver.network.server import Server
from tmserver.profiles.service import ProfileServer

BASE = runtime_root()


async def main():
    browser_temp = BASE / "browser-temp"
    browser_temp.mkdir(exist_ok=True)
    os.environ["TEMP"] = os.environ["TMP"] = str(browser_temp)
    with zipfile.ZipFile(
        BASE / "online/socket-probe.zip", "w", zipfile.ZIP_DEFLATED
    ) as archive:
        archive.write(BASE / "socket_probe.exe", "socket_probe.exe")
    logs = []
    with tempfile.TemporaryDirectory(dir=BASE) as temp:
        temp = Path(temp)
        game = Server(log_path=temp / "game.jsonl", state_path=temp / "state.json")
        profile = ProfileServer(temp / "profile.jsonl", game)
        runner = None
        try:
            game_port, profile_port = await game.start(0), await profile.start(0)
            runner = web.AppRunner(
                create_app(
                    BASE / "online",
                    game=("127.0.0.1", game_port),
                    profile=("127.0.0.1", profile_port),
                )
            )
            await runner.setup()
            site = web.TCPSite(runner, "127.0.0.1", 0)
            await site.start()
            port = site._server.sockets[0].getsockname()[1]
            async with async_playwright() as playwright:
                browser = await playwright.chromium.launch(
                    channel="msedge",
                    headless=True,
                    args=["--enable-unsafe-swiftshader"],
                )
                try:
                    page = await browser.new_page()
                    page.on("console", lambda message: logs.append(message.text))
                    page.on(
                        "pageerror",
                        lambda error: logs.append("PAGE ERROR: " + str(error)),
                    )
                    await page.goto(f"http://127.0.0.1:{port}/?probe=1")
                    try:
                        await page.wait_for_function(
                            "() => {const t=document.getElementById('output').value"
                            ".replace(/\\x1b\\[[0-9;?]*[A-Za-z]/g,'');"
                            "return t.includes('PROBEPASS') || t.includes('PROBEFAIL');}",
                            timeout=60000,
                            polling=500,
                        )
                    finally:
                        output = await page.locator("#output").input_value()
                        (BASE / "headless-network-output.txt").write_text(
                            output, encoding="utf8"
                        )
                    cleaned = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", output)
                    assert "PROBEPASS" in cleaned, output[-3000:]
                    print(
                        "PASS: headless Wine Winsock reached actual game/profile TCP services."
                    )
                finally:
                    await browser.close()
        finally:
            if runner is not None:
                await runner.cleanup()
            await profile.stop()
            await game.stop()
            (BASE / "headless-network-console.json").write_text(
                json.dumps(logs, indent=2), encoding="utf8"
            )
            for name in ("game.jsonl", "profile.jsonl"):
                path = temp / name
                if path.exists():
                    (BASE / ("headless-network-" + name)).write_bytes(path.read_bytes())


if __name__ == "__main__":
    asyncio.run(main())
