# polcore in the browser

Research for the browser client: what TM.dll needs from `polcore_native.dll` (the PlayOnline Viewer core), and
whether to run polcore itself in the interpreter or reimplement the parts TM uses. Date: 2026-11-22. Scripts are in
`browser/tools/polcore/`; they read the user's own files from environment variables and commit nothing from them.

## Recommendation

**Run polcore itself (emulate), unmodified, at the Viewer base `0x10400000`.** Reimplement nothing in the common
table. The host does three things around it:

1. **The Viewer's set-up**, as tmloader does today: create the core object, set the area code, parameters,
   languages, registry root and install folder, the POL ID and account, initialise the INET and profile layers,
   seed the profile cipher after the first Game IRC login, and preload handles, characters and status. Every step
   calls polcore's own code.
2. **Winsock on WebSockets.** polcore's sockets on TM's paths are non-blocking TCP polled with `select` and a zero
   timeout. Each socket maps onto one tmserver browser stream.
3. **Win32 stand-ins** for the 67 imports polcore called in the measured run (71 on TM's paths counting static
   reach), most of which TM needs anyway.

Measured under Unicorn with the scripts here: the packed polcore unpacks and relocates itself at `0x10400000` byte
for byte; COM creation, tmloader's whole Viewer set-up and the TM boot entries run; Game IRC logs in to a real
tmserver (RSA key exchange, encrypted NICK, `authenticated` in the server log); and the handle, character and
status profile loads all return 1, over the same loopback sockets a browser build would replace with WebSockets.

