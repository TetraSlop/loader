"""Bounded packaged-client validation inside headless BoxedWine, never on Windows."""

import argparse
import asyncio
import json
import os
import tempfile
from pathlib import Path

from aiohttp import web
from network_gateway import create_app
from playwright.async_api import async_playwright
from runtime_paths import runtime_root
from tmserver.network.server import Server
from tmserver.profiles.service import ProfileServer

BASE = runtime_root()


def events(path):
    if not path.exists():
        return []
    return [
        json.loads(line)
        for line in path.read_text(encoding="utf8").splitlines()
        if line
    ]


async def run(check_only, original_art=False, observe_seconds=45, enter_game=False):
    browser_temp = BASE / "browser-temp"
    browser_temp.mkdir(exist_ok=True)
    os.environ["TEMP"] = os.environ["TMP"] = str(browser_temp)
    name = (
        "headless-client-check"
        if check_only
        else "headless-client-original"
        if original_art
        else "headless-client"
    )
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
                    page = await browser.new_page(
                        viewport={"width": 1200, "height": 1000}
                    )
                    console = (BASE / (name + "-console.txt")).open(
                        "w", encoding="utf8"
                    )

                    def record(message):
                        console.write(message + "\n")
                        console.flush()

                    page.on("console", lambda message: record(message.text))
                    page.on(
                        "pageerror", lambda error: record("PAGE ERROR: " + str(error))
                    )
                    await page.goto(
                        f"http://127.0.0.1:{port}/?check={int(check_only)}"
                        + ("&artwork=original" if original_art else "&artwork=hd")
                    )
                    try:
                        for _ in range(120):
                            output = await page.locator("#output").input_value()
                            if (
                                check_only
                                and "No DLL loaded or game launched" in output
                            ):
                                print(
                                    "PASS: packaged loader verified its runtime inside headless Wine."
                                )
                                break
                            if not check_only:
                                logged_in = any(
                                    e["event"] == "authenticated"
                                    for e in events(temp / "game.jsonl")
                                )
                                profiles = any(
                                    e["event"] == "request"
                                    for e in events(temp / "profile.jsonl")
                                )
                                if logged_in and profiles:
                                    print(
                                        "PASS: packaged tmloader authenticated with actual game and profile services."
                                    )
                                    await asyncio.sleep(observe_seconds)
                                    if enter_game:
                                        await page.screenshot(
                                            path=str(
                                                BASE / (name + "-before-input.png")
                                            )
                                        )
                                        # Original client 1.06.011: observed Yes button
                                        # in the 640x480 Start game confirmation.
                                        canvas = await page.locator(
                                            "#canvas"
                                        ).bounding_box()
                                        await page.mouse.move(
                                            canvas["x"] + canvas["width"] * 250 / 640,
                                            canvas["y"] + canvas["height"] * 356 / 480,
                                        )
                                        await page.mouse.down()
                                        await asyncio.sleep(0.7)
                                        await page.mouse.up()
                                        await asyncio.sleep(15)
                                    break
                            if "tmloader:" in output or "PAGE ERROR" in output:
                                raise AssertionError(output[-3000:])
                            await asyncio.sleep(2)
                        else:
                            raise AssertionError(
                                "Packaged client did not reach the required boundary in240s."
                            )
                    finally:
                        output = await page.locator("#output").input_value()
                        (BASE / (name + "-output.txt")).write_text(
                            output, encoding="utf8"
                        )
                        await page.screenshot(
                            path=str(BASE / (name + ".png")), full_page=True
                        )
                        files = await page.evaluate("""() => {
                          const result = {};
                          if (typeof FS === 'undefined') return result;
                          function scan(path, depth) {
                            if (depth > 18) return;
                            try { for (const name of FS.readdir(path)) {
                              if (name === '.' || name === '..') continue;
                              const full = path + '/' + name;
                              if (FS.isDir(FS.stat(full).mode)) scan(full, depth + 1);
                              else if (name === 'tmloader.log') result[full] = FS.readFile(full, {encoding:'utf8'});
                            }} catch (_) {}
                          }
                          scan('/root', 0);
                          return result;
                        }""")
                        (BASE / (name + "-loader.json")).write_text(
                            json.dumps(files, indent=2), encoding="utf8"
                        )
                        console.close()
                finally:
                    await browser.close()
        finally:
            if runner is not None:
                await runner.cleanup()
            await profile.stop()
            await game.stop()
            for filename in ("game.jsonl", "profile.jsonl"):
                path = temp / filename
                if path.exists():
                    (BASE / (name + "-" + filename)).write_bytes(path.read_bytes())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--original-art", action="store_true")
    parser.add_argument("--observe-seconds", type=int, default=45)
    parser.add_argument(
        "--enter-game",
        action="store_true",
        help="Confirm the observed Start game prompt in the disposable browser",
    )
    args = parser.parse_args()
    asyncio.run(
        run(args.check, args.original_art, args.observe_seconds, args.enter_game)
    )
