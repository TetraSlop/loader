"""Opt-in development host preserves local accounts/rooms and cleans up services."""

import asyncio
import tempfile
import unittest
from contextlib import asynccontextmanager
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from serve_development import default_rooms, run
from tmserver.cli import serve
from tmserver.storage.json import writer_lease


class HostTests(unittest.IsolatedAsyncioTestCase):
    async def test_host_cancellation_releases_services_and_state_lease(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "online").mkdir()
            (root / "online/index.html").write_text("test", encoding="utf8")
            package_root = Path(serve.__file__).resolve().parents[2]
            state = root / "play/state.json"
            args = SimpleNamespace(
                server_dir=package_root,
                runtime_dir=root,
                state=state,
                rooms=None,
                port=0,
            )
            captured = {}
            actual_services = serve.running_services

            @asynccontextmanager
            async def services(options):
                self.assertEqual(options.accounts, 3)
                self.assertEqual(options.rooms, default_rooms(package_root))
                async with actual_services(options) as active:
                    captured["services"] = active
                    yield active

            with patch.object(serve, "running_services", services):
                task = asyncio.create_task(run(args))
                try:
                    for _ in range(100):
                        if (root / "online-host.pid").exists():
                            break
                        if task.done():
                            await task
                        await asyncio.sleep(0.02)
                    else:
                        self.fail("Host did not start")
                    active = captured["services"]
                    self.assertEqual(len(active.game.accounts), 3)
                    with (
                        self.assertRaisesRegex(OSError, "writer lease"),
                        writer_lease(state),
                    ):
                        pass
                finally:
                    task.cancel()
                    with self.assertRaises(asyncio.CancelledError):
                        await task
            self.assertFalse((root / "online-host.pid").exists())
            with writer_lease(state):
                pass
            with self.assertRaises(OSError):
                await asyncio.open_connection("127.0.0.1", active.game_port)

    def test_default_rooms_supports_external_source_layouts(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            expected = root / "examples/auto_rooms.example.json"
            expected.parent.mkdir()
            expected.write_text("{}", encoding="utf8")
            self.assertEqual(default_rooms(root), expected)
            self.assertEqual(default_rooms(root / "src"), expected)


if __name__ == "__main__":
    unittest.main()