Reimplementing would mean recovering about 500 polcore functions (25,400 instructions, a static lower bound) with
little documentation. Emulating needs about 3.5–4 person-weeks, mostly socket and thread stand-ins the interpreter
needs regardless. See [Effort](#effort).

## How it was measured

| Script | What it does |
|---|---|
| `common.py` | Loads the images. polcore is unpacked in memory (POL1 is plain LZSS, see below); TM comes from its unpacked mapped image. `python common.py` self-checks the LZSS decoder |
| `scan_tm.py` | Every TM call into the Viewer: common-table entries through the pointer at `0x1024DFE8`, including tail-jump thunks, and core-interface slots. Records the calling function, its callers when it is a thunk, and the strings it references |
| `scan_polcore.py` | Static closure of every table entry and core slot: functions reached through direct calls, jumps and switch tables, instruction counts, imports and strings. Stops at the statically linked CRT and ATL (from `+4F530`), which a host build supplies itself |
| `emu.py`, `hle.py` | A small Win32 user-mode harness for Unicorn: our loader (relocations, import stubs that log every call), cooperative guest threads, and stand-ins for files, registry, synchronisation and winsock |
| `run_polcore.py` | Runs polcore at `0x10400000` through DllMain, COM, tmloader's Viewer set-up, the TM boot entries and, with `--irc`, Game IRC and the profile bootstrap against a running tmserver |
| `entries.py`, `report.py` | Our annotations of each entry, merged with the measurements into the table below |

TM's own source-file and function names, kept as strings in its callers, name many entries: `mg.c sqMgOpen`,
`sqMgPoll`, `sqMgReadFile`, `pol_qm\FriendList.cpp`. The PS2 decomp's `sqPlayOnline*` symbols supply the
vocabulary for the rest (`LoadOptionCheck`, `EnvVariableInit`, `VulgarMapDictionary`). Names in brackets in the
table are probable, not confirmed.

## Findings

### Loading

- POL1 is not a protector. The section holds `.text` compressed with LZSS (control byte MSB first, 12-bit
  distance, length 3–18). The stub at the entry point decompresses it, applies `.text` relocations and jumps to the
  real DllMain at `+5347E`.
- The `.text` relocations are hidden from the OS. They sit in the first blocks of `.reloc` (`+44B000..+44EF7C`),
  before the range the data directory names, so the OS loader never applies them and only the stub does. The stub
  gets the load delta from a relocated `mov edi, 0`. Our loader should behave like the OS: apply the directory's
  relocations, then run the stub. A host that unpacks `.text` itself instead must also apply those hidden blocks.
- At `0x10400000` the result matched the reference unpack, relocated by the same delta, over all of `.text`
  (0 differing bytes). Both DLLs prefer `0x10000000`, so this relocation is required, and tmloader already relies on
  it natively.
- The common table is static data at `+6FBE8`: 1200 slots, 714 filled (0–124 empty), 660 distinct targets. No
  entry changed at run time. The table's location is what tmloader checks.

### Which entries and slots each path uses

TM calls **145 distinct table entries from 371 call sites**, plus core slots. 144 are real entries; index 1506 lies
past the table and is a TM thunk that no working path reaches. The spike's list had four indices (0, 2, 3, 9)
that are really vtable calls, and missed three entries that TM reaches only through tail-jump thunks (443, 696,
1167).

| Path | Entries | Evidence |
|---|---|---|
| Boot, before `Direct3DCreate8` | core 0, 26, 4, 3, 1, 7; table 824, 966, 288, 470–473, 907, 908, 126, 362 | Measured: the spike's boot run (the order of calls) and this run (all return as the game expects with the real `env.dat` and `option.bin`) |
| Host set-up (no Viewer) | core 27, 5, 7; table 965, 420, 367, 125, 932, 854, 851; internals `+FE20`, `+127A0`, `+1EB00`/`+1EA60`, `+1E930`, `+19BA0` | Measured. TM never calls these: pol.exe normally does |
| Login | 132, 133, 153 (Game IRC); host: `+1A170`, `+1E980`, then 171/172, 175/176, 193/194 | Measured against tmserver: 133 returns 1 after 12 polls with 153 pumping; each profile check returns 1 |
| Lobby | 144, 152 (channels), 153, 313 and 352–355 (send), 345/349 (zone and room files `b/g/ZL`, `b/g/RL%03d`), 132/133 again to the zone's host, 966, 190 | TM callers (`sqMgJoinChannel`, `sqMgReadFile`), tmserver's protocol notes |
| Match | 153, 313, 352–355, 145/146, 157, 831/832, 843/844, 190, 966 | TM's service-message builders (`@GameENC=`, `@TrID=`) call 190 and 966; ingress is the polled queue |
| Profile and save files | 165–207 lists, 344–350 files, 173/177/178 record queries | Request/check pairs; the check side drives the profile connection |
| Options | 907–913 | 908 measured; 909/910 write `pub/home00/option.bin` |
| Friends and POL messages | 271–302, 335/337, 1152, 186–188 | TM `pol_qm\FriendList.cpp`, the `O/m/` and `Contents%04d` message paths |
| Chat | 362, 363 (offensive-word filter), 689, 694, 967, 968, 774 (IME) | TM callers with `chat2` |
| Harassment report mail | 128–130 (MD5), 221–247 (raw sockets), 259, 443, 444, 653–699, 1164, 1167 | TM's own SMTP-style mailer with CRAM-MD5 over polcore's socket API. tmloader already refuses its connections |

TM never calls 171, 175 or 193 itself. pol.exe normally preloads handles, characters and status before it starts
the game, so the browser host must do it, as tmloader does.

### What polcore uses

- **Imports.** Of polcore's 200 imports, the measured run called 67 (2,989 calls). Static reach from TM's entries
  and the core slots TM calls adds `Sleep`, `OutputDebugStringA` and two IME calls, 71 in all. The rest belong to
  Viewer code that TM never reaches.
- **Threads.** Worker threads are started through the CRT's `_beginthreadex`, often one per poll: 13 threads in
  the run, all short-lived (option and `env.dat` loads at `+2EA80`, `+2ED10`, `+2EF10`, `+2EFE0`; profile
  connections at `+10A50`). TM polls with `Sleep(1)` between checks, so `Sleep` must yield to them. polcore guards
  its state with mutexes: 537 `WaitForSingleObject`/`ReleaseMutex` pairs, all with an infinite timeout. Since the
  scheduler preempts at the end of a slice, mutexes need real ownership. A cooperative shortcut would deadlock or
  corrupt state.
- **Winsock.** Only the non-blocking polled style: `socket`, `bind` (local port 0), `ioctlsocket(FIONBIO)`,
  `setsockopt`, `connect` (expects `WSAEWOULDBLOCK`), `select`, `__WSAFDIsSet`, `getsockopt(SO_ERROR)`, `send`,
  `recv`, `closesocket`, `WSAGetLastError`. All 54 `select` calls had a zero timeout, so no guest thread ever blocks
  in winsock. polcore's `WSAEventSelect`/`WSAWaitForMultipleEvents` loop (`+3C285`), its UDP code (`sendto`,
  `recvfrom`) and its `listen`/`accept` code are not reachable from TM's entries. It never calls `WSAAsyncSelect`.
  `gethostbyname` is used when the game passes host names (`gi000.pol.com`, `pp%03d.pol.com`, a zone's host);
  tmloader resolves them to the server, and the run passed addresses directly.
- **GDI, hooks, DirectInput.** Only core slots 19 (`LoadBitmapA`, `BitBlt` into a memory DC), 20 and 21
  (`SetWindowsHookExA`) and code that TM never reaches. TM calls none of them. No stand-ins needed.
- **IME.** Entry 774 only: `ImmGetOpenStatus` and `ImmGetConversionStatus`. Answer "closed" until the browser maps
  composition events.
- **Registry.** None on TM's paths: the run made no registry call. polcore's registry code is behind core slots 9
  and 12, which TM does not call. TM's own key (`SOUND`, `B_00OK`...) is TM's piece.
- **Files.** Relative to the folder set by entry 125: `data\utf\env.dat` and `pub\home00\option.bin`, opened for
  reading through the CRT (which needs `GetFileType` to answer `FILE_TYPE_DISK`). 909/910 write `option.bin`.
- **Other.** `GetCurrentHwProfileA` (48 calls during key generation, as entropy), `CoCreateGuid` (Game IRC open),
  `GetTickCount`, `GetSystemTimeAsFileTime`, `timeGetTime`, and the CRT's TLS, heap and locale calls.

### Encryption

All of it is inside polcore and runs as is; the host never handles keys except to call polcore's own seeding
function. It matches tmserver's `protocol/wire.py`:

- Game IRC: a 25-byte challenge in polcore's base32; `USER` carries the client's 256-bit RSA modulus in its
  base64; the server's 300 reply wraps the 8-byte session key with exponent 65535; `SqBlowfish` uses an MD5-derived
  key schedule and per-line OFB (CR and LF pass through); every line ends with a four-character checksum; `NICK`
  carries the scrambled POL ID and MD5(challenge + password).
- Profile: 40-byte request headers with MD5(ID + password + nonce), a cipher reset at each header, and a context of
  0x68 bytes that points to a separate 0x1000-byte matrix. tmloader copies both after the first Game IRC login,
  because Game IRC slots are reused after a match.

**Cost:** the RSA key pair is generated in `IClassFactory::CreateInstance`, which ran about **520 million
instructions**. At the interpreter's expected 100 MIPS that is about 5 seconds of start-up. About 84% of it is in four
leaf routines: `+2CFA0` (43 instructions), `+2CA20` (46, a 256-bit add), `+2C1D0` (90) and the CRT's `+53940`. Host
stand-ins for those, checked against the emulated results, remove most of it. Or run CreateInstance behind the
loading screen.

### Emulate or reimplement

| | Emulate polcore | Reimplement the entries TM uses |
|---|---|---|
| Code to write | Win32 stand-ins (67–71 imports, mostly shared with TM), the WebSocket winsock, the host set-up (a port of about 150 lines of tmloader) | 144 entries over about 500 functions and 25,400 instructions (static lower bound), thread procedures, ticket state machines, the option and `env.dat` formats, the message, friend and file services, the IRC client and its crypto, the offensive-word filter |
| Semantics | Already right: it is the original code | Mostly unknown; each entry needs reverse engineering and an oracle |
| Proven | Boot, login and the profile bootstrap, end to end against tmserver | Nothing |
| Risk | Thread and mutex semantics, key-generation speed | Long tail of subtle failures in UI paths nobody exercises (friends, messages, mail) |
| Pins | polcore's hash (already pinned) and eight internal function addresses the host calls | TM's call sites only |

A hybrid stays possible: any entry can later get a host stand-in by replacing its table pointer, as tmloader
already does for 132, 133 and 349. Nothing measured calls for one.

### What the Viewer provides that the browser must supply

| What | Where it comes from in the browser |
|---|---|
| Account identity: POL ID and legacy password | tmserver's play session (`legacy.pol_id`, `legacy.password`); set through 854/851, the profile login and the Game IRC open arguments before the game caches it |
| Registry | Nothing for polcore. TM's key comes from the virtual registry (TM's piece) |
| Install folder, languages, area code, command line | Constants as in tmloader: area 1, languages 1, `SOFTWARE\PlayOnlineUS`, `" /game DdcMLbB"`, the virtual runtime root |
| `data\utf\env.dat`, `pub\home00\option.bin` | The user's runtime files; `pub/` must persist (OPFS), since saving Options writes `option.bin` |
| Handle, character and status preload | The host calls 171, 175, 193 and their checks after the first Game IRC login |
| Profile endpoint and cipher seed | The host calls polcore's internals, as tmloader's `platform/network.cpp` does |
| Server address | Any host name or address the game connects to; the port picks the stream (below) |
| Hardware profile, GUIDs, clocks | Synthetic values |

