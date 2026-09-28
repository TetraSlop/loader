# Third-party code

CMake fetches every dependency from a pinned archive and verifies its SHA256.
None adds a runtime DLL. Include each license notice with redistributed loader
binaries; the notices live in `third_party/` and are copied into the
`licenses/` folder of packaged clients.

| Library | Version | License | Used for | Notice |
|---|---|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.92.5 (`6d910d5`) | MIT | Settings panel, with the unmodified Win32 and DirectX 9 backends | `third_party/imgui.LICENSE.txt` |
| [JSON for Modern C++](https://github.com/nlohmann/json) | 3.12.0 | MIT | Texture pack manifests | `third_party/nlohmann-json.LICENSE.MIT` |
| [CLI11](https://github.com/CLIUtils/CLI11) | 2.7.2 | BSD-3-Clause | Command-line and config file parsing (header-only) | `third_party/CLI11.LICENSE.txt` |
| [Microsoft WIL](https://github.com/microsoft/wil) | v1.0.260126.7 (`cbf677f`) | MIT | Windows resource ownership (header-only) | `third_party/WIL.LICENSE.txt` |
| [Catch2](https://github.com/catchorg/Catch2) | 3.16.0 | BSL-1.0 | Native tests only; fetched with `BUILD_TESTING=ON` and never linked into the loader | `third_party/Catch2.LICENSE.txt` |

## Pinned archives

| Library | Archive | SHA256 |
|---|---|---|
| Dear ImGui | commit `6d910d5487d11ca567b61c7824b0c78c569d62f0` | `b715a3226320aaebaea79dc6af6405ae37cdcb479817fddaef12a17d8bd15a3a` |
| JSON for Modern C++ | release `json.tar.xz` | `42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa` |
| CLI11 | tag `v2.7.2` | `46eef3101da70852ec7af026e09d485ccee81813331c8c6052d39344443b83da` |
| Microsoft WIL | commit `cbf677fb0a942557d08fd129f4c106a76247b2ec` | `d7b3c903b501f6ea73a733ee4cd4215119dc10eee969bda2aa134732b8e8fe6f` |
| Catch2 | tag `v3.16.0` | `0957cae5821b17ce07f0833aaa52b5137643a8382203221f363a8303c109af34` |

The declarations are in `cmake/Dependencies.cmake`. Proprietary game files are
not part of any dependency.
