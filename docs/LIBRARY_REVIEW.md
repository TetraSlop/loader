# Library review

Reviewed 2026-09-27 against the actual loader sources and the upstream releases
linked below. This is an adoption review, not a claim that every candidate has
been built or exercised with the native game. Replacement estimates describe
existing code affected, not guaranteed net line savings.

The best immediate addition is **Microsoft WIL for owned Windows resources**.
Keep the existing Dear ImGui and nlohmann/json integrations and the Catch2
migration. CLI11 is a reasonable next step for configuration; logging and a
different graphics backend need a concrete feature requirement first.

| Candidate | Decision | Actual scope in this loader |
|---|---|---|
| WIL | Adopt a small ownership subset now | Repeated CNG, token, local allocation and file handle cleanup across four or five functions |
| CLI11 | Later, when extending configuration | About 80 lines of option dispatch, config parsing and duplicated help text in `config.cpp` |
| spdlog | Later, for log levels or an in-panel log viewer | A small logging implementation plus roughly 60 call sites; mainly new capability, little code removed |
| MinHook | Keep in reserve for a genuine function detour | Current import/vtable replacement and verified call-site patches are a different problem |
| d3d8to9 | Separate optional rendering experiment | Adds a D3D9 translation backend; does not replace the scaling, texture or input adapters |

## WIL: resource ownership