### Sockets on tmserver's browser streams

Against tmserver PR #19 (`docs/LOADER_AUTH_SPEC.md`, "Browser clients"):

| Winsock call | Stand-in |
|---|---|
| `gethostbyname(*.pol.com, zone host)` | One fixed synthetic address; the name is not used |
| `socket`, `bind`, `ioctlsocket`, `setsockopt` | A socket record; nothing on the wire |
| `connect` to port 51241 | Open `new WebSocket(streams.game, "tm-stream-v1")`, `binaryType = "arraybuffer"`, send the play-session token as the first text message; return `WSAEWOULDBLOCK` |
| `connect` to the profile port | The same on `streams.profile`. The run used one game stream and three profile streams, one at a time; the server allows four |
| `connect` to any other port (mailer) | Refuse, as tmloader does |
| `select` (always a zero timeout) | Writable once the WebSocket is open; readable when bytes are queued or it closed; never blocks |
| `getsockopt(SO_ERROR)` | 0 once open; `WSAECONNREFUSED` after a failed upgrade or a 1008 close (`invalid_token`, `invalid_session`, `channel_busy`, `auth_timeout`) |
| `send` | `ws.send` in chunks of at most 65,536 bytes; return the full length |
| `recv` | From the queue; 0 after close; `WSAEWOULDBLOCK` when empty |
| `closesocket` | `ws.close()` |

