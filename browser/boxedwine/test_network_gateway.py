"""Actual tmserver game/profile authentication through the byte bridge."""

import asyncio
import hashlib
import struct
import tempfile
import unittest
from pathlib import Path

import aiohttp
from aiohttp import web
from network_gateway import create_app
from tmserver.network.server import Server
from tmserver.profiles.service import ProfileServer
from tmserver.protocol import wire

# Disposable 256-bit legacy test key; no production secret or original DLL.
P, Q = 340282366920938463463374607431768211283, 340282366920938463463374607431768211223
N, D = P * Q, pow(65535, -1, (P - 1) * (Q - 1))
MODULUS = N.to_bytes(32, "little")


class Socket:
    def __init__(self, ws):
        self.ws, self.buffer = ws, bytearray()

    async def read(self, size=None, *, line=False):
        while (b"\n" not in self.buffer) if line else (len(self.buffer) < size):
            message = await asyncio.wait_for(self.ws.receive(), 10)
            if message.type != aiohttp.WSMsgType.BINARY:
                raise EOFError("TCP service closed")
            self.buffer.extend(message.data)
        if line:
            size = self.buffer.index(b"\n") + 1
        data = bytes(self.buffer[:size])
        del self.buffer[:size]
        return data

    async def send(self, data, fragment=0):
        for offset in range(0, len(data), fragment or len(data)):
            await self.ws.send_bytes(data[offset : offset + (fragment or len(data))])


class GatewayTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.game = Server(state_path=Path(self.temp.name) / "state.json")
        self.addAsyncCleanup(self.game.stop)
        self.profile = ProfileServer(Path(self.temp.name) / "profile.jsonl", self.game)
        self.addAsyncCleanup(self.profile.stop)
        game_port, profile_port = await self.game.start(0), await self.profile.start(0)
        self.runner = web.AppRunner(
            create_app(
                self.temp.name,
                game=("127.0.0.1", game_port),
                profile=("127.0.0.1", profile_port),
            )
        )
        await self.runner.setup()
        self.addAsyncCleanup(self.runner.cleanup)
        site = web.TCPSite(self.runner, "127.0.0.1", 0)
        await site.start()
        self.url = f"http://127.0.0.1:{site._server.sockets[0].getsockname()[1]}"
        self.client = aiohttp.ClientSession()
        self.addAsyncCleanup(self.client.close)

    async def connect(self, route):
        ws = await self.client.ws_connect(
            self.url + route, protocols=("binary",), origin=self.url
        )
        self.addAsyncCleanup(ws.close)
        return Socket(ws)

    async def login(self):
        sock = await self.connect("/tm/game")
        self.assertIn(b"NOTICE AUTH", await sock.read(line=True))
        # Reconnect to cover retirement of the first unauthenticated route.
        await sock.ws.close()
        sock = await self.connect("/tm/game")
        raw = await sock.read(line=True)
        self.assertEqual(wire.checksum_chars(raw[:-6]), raw[-6:-2])
        challenge = raw[:-6].split()[-1]
        await sock.send(
            wire.frame_line(b"USER x 8 * :" + wire.b64enc(MODULUS, pad=False)), 3
        )
        reply = (await sock.read(line=True))[:-6]
        encrypted = wire.b64dec(reply.split()[-1])[:32]
        block = pow(int.from_bytes(encrypted, "big"), D, N).to_bytes(32, "big")
        key = block[block.index(0, 2) + 1 :][::-1]
        cipher = await asyncio.to_thread(wire.SqBlowfish, key)
        iv = int.from_bytes(MODULUS[:8], "little")
        nick = next(iter(self.game.accounts))
        digest = hashlib.md5(challenge + self.game.accounts[nick]).hexdigest().encode()
        await sock.send(
            wire.frame_line(
                b"NICK " + nick + b":" + digest + b":browser-test", cipher, iv
            ),
            5,
        )
        self.assertIn(b" 422 ", wire.crypt64(cipher, iv, await sock.read(line=True)))
        return sock, cipher, iv

    async def test_encrypted_login_fragmented_ping_and_profile_records(self):
        sock, cipher, iv = await self.login()
        await sock.send(wire.frame_line(b"PING :browser-roundtrip", cipher, iv), 1)
        reply = wire.crypt64(cipher, iv, await sock.read(line=True))
        self.assertEqual(reply[:-6], b"PONG browser-roundtrip")
        for command, sub, size in ((0, 9, 8 + 0x88), (1, 3, 8 + 0x68)):
            profile = await self.connect("/tm/profile")
            opening = bytearray(40)
            opening[4] = 1
            await profile.send(opening, 1)
            opened = await profile.read(24)
            request = bytearray(40)
            request[:3] = bytes((2, command, sub))
            request[24:] = hashlib.md5(
                b"EEEEEEEFtm-local-test" + opened[20:24]
            ).digest()
            await profile.send(wire.crypt64(cipher, iv, request), 7)
            head = wire.crypt64(cipher, iv, await profile.read(24))
            count = struct.unpack_from("<I", head, 4)[0]
            tail = await profile.read(count)
            payload = wire.crypt64(cipher, iv, bytes(24) + tail)[24:]
            self.assertEqual((head[1], len(payload), payload[0]), (0, size, 1))
            await profile.ws.close()

    async def test_invalid_origin_route_and_text_are_not_forwarded(self):
        async with self.client.get(
            self.url + "/health", headers={"Host": "other.invalid"}
        ) as response:
            self.assertEqual(response.status, 403)
        with self.assertRaises(aiohttp.WSServerHandshakeError) as error:
            await self.client.ws_connect(
                self.url + "/tm/game", origin="https://other.invalid"
            )
        self.assertEqual(error.exception.status, 403)
        async with self.client.get(
            self.url + "/tm/arbitrary?host=127.0.0.1&port=1"
        ) as response:
            self.assertEqual(response.status, 404)
        async with self.client.get(self.url + "/tm/game") as response:
            self.assertEqual(response.status, 400)
        sock = await self.connect("/tm/game")
        await sock.read(line=True)
        await sock.ws.send_str("not binary")
        message = await asyncio.wait_for(sock.ws.receive(), 10)
        self.assertEqual((message.type, message.data), (aiohttp.WSMsgType.CLOSE, 1003))

    async def test_tcp_eof_closes_websocket(self):
        sock = await self.connect("/tm/game")
        await sock.read(line=True)
        await sock.send(b"invalid framing\r\n" * 3)
        message = await asyncio.wait_for(sock.ws.receive(), 10)
        self.assertEqual(message.type, aiohttp.WSMsgType.CLOSE)


if __name__ == "__main__":
    unittest.main()