[`v1.0.260126.7`](https://github.com/microsoft/wil/releases/tag/v1.0.260126.7)
was published January 26, 2026. Microsoft maintains this header-only library;
its documentation includes MSVC and `x86-windows` use. The
[license is MIT](https://github.com/microsoft/wil/blob/v1.0.260126.7/LICENSE).
There is no runtime DLL or additional service. New header instantiations add
compile work; no binary-size measurement was made.
[Upstream README](https://github.com/microsoft/wil/blob/v1.0.260126.7/README.md).

The useful first changes are specific:

- `validation.cpp::sha256File`: replace the cleanup lambda and its repeated
  calls with `wil::unique_bcrypt_algorithm` and `wil::unique_bcrypt_hash`.
  Destruction order must remain hash before algorithm.
- `texture_pack.cpp::sha256Bytes`: use the same algorithm owner.
- `log.cpp::lowerIntegrity`: own the process token and the SID allocation.
  `ConvertStringSidToSidA` needs **LocalFree**, so use `unique_hlocal` or
  `unique_hlocal_ptr`, not `unique_sid`, whose deleter is **FreeSid**.
- `endpoints.cpp::localOutputAllowed`: own the temporary file handle without
  changing the canonical path or hard-link checks.
- Later, `screenshot.cpp::writeScreenshot`: a handle owner can simplify the
  failure cleanup. Preserve the explicitly checked close, `CREATE_NEW`, and
  removal of only this invocation's incomplete file.

That affects roughly 15 cleanup statements and several failure branches; the
benefit is clear ownership and exception safety. The
[resource definitions](https://github.com/microsoft/wil/blob/v1.0.260126.7/include/wil/resource.h)
provide these aliases. Include the relevant Windows SDK headers, including
`bcrypt.h`, before `wil/resource.h`.
[Resource usage](https://github.com/microsoft/wil/wiki/RAII-resource-wrappers).

Pin commit `cbf677fb0a942557d08fd129f4c106a76247b2ec`; its
[source archive](https://github.com/microsoft/wil/archive/cbf677fb0a942557d08fd129f4c106a76247b2ec.tar.gz)
was retrieved during this review and has SHA256
`d7b3c903b501f6ea73a733ee4cd4215119dc10eee969bda2aa134732b8e8fe6f`.
The official CMake target is `WIL::WIL`. Set `WIL_BUILD_TESTS=OFF` and
`WIL_BUILD_PACKAGING=OFF` before adding it. This release's
[CMake minimum is 3.29](https://github.com/microsoft/wil/blob/v1.0.260126.7/CMakeLists.txt),
above the loader's previous 3.24 requirement. Raise and document that minimum
or deliberately expose the pinned headers through an interface target.

Use the nonthrowing owner types inside native callbacks, preserving error-code
returns. Do not migrate all error handling to throwing/fail-fast macros.
WIL cannot grant low-integrity write access; existing output checks and token
handling remain necessary. Keep the SDK's existing `WRL::ComPtr` for COM.
Do not automatically release or unload game/Viewer objects on new paths:
their teardown is an explicitly unresolved native lifecycle issue. C++23
compatibility is expected from the header design and supported MSVC toolchain;
the loader's actual x86 C++23 build remains the acceptance check.

## CLI11: one declaration per option

[`v2.7.2`](https://github.com/CLIUtils/CLI11/releases/tag/v2.7.2), published
August 2, 2026, follows recent parser fixes and compile-time improvements.
It is [BSD-3-Clause licensed](https://github.com/CLIUtils/CLI11/blob/v2.7.2/LICENSE),
header-only by default, has no runtime dependency and exposes `CLI11::CLI11`.
Its [upstream CI](https://github.com/CLIUtils/CLI11/blob/v2.7.2/azure-pipelines.yml)
includes Windows and C++23 configurations. A specific MSVC x86 C++23 loader
build has not been verified by this review.

`config.cpp` currently repeats option names in `assign`, the argument loops
and `usage`, with a separate `key=value` parser. CLI11 can derive help and
conversion from option declarations and supply config parsing. Restrict the
include to `config.cpp` to limit template compilation. There is also an
optional precompiled mode, unnecessary for one translation unit.
[Official usage](https://github.com/CLIUtils/CLI11/blob/v2.7.2/README.md).

The small existing parser already works, so this is a maintainability choice
when adding options. Retain the custom validation for output boundaries,
numeric service addresses, reserved ports, account IDs and credentials. Tests
must preserve CLI-over-file precedence, duplicate-key rejection, relative
runtime paths resolved beside the config, and password bytes containing `#`,
`;` or `=`. A library's normal INI quoting/comment rules may change those
semantics. Keep credentials out of generated help/default dumps. Parsing
stays before the integrity drop; the native ANSI/MAX_PATH limitations remain.

## Logging: standard formatting first, spdlog for new capabilities

`log.cpp::logf` is roughly 15 lines around CRT output. Replacing it alone does
not justify a framework. C++23's standard formatting can remove unchecked
printf argument/type pairs during normal logging. Keep a small emergency
writer for `crash`; an exception filter must not depend on allocating format
strings or acquiring locks that the failing thread may already hold.

If we add severity filtering, structured context or a bounded diagnostic panel,
[`spdlog v1.17.0`](https://github.com/gabime/spdlog/releases/tag/v1.17.0)
(January 4, 2026) is a good fit. It offers synchronized sinks, levels and
formatting; the [license is MIT](https://github.com/gabime/spdlog/blob/v1.17.0/LICENSE).
Its [CMake build](https://github.com/gabime/spdlog/blob/v1.17.0/CMakeLists.txt)
supports a static `spdlog::spdlog` target and `SPDLOG_USE_STD_FORMAT=ON`, which
avoids a separate fmt dependency. Disable its examples/tests, keep the loader's
static CRT setting, and verify Win32 with the exact C++23 toolchain. This review
does not establish a local x86 build result or executable-size overhead.

Adoption would touch approximately 60 ordinary log calls, preserving explicit
wide-path conversion. Start with synchronous logging: the current final
`TerminateProcess` cannot be trusted to drain an asynchronous queue. Open the
approved file before lowering integrity, as today. Rotation/reopen creates new
files and needs separate low-integrity/path-boundary validation. Exceptions
must not escape game callbacks, and the crash path should remain independent.

## Hooking: do not exchange pointer replacement for global detours

[`MinHook v1.3.4`](https://github.com/TsudaKageyu/minhook/releases/tag/v1.3.4)
(March 28, 2025) supports x86/x64, adds CMake/VS2022 support, and fixes thread
enumeration/suspension error handling. It uses a
[BSD-style two-clause license](https://github.com/TsudaKageyu/minhook/blob/v1.3.4/LICENSE.txt),
including notices for its disassembler. Its C implementation and C API can
coexist with C++23; static integration would require enabling C in our project.
[Build source](https://github.com/TsudaKageyu/minhook/blob/v1.3.4/CMakeLists.txt).

The approximately 50 lines in `patch.cpp` edit pointers; `graphics.cpp` hooks
COM vtables. `texture_hooks.cpp` validates seven existing relative calls and
changes their operands transactionally. `desktop.cpp` skips a known cursor
block. MinHook's entrypoint trampolines do not directly replace these operations
or their per-build checks. Detouring a Windows API entrypoint could also affect
the loader and ImGui, whereas the current desktop policy targets game imports.

Use MinHook only if a future internal function genuinely needs an entrypoint
detour. Preserve exact x86 calling conventions, failure handling, DLL pinning
and initialization order; establish whether other threads can be executing
patched code. Low integrity is not permission to patch another process. No
hooking dependency or broad PE framework is justified for the current patch map.

## Graphics: d3d8to9 can add a backend, not remove the wrapper

[`d3d8to9 v1.16.0`](https://github.com/crosire/d3d8to9/releases/tag/v1.16.0)
was released September 23, 2026. It translates D3D8 API calls and shaders to
D3D9 and is [BSD-2-Clause licensed](https://github.com/crosire/d3d8to9/blob/v1.16.0/LICENSE.md).
The upstream [project](https://github.com/crosire/d3d8to9/blob/v1.16.0/d3d8to9.vcxproj)
and [CI](https://github.com/crosire/d3d8to9/blob/v1.16.0/.github/workflows/build.yml)
explicitly build Win32. A separate DLL leaves its compiler standard independent
of the C++23 host. Source integration would need its own build adaptation.

This could enable a supported D3D9 compatibility path or later integration with
D3D9 tools. It will not create higher-detail assets or replace `render_scale`,
`texture_replacements`, input mapping, screenshot handling or the verified
native ABI. The [README](https://github.com/crosire/d3d8to9/blob/v1.16.0/README.md)
documents D3DX shader dependencies and warns that translated presentation
behavior can differ, including VSync.

Keep this outside the readability refactor. The loader explicitly chooses a
D3D8 factory, so dropping a DLL beside the game is not sufficient proof of use.
A future opt-in adapter must load only a known explicit library path and test
shader scaling, managed textures, device reset, screenshots and presentation.
Changing DLL search order or importing another graphics stack adds deployment
and dependency risks. Low-integrity startup and a user-run native visual pass
are separate acceptance checks.

## Keep the existing platform facilities

WIC already supplies PNG encoding and `WRL::ComPtr` supplies COM ownership.
CNG already supplies SHA256. An image library, another COM smart pointer or a
cryptographic dependency would duplicate these facilities. SDL would not
remove the need to host the original Win32/DirectInput/D3D8 client, and moving
its window/input ownership would be a separate compatibility project.

For any adopted library, pin an immutable revision and archive hash, include
its redistribution notice, and keep dependency types out of the native game
ABI. Headless tests can verify resource cleanup and existing policy; they do
not establish native visual or shutdown parity.