The bytes are polcore's own encrypted stream, unchanged. WebSocket events arrive on the JavaScript event loop, so
the machine must return to it every frame for the queues to fill. A nonzero `select` timeout, if any path has one,
should park the thread on a `Waitable` for socket readiness.

## Entry table

Groups are explained in `browser/tools/polcore/entries.py`. Paths: B boot, L login, Y lobby, M match, P profile and
save files, O options, F friends and POL messages, C chat, R harassment report mail. "polcore insns" is the entry's
static closure without the CRT, a lower bound. "Reaches" lists sockets, thread creation and IME found in that
closure. "Run" marks entries executed in the measured run. Verdict: **E** run polcore's code; **E\*** run it, and
the host adds something around it (argument or socket mapping, file persistence, the set-up before it).

| # | Group | What it does | Paths | TM sites | polcore insns | Reaches | Run | Verdict |
|---:|---|---|---|---:|---:|---|---|---|
| 126 | text | GetDirectoryName(id, buffer, size) [GetDirectoryName]; boot asks for id 0x13 | B P C | 8 | 1021 |  | yes | E |
| 128 | md5 | MD5 init (writes the standard IV) | R | 1 | 9 |  |  | E |
| 129 | md5 | MD5 update | R | 1 | 980 |  |  | E |
| 130 | md5 | MD5 final | R | 1 | 1051 |  |  | E |
| 132 | irc | Game IRC open (sqMgOpen): host, POL ID, password, client version, mode 8, callback block, cipher storage, queue storage, queue count. Returns a slot | L Y M | 1 | 1270 | thread | yes | E* |
| 133 | irc | Game IRC open check (sqMgOpenCheck): connects, then reports 1 once NICK is accepted | L Y M | 1 | 3275 | net thread | yes | E* |
| 134 | irc | Game IRC open-failure path (from sqMgOpenCheck) | L Y | 2 | 351 | thread |  | E |
| 135 | irc | Game IRC close | Y M | 2 | 372 | thread |  | E |
| 144 | irc | join channel (sqMgJoinChannel): slot, channel, 0 | Y M | 1 | 715 |  |  | E |
| 145 | irc | command request check (sqMgCommandReqCheck) | Y M | 1 | 93 |  |  | E |
| 146 | irc | command request check, second form (sqMgCommandReqCheck) | Y M | 1 | 74 |  |  | E |
| 152 | irc | part channel (sqMgPartChannel) | Y M | 1 | 627 |  |  | E |
| 153 | irc | poll (sqMgPoll): select(0) on the slot, send and receive, decrypt, queue messages | L Y M | 3 | 5504 | net | yes | E |
| 155 | irc | slot state query (sqMgOpenCheck, cp.c) | L Y M | 2 | 111 |  |  | E |
| 157 | irc | channel-protocol open check (sqMgCpOpenCheck) | Y M | 1 | 77 |  |  | E |
| 165 | list | list request (ticket), option GOPN1_HF context | P F | 2 | 329 |  |  | E |
| 166 | list | check for 165; opens the profile connection | P F | 2 | 4859 | net thread |  | E |
| 167 | list | list entry query | P F | 2 | 119 |  |  | E |
| 169 | list | list entry query | P F | 1 | 105 |  |  | E |
| 173 | list | handle or character record query, 20 call sites [GetHandleNameInfo] | L P F | 20 | 80 |  |  | E |
| 174 | list | record update | P | 2 | 327 |  |  | E |
| 177 | list | record query, 8 call sites [GetCharacterInfo] | L P | 8 | 81 |  |  | E |
| 178 | list | record query, 6 call sites | L P | 6 | 127 |  |  | E |
| 179 | list | list request (ticket) | P | 1 | 379 |  |  | E |
| 180 | list | check for 179 | P | 1 | 4191 | net thread |  | E |
| 181 | list | list request (ticket) | P F | 6 | 380 |  |  | E |
| 182 | list | check for 181 | P F | 6 | 4368 | net thread |  | E |
| 183 | list | record query | P | 5 | 236 |  |  | E |
| 184 | list | record query | P | 1 | 179 |  |  | E |
| 186 | list | friend list query (friend UI) | F | 1 | 246 |  |  | E |
| 187 | list | friend list query (friend UI) | F | 1 | 358 |  |  | E |
| 188 | list | friend list query (friend UI) | F | 2 | 246 |  |  | E |
| 189 | list | state getter under the profile lock | P | 1 | 53 |  |  | E |
| 190 | list | state getter under the profile lock, 28 sites in service-message builders (@ShReq, @TrID, @EInitReq) [GetActiveCharacterNumber] | L Y M | 28 | 53 |  |  | E |
| 195 | list | list request (ticket) | P | 6 | 479 |  |  | E |
| 196 | list | check for 195 | P | 5 | 5103 | net thread |  | E |
| 197 | list | record update | P | 1 | 269 |  |  | E |
| 198 | list | record update | P | 3 | 261 |  |  | E |
| 199 | list | status query, 17 sites | P F | 17 | 256 | thread |  | E |
| 204 | list | state getter under the profile lock | P | 1 | 53 |  |  | E |
| 206 | list | state getter under the profile lock | P F | 5 | 53 |  |  | E |
| 207 | list | state getter under the profile lock | P | 1 | 53 |  |  | E |
| 221 | sock | socket layer start (resolver thread) | R | 1 | 340 | thread |  | E* |
| 222 | sock | socket layer stop | R | 1 | 338 |  |  | E* |
| 231 | sock | TCP connect (socket, ioctlsocket, connect) | R | 1 | 601 | net |  | E* |
| 232 | sock | connect check (select, SO_ERROR) | R | 1 | 338 | net |  | E* |
| 236 | sock | close | R | 1 | 69 | thread |  | E* |
| 244 | sock | send request | R | 1 | 82 |  |  | E* |
| 245 | sock | send check (select, send) | R | 1 | 321 | net |  | E* |
| 246 | sock | receive request | R | 1 | 81 |  |  | E* |
| 247 | sock | receive check (select, recv) | R | 1 | 303 | net |  | E* |
| 259 | mail | mailer encode helper | R | 1 | 12 |  |  | E |
| 271 | msg | message request setup (O/m/ folder) | F | 1 | 71 |  |  | E |
| 272 | msg | message request setup | F | 1 | 79 |  |  | E |
| 273 | msg | message list request (ticket) | F | 1 | 968 |  |  | E |
| 274 | msg | check for 273 | F | 3 | 4472 | net thread |  | E |
| 275 | msg | message read request (ticket) | F | 2 | 645 |  |  | E |
| 276 | msg | check for 275 | F | 3 | 4954 | net thread |  | E |
| 277 | msg | message send request (ticket) | F | 5 | 315 | thread |  | E |
| 278 | msg | message delete request (ticket) | F | 1 | 279 |  |  | E |
| 279 | msg | check for 278 | F | 3 | 4634 | net thread |  | E |
| 280 | msg | message count request (ticket) | F | 2 | 606 |  |  | E |
| 281 | msg | check for 280 | F | 3 | 4118 | net thread |  | E |
| 282 | msg | message header flag (bit 15 of +0x3E) | F | 2 | 5 |  |  | E |
| 283 | msg | message state reset | F | 5 | 58 |  |  | E |
| 284 | msg | message file name (Contents%04d) | F | 4 | 1264 |  |  | E |
| 285 | msg | message file name (Contents%04d) | F | 1 | 1264 |  |  | E |
| 286 | msg | message file name (Contents%04d) | F | 1 | 1264 |  |  | E |
| 287 | msg | message file name (Contents%04d) | F | 1 | 1264 |  |  | E |
| 288 | msg | store a value, return the argument (called at boot from TM +190D00) | B F | 6 | 9 |  | yes | E |
| 291 | msg | profile file query | F | 2 | 228 |  |  | E |
| 292 | msg | profile file request | F | 1 | 288 |  |  | E |
| 293 | msg | friend-list file op (FriendList.cpp) | F | 1 | 96 |  |  | E |
| 294 | msg | friend-list file op | F | 2 | 521 |  |  | E |
| 295 | msg | friend-list file op | F | 3 | 117 |  |  | E |
| 296 | msg | friend-list file op | F | 1 | 652 |  |  | E |
| 298 | msg | friend-list file op | F | 1 | 22 |  |  | E |
| 299 | msg | friend-list file op | F | 1 | 17 |  |  | E |
| 301 | msg | friend-list file request (ticket) | F | 1 | 549 |  |  | E |
| 302 | msg | check for 301 | F | 1 | 4765 | net thread |  | E |
| 313 | irc | send a message to a target (_writeMsg): target ID halves, payload | L Y M | 5 | 11 |  |  | E |
| 314 | irc | message receive helper (cp.c) | Y M | 1 | 72 |  |  | E |
| 335 | msg | message service query (TM message IDs 37036..37049) | F | 1 | 95 |  |  | E |
| 337 | msg | message service state, 5 TM functions | Y F | 2 | 251 | thread |  | E |
| 344 | file | delete file request (sqMgDeleteFile2) | P | 1 | 277 |  |  | E |
| 345 | file | read file request (sqMgReadFile, sqMgReadFileOffset): b/g/ZL, b/g/RL%03d, U/g/TM0DataFile | L Y P | 1 | 298 |  |  | E |
| 349 | file | read file check (sqMgReadFileCheck) | L Y P | 1 | 4886 | net thread |  | E |
| 350 | file | delete file check (sqMgDeleteFileCheck2) | P | 1 | 4050 | net thread |  | E |
| 352 | irc | own 64-bit account ID (returns the encoded POL ID) | L Y M | 5 | 3 |  |  | E |
| 353 | irc | message header helper (_writeMsg) | L Y M | 2 | 61 |  |  | E |
| 354 | irc | ID scramble helper (_writeMsg) | L Y M | 1 | 57 |  |  | E |
| 355 | irc | XOR an ID with the own encoded ID [__sqChangeCryptPolId] | L Y M | 1 | 7 |  |  | E |
| 362 | text | map the offensive-word dictionary (vulgar2.dic) [VulgarMapDictionary]; 13 arguments | B C | 1 | 395 |  | yes | E |
| 363 | text | replace offensive words in a string [VulgarReplaceOffensiveWord] | C | 4 | 959 |  |  | E |
| 441 | text | string helper | P | 1 | 5 |  |  | E |
| 443 | mail | returns a constant string (mailer charset) | R | 1 | 2 |  |  | E |
| 444 | mail | encoder used by the CRAM-MD5 and UTF-8 mail code | R | 1 | 8 |  |  | E |
| 470 | env | load env.dat variables, starts a worker thread [EnvVariableInit] | B | 2 | 1290 |  | yes | E |
| 471 | env | check for 470 [EnvVariableInitChk]; measured 1 after 5 polls | B | 2 | 1304 |  | yes | E |
| 472 | env | look up a variable (HARASSMENT_MAILADDRESS) [GetEnvVariable] | B R | 2 | 47 |  | yes | E |
| 473 | env | copy its value [GetEnvVariableChk]; measured 1 | B R | 2 | 145 |  | yes | E |
| 474 | env | release env.dat [EnvVariableUnInit] | B | 1 | 92 |  |  | E |
| 475 | env | variable lookup for the profile screen | P | 4 | 144 |  |  | E |
| 477 | env | icon or resource file name (%s%03d%s%s, guild) [GetIconFileName] | P Y | 6 | 1183 |  |  | E |
| 478 | mail | returns -2056 | R | 1 | 2 |  |  | E |
| 653 | mail | mailer helper | R | 1 | 39 |  |  | E |
| 655 | mail | mailer helper | R | 1 | 59 |  |  | E |
| 657 | mail | mailer helper | R | 1 | 61 |  |  | E |
| 659 | mail | mailer helper | R | 1 | 34 |  |  | E |
| 688 | text | time query | C R | 1 | 77 |  |  | E |
| 689 | text | time query, chat timestamps | C | 3 | 21 |  |  | E |
| 692 | text | date and number formatting input | P C | 9 | 40 |  |  | E |
| 694 | text | chat time helper | C | 1 | 112 |  |  | E |
| 696 | text | GetTickCount pass-through | R | 1 | 1 |  |  | E |
| 697 | text | returns a server-time global | P C | 5 | 2 |  |  | E |
| 698 | text | returns a server-time global | P | 1 | 2 |  |  | E |
| 699 | text | time helper (mailer) | R | 1 | 160 |  |  | E |
| 705 | text | large network-driven state machine, one call from TM +186F90 (boot/option code) | B | 1 | 8137 | net thread |  | E |
| 774 | text | IME open and conversion status (ImmGetOpenStatus, ImmGetConversionStatus) | C P | 3 | 38 | ime |  | E* |
| 824 | core | register TM callbacks +1821E0, +182200, +182210 [SetEventHandler]; also called with 0, 0, 0 | B | 4 | 34 |  | yes | E |
| 831 | irc | channel-protocol event query (cp.c) | Y M | 1 | 95 |  |  | E |
| 832 | irc | channel-protocol event release (cp.c) | Y M | 1 | 43 |  |  | E |
| 836 | core | Viewer state query | Y | 2 | 49 |  |  | E |
| 843 | irc | hash key for message checks [GetCommonHashKey]; shares a key string with 132 | Y M | 1 | 63 |  |  | E |
| 844 | irc | hash key companion of 843 | Y M | 1 | 43 |  |  | E |
| 907 | opt | load options [LoadOption] | B O | 3 | 10 |  | yes | E |
| 908 | opt | load-options check, polled with Sleep(1) [LoadOptionCheck]; reads data/utf/env.dat and pub/home00/option.bin; measured 1 after 6 polls | B O | 3 | 2148 |  | yes | E* |
| 909 | opt | store options [StoreOption] | O | 3 | 10 |  |  | E* |
| 910 | opt | store-options check [StoreOptionCheck]; writes pub/home00/option.bin | O | 3 | 2291 |  |  | E* |
| 911 | opt | option state query | B O | 4 | 71 |  |  | E |
| 912 | opt | option value by name (GOPN1_HF, LFRD1_FN, LFRD1_OG, LFRD1_OJ) | O F | 5 | 158 |  |  | E |
| 913 | opt | option value by name (GOPN1ESL) | O | 1 | 73 |  |  | E |
| 923 | text | file or directory name (%s%s) | P F | 5 | 54 |  |  | E |
| 930 | text | file or directory name | P F | 2 | 52 |  |  | E |
| 931 | text | file or directory name | P F | 2 | 139 |  |  | E |
| 966 | core | returns the language set by 965 | B Y M | 9 | 2 |  | yes | E |
| 967 | core | returns 1 | C | 1 | 2 |  |  | E |
| 968 | core | chat helper | C | 2 | 19 |  |  | E |
| 1023 | core | profile-screen helper | P | 1 | 41 |  |  | E |
| 1069 | core | returns 0 | R | 1 | 2 |  |  | E |
| 1132 | core | game-object helper (TM +186BD0) | B | 1 | 157 |  |  | E |
| 1152 | msg | relative time text ("am", "minutes") for the friend list | F | 1 | 758 |  |  | E |
| 1164 | mail | hex formatter | R | 1 | 25 |  |  | E |
| 1167 | mail | returns a mail buffer pointer | R | 1 | 2 |  |  | E |

