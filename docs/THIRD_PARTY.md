# Third-party code

The settings panel uses [Dear ImGui](https://github.com/ocornut/imgui), version
1.92.5, commit `6d910d5487d11ca567b61c7824b0c78c569d62f0` (MIT license).
CMake fetches its source archive and verifies SHA256
`b715a3226320aaebaea79dc6af6405ae37cdcb479817fddaef12a17d8bd15a3a`.
The official Win32 and DirectX 9 backends are compiled without modifications.

The accompanying license is in `third_party/imgui.LICENSE.txt`; include it with
redistributed loader binaries. The fetched source/license also remain under
`build/_deps/imgui-src`. Proprietary game files are not part of this dependency.

Texture manifests use [JSON for Modern C++](https://github.com/nlohmann/json),
version 3.12.0 (MIT license). CMake verifies the official release archive's
SHA256 `42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa`.
Include `third_party/nlohmann-json.LICENSE.MIT` with redistributed binaries.

Windows resource ownership uses [Microsoft WIL](https://github.com/microsoft/wil),
release `v1.0.260126.7`, commit `cbf677fb0a942557d08fd129f4c106a76247b2ec`
(MIT license). CMake verifies SHA256
`d7b3c903b501f6ea73a733ee4cd4215119dc10eee969bda2aa134732b8e8fe6f`.
It is header-only. Include `third_party/WIL.LICENSE.txt` with redistributed binaries.

Native tests use [Catch2](https://github.com/catchorg/Catch2), version 3.16.0
(Boost Software License 1.0). CMake verifies SHA256
`0957cae5821b17ce07f0833aaa52b5137643a8382203221f363a8303c109af34`.
The dependency is fetched only with `BUILD_TESTING=ON` and links into test
executables, never the loader. Its notice is in `third_party/Catch2.LICENSE.txt`.
