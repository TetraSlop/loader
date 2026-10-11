# Library review

Libraries the loader adopted, and libraries that were evaluated and deferred.
Versions and licenses of the adopted ones are in
[third-party code](third-party.md).

| Library | Decision | Scope in this loader |
|---|---|---|
| Dear ImGui | Adopted | Settings panel with the official Win32/DX9 backends |
| nlohmann/json | Adopted | Texture pack manifests |
| Catch2 | Adopted | Native tests |
| Microsoft WIL | Adopted | Ownership of CNG, token, local-allocation and file handles |
| CLI11 | Adopted | Command-line and config file parsing |
| spdlog | Deferred | Only with a feature that needs log levels or an in-panel log viewer |
| MinHook | Deferred | Only if an internal function needs an entry-point detour |
| d3d8to9 | Deferred | A separate, optional rendering backend |

For any adopted library: pin an immutable revision and archive hash, include
its license notice, and keep library types out of the native game ABI.

## WIL: resource ownership

[WIL](https://github.com/microsoft/wil) is header-only, maintained by
Microsoft and MIT licensed. It owns:

- the CNG algorithm and hash handles used for SHA256 in
  `config/validation.cpp` (the hash is destroyed before the algorithm);
- the process token and SID in the low-integrity setup (`app/runtime`).
  `ConvertStringSidToSidA` memory needs `LocalFree`, so it uses
  `unique_hlocal`; the deleter of `unique_sid` is `FreeSid`;
- temporary file handles in output-path validation (`config/policy`).

Native callbacks use the non-throwing owner types and keep their error-code
returns. WIL's official CMake integration requires CMake 3.29. COM objects
keep using the SDK's `WRL::ComPtr`. Game and Viewer objects are never
released automatically, because their teardown is not supported.

## CLI11: command-line and config parsing

[CLI11](https://github.com/CLIUtils/CLI11) 2.7.2 is header-only and
BSD-3-Clause licensed. It replaced the hand-written parser in
`config/config.cpp`, which repeated each option name in its assignment code,
argument loop, config reader and usage text. Each option is declared once,
and CLI11 derives help text, type conversion and `key=value` config file
parsing from that declaration. Only `config.cpp` includes it.

The loader keeps its own validation on top of CLI11: output path boundaries,
numeric server addresses, reserved ports, account IDs and credentials. Tests
cover command-line-over-file precedence, duplicate-setting rejection, relative
`runtime-dir` resolved beside the config file, and passwords containing `=`.
Credentials are never printed in help or default values.

## spdlog: deferred

`app/log` formats with `std::format_to_n` into a stack buffer, so format
strings are checked at compile time and the crash handler can log without
allocating. The crash path must stay independent of any logging library,
because an exception filter cannot rely on allocation or on locks the failing
thread may hold.

[spdlog](https://github.com/gabime/spdlog) (MIT) is worth adding with a
feature that needs it, such as an F10 diagnostics tab showing recent warnings,
renderer and texture-pack status, and a copyable summary. Adopting it would
mean:

- a static `spdlog::spdlog` target with `SPDLOG_USE_STD_FORMAT=ON`, avoiding a
  separate fmt dependency;
- synchronous sinks only, because the final `TerminateProcess` cannot drain an
  asynchronous queue;
- opening the log file before lowering integrity, as today;
- separate path-boundary checks for any rotation, which creates new files.

## MinHook: deferred

[MinHook](https://github.com/TsudaKageyu/minhook) (BSD-2-Clause) provides
function entry-point trampolines. The loader has no entry-point detours: it
replaces import and vtable pointers (`platform/patch`, `graphics/graphics`),
redirects seven verified call sites (`textures/texture_replacements`), and skips one
verified code block (`platform/desktop`). Detouring a Windows API entry point
would also affect the loader and ImGui, whereas the current hooks target only
the game DLL's imports.

Adopt MinHook only if an internal game function needs an entry-point detour.
That work must preserve exact x86 calling conventions, DLL hash pinning and
initialization order, and establish whether other threads can be executing
the patched code.

## d3d8to9: deferred

[d3d8to9](https://github.com/crosire/d3d8to9) (BSD-2-Clause) translates D3D8
calls and shaders to D3D9. It could provide an optional D3D9 compatibility
backend, but it would not replace render scaling, texture replacement, input
mapping or screenshots, and its presentation behavior (including VSync) can
differ. It needs D3DX for shader translation.

The loader explicitly creates a D3D8 factory, so placing a DLL beside the game
would not activate it. An opt-in adapter would have to load one known library
path and be tested for shader scaling, managed textures, device reset,
screenshots and presentation.

## Platform facilities kept

WIC already encodes PNGs, `WRL::ComPtr` owns COM objects and CNG computes
SHA256, so image, smart-pointer and cryptography libraries would duplicate
them. SDL would not remove the need to host the original Win32, DirectInput
and D3D8 client.