### Core slots

| Slot | What it does | Caller | Verdict |
|---:|---|---|---|
| 0 | QueryInterface | TM GameStart | E |
| 1 | AddRef | TM GameStart | E |
| 3 | Returns a second interface, kept at TM `0x1024DFEC`; TM later calls its slots 8 and 2 | TM GameStart | E |
| 4 | Command-line getter | TM GameStart | E |
| 5 | SetParamInit(HINSTANCE, `" /game DdcMLbB"`) | host | E\* |
| 7 | GetCommonTable: the table at `+6FBE8` | TM GameStart, host | E |
| 26 | Registry root name getter | TM GameStart | E |
| 27 | SetAreaCode(1) | host | E\* |
| 9, 12 | OS version and Viewer settings from the registry | nobody | not needed |
| 19–21 | Bitmap blit into a memory DC; keyboard hook install and removal | nobody | not needed |

### polcore internals the host calls

From tmloader's `src/platform/network.cpp`, all measured in the run: `+FE20` initialise INET (`WSAStartup` alone
is not enough), `+127A0` resolve an endpoint, `+1EB00` initialise the profile client or `+1EA60` set its login,
`+1E930` set its endpoint, `+19BA0`, `+1A170` seed the profile cipher from the Game IRC keys, `+1E980`. Their
addresses are valid only for the pinned polcore hash.

