# Third-party code

CMake fetches every dependency from a pinned archive and verifies its SHA256.
None adds a runtime DLL. Each library's license is in `third_party/`, and
`tools/build_release.py` copies those of the linked libraries into the
archive's `THIRD_PARTY_NOTICES.txt`.

| Library | Version | License | Used for | Notice |
|---|---|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.92.5 (`6d910d5`) | MIT | Settings panel, with the unmodified Win32 and DirectX 9 backends | `third_party/imgui.LICENSE.txt` |
| [JSON for Modern C++](https://github.com/nlohmann/json) | 3.12.0 | MIT | Saved preferences and the hosted API | `third_party/nlohmann-json.LICENSE.txt` |
| [CLI11](https://github.com/CLIUtils/CLI11) | 2.7.2 | BSD-3-Clause | Command-line and config file parsing (header-only) | `third_party/CLI11.LICENSE.txt` |
| [Microsoft WIL](https://github.com/microsoft/wil) | v1.0.260126.7 (`cbf677f`) | MIT | Windows resource ownership (header-only) | `third_party/WIL.LICENSE.txt` |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | 0.60.1 | MIT | Loopback sign-in callback for hosted play (header-only); fetched and linked only with `TMLOADER_HOSTED`, without its TLS or compression backends | `third_party/cpp-httplib.LICENSE.txt` |
| [Catch2](https://github.com/catchorg/Catch2) | 3.16.0 | BSL-1.0 | Native tests only; fetched with `BUILD_TESTING=ON` and never linked into the loader | `third_party/Catch2.LICENSE.txt` |

## Pinned archives

| Library | Archive | SHA256 |
|---|---|---|
| Dear ImGui | commit `6d910d5487d11ca567b61c7824b0c78c569d62f0` | `b715a3226320aaebaea79dc6af6405ae37cdcb479817fddaef12a17d8bd15a3a` |
| JSON for Modern C++ | release `json.tar.xz` | `42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa` |
| CLI11 | tag `v2.7.2` | `46eef3101da70852ec7af026e09d485ccee81813331c8c6052d39344443b83da` |
| Microsoft WIL | commit `cbf677fb0a942557d08fd129f4c106a76247b2ec` | `d7b3c903b501f6ea73a733ee4cd4215119dc10eee969bda2aa134732b8e8fe6f` |
| cpp-httplib | tag `v0.60.1` | `f42fc046e55f968a9dd43b272251f033b08538d3e359235608f980f551053215` |
| Catch2 | tag `v3.16.0` | `0957cae5821b17ce07f0833aaa52b5137643a8382203221f363a8303c109af34` |

cpp-httplib's maintainer does not support or review 32-bit builds. The loader
uses it only for the sign-in callback on `127.0.0.1`; HTTPS and WebSockets stay
on WinHTTP.

The declarations are in `cmake/Dependencies.cmake`. Proprietary game files are
not part of any dependency.
