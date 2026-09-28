# Releases

The private [tmloader repository](https://github.com/sruon/tmloader) uses
[release-please](https://github.com/googleapis/release-please-action). Push
Conventional Commits to `main`: `fix:` for patches, `feat:` for features and
`!` or `BREAKING CHANGE:` for incompatible changes. Release-please maintains a
release PR with `version.txt`, its manifest and generated changelog. Merge that
PR to create the release. CMake, `--version` and package receipts use the same
version file.

The workflow builds the exact release commit on Windows with MSVC, C++23 and
the static x86 runtime. It checks formatting, runs C++/Python tests, verifies
the executable and tests both extracted archives. Tests requiring a real
graphics device are excluded from hosted CI; they remain available locally.
No CI step starts the original game or a server.

Each release supplies:

- `tmloader-VERSION-windows-x86.zip`: loader, setup/import helpers and connection
  configuration; no original DLLs, media, HD textures, Python or server.
- `tmloader-VERSION-windows-x86-full.zip`: the same client plus the supported
  original DLLs/media, factory settings and the HD texture pack.
- `SHA256SUMS.txt` and `validation.json`: archive hashes and verification results.

Releases stay in draft until both archives pass checks and are attached. Build
artifacts are also available on successful CI runs for seven days. If a release
build or upload fails, rerun its failed jobs; do not create another tag manually.

The flow uses the repository's automatic `GITHUB_TOKEN`, without a personal
access token. Repository Actions settings must allow Actions to create pull
requests. Because bot-created PRs do not trigger another PR workflow, the release
job explicitly dispatches CI for its release branch. Publishing is part of the
same workflow, so it does not depend on a bot-created release event.

Build both archives locally after compiling the loader:

```powershell
python tools/build_release.py --loader build/Release/tmloader.exe
```

Output defaults to `out/release/`; existing archives are never overwritten.
The checked-in inputs are described in [assets/README.md](../assets/README.md).
They are isolated from personal runtime files. The [individual package
builder](DISTRIBUTION.md) still supports each inclusion flag separately.
