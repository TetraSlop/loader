# Third-party code

The settings panel uses [Dear ImGui](https://github.com/ocornut/imgui), version
1.92.5, commit `6d910d5487d11ca567b61c7824b0c78c569d62f0` (MIT license).
CMake fetches its source archive and verifies SHA256
`b715a3226320aaebaea79dc6af6405ae37cdcb479817fddaef12a17d8bd15a3a`.
The official Win32 and DirectX 9 backends are compiled without modifications.

The accompanying license is in `third_party/imgui.LICENSE.txt`; include it with
redistributed loader binaries. The fetched source/license also remain under
`build/_deps/imgui-src`. Proprietary game files are not part of this dependency.