## Effort

| Work | Person-weeks |
|---|---:|
| Winsock on WebSockets: socket records, `select`/`SO_ERROR`/`recv` semantics, first-message auth, close codes, chunking | 1–1.5 |
| Host Viewer set-up and profile bootstrap: port tmloader's `loadViewer`, `initializeViewerSession`, `seedProfile` and `loadProfile` onto `callGuest` | 0.5 |
| polcore's thread needs on the shared scheduler: `_beginthreadex` workers, mutex ownership under preemption, `Sleep` yielding, `ExitThread` | 0.5 |
| Files: `env.dat` and `option.bin` reads, `option.bin` writes persisted in OPFS | 0.25 |
| Key generation: host stand-ins for the four hot routines, checked against emulation | 0.25 |
| Tests: a Node run against a local tmserver over its browser streams; a differential check against this Unicorn harness | 1 |
| **Total** | **3.5–4** |

This fits inside the spike's 4–6 weeks for polcore; the gateway change it included is now tmserver PR #19.
Reimplementing instead: about 10–16 person-weeks with a long tail, plus an oracle for every entry (this harness
could serve as one).

## Limits

- Boot and login were run; lobby, match, profile writes, options writes, friends, messages and mail are attributed
  statically, from TM's calling functions and tmserver's notes. They use the Game IRC slot and profile connections
  that the run proved.
