# Hosted play

Hosted mode connects to a public [tmserver](https://github.com/sruon/tmserver)
through its sign-in gateway instead of the raw game ports. Players sign in with
Discord in their normal browser; the server maps the verified identity to their
enrolled account. The contract is tmserver's `LOADER_AUTH_SPEC.md` (protocol 1).

## Using it

Set the server's HTTPS origin in `tmloader.ini` and run `tmloader.exe`:

```ini
runtime-dir=runtime
hosted-server=https://tm.example.com
```

A launcher window opens:

1. **Sign in with Discord** opens the browser. After approving, the browser
   returns to the loader and the tab can be closed. The sign-in is remembered
   in Windows Credential Manager for the next launch.
2. **Play** creates a play session and starts the game. If the account is
   already playing elsewhere, **Disconnect other session** takes over.
3. **Log out** revokes the play session and forgets the remembered sign-in.

`server`, the ports, `pol-id` and `password` are ignored in hosted mode; the
server supplies the account.

## How it works

| Step | Loader behavior |
|---|---|
| Discover | `GET /v1/auth/config`; only protocol 1 with the `discord` provider is accepted |
| Sign in | Supabase PKCE: random verifier, S256 challenge, a one-time loopback callback on `127.0.0.1` with a random path served by cpp-httplib, a five-minute limit, then `POST /auth/v1/token?grant_type=pkce` |
| Remember | The refresh token is stored in Credential Manager, keyed by server origin and Supabase project; refreshes are serialized and rotated tokens replace both |
| Play | `POST /v1/play/tickets` with the access token, then `POST /v1/play/sessions`; stream URLs must be on the server's origin |
| Tunnel | Loopback listeners replace the game and profile endpoints. Each native connection opens its own WebSocket with `Authorization: Bearer <play token>` and subprotocol `tm-stream-v1`, carrying binary chunks of at most 64 KiB unchanged |
| Renew | Five minutes before the play session expires, `POST /v1/play/renew`, refreshing the access token first when needed. Unavailable-service errors retry with backoff; anything else ends play |
| Exit | `DELETE /v1/play/session` |

Tokens and tickets never reach the game, `tmloader.ini`, the command line or
the log. HTTPS certificates are validated by Windows, TLS 1.2+ is required and
redirects are never followed. A remote stream closure (including `4401`
expiry) reaches the game as a normal disconnect; buffered data is never
replayed into a new stream.

The code is in `src/hosted/`: `http` (WinHTTP client and WebSocket), `auth`
(PKCE, Supabase tokens, play API, Credential Manager), `tunnel` and `session`
(sign-in state, play session and renewal). `src/ui/launcher.cpp` is the window.

## Development

`--hosted-insecure-loopback` allows `http://127.0.0.1:PORT` endpoints for local
testing; hosted mode otherwise requires HTTPS.

`tests/hosted/gateway_harness.py` runs a real tmserver gateway against a fake
Supabase that issues real PKCE codes and rotating refresh tokens. It needs:

- a tmserver checkout with its `auth` and `postgres` extras installed, by
  default `../tmserver` beside this one (`--tmserver` names another), set up as
  tmserver's `DATABASE.md` describes for the 32-bit game server;
- a scratch PostgreSQL database named by `TM_TEST_DATABASE_URL`. The harness
  runs tmserver's schema migration on it and tmserver writes accounts to it, so
  do not use a database you want to keep. It reaches tmserver as
  `TM_DATABASE_URL`, never on a command line.

It prints why and exits successfully when there is no checkout, no
`TM_TEST_DATABASE_URL`, or the database does not accept connections:

```powershell
docker run --detach --rm --name tm-gateway-test --env POSTGRES_PASSWORD=PASSWORD --env POSTGRES_DB=tm_gateway_test --publish 127.0.0.1:5432:5432 postgres:16
$env:TM_TEST_DATABASE_URL = 'postgresql+psycopg://postgres:PASSWORD@127.0.0.1:5432/tm_gateway_test'

# Automated: sign-in, tunnel, remembered login, conflict/takeover, logout
..\tmserver\.venv\Scripts\python tests\hosted\gateway_harness.py --tmserver ..\tmserver `
  --tests build\tests\RelWithDebInfo\hosted_tests.exe

# Manual: keeps the servers running; sign-in is approved automatically
..\tmserver\.venv\Scripts\python tests\hosted\gateway_harness.py --tmserver ..\tmserver
```

Live Discord sign-in needs a configured Supabase project; see tmserver's
`AUTHENTICATION.md`. Its redirect allowlist must include the loader's callback
pattern, `http://127.0.0.1:*/callback/*`.