- Static closures do not follow indirect calls, so instruction counts are lower bounds.
- The per-entry descriptions are best effort; a name in brackets comes from the PS2 decomp and is unconfirmed.
- The harness runs threads only between the main thread's calls. The interpreter preempts, which is why mutex
  ownership is listed as a requirement.

## Reproduce

Python with `pefile`, `capstone` and `unicorn`. Nothing is written outside `out/`, which git ignores, because it
holds addresses and strings taken from the user's DLLs.

```sh
cd browser/tools/polcore
export POLCORE_DLL=<runtime>/polcore_native.dll TM_RUNTIME=<runtime> TM_MAPPED=<unpacked TM image>
python common.py
python scan_tm.py out/tm_calls.json
python run_polcore.py out/run_offline.json                       # boot only, about 12 s
# with tmserver running: python -m tmserver --game-port 52241 --profile-port 52220 ...
python run_polcore.py out/run_irc.json --irc 127.0.0.1:52241:52220
python run_polcore.py out/run_profile.json --profile             # key-generation hot spots, about 2 min
python scan_polcore.py out/polcore_entries.json out/run_irc.json out/tm_calls.json
python report.py out/tm_calls.json out/polcore_entries.json out/run_irc.json > out/table.md
```

`TM_MAPPED` is the spike's `tm_mapped.bin` (`unpack_emu.py` on the `wasm-spike` branch) or tmserver's verified
`TM_emulated_mapped.dll`.
