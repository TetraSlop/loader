# Releasing

Releases are automated with
[release-please](https://github.com/googleapis/release-please-action). The
version lives in `version.txt` and is shared by CMake, `--version` and package
manifests.

## Flow

1. Push [Conventional Commits](https://www.conventionalcommits.org/) to `main`:
   `fix:` for patches, `feat:` for features, and `!` or `BREAKING CHANGE:` for
   incompatible changes.
2. Release-please maintains a release PR that updates `version.txt`, its
   manifest and `CHANGELOG.md`.
3. Merging that PR creates a draft release. CI builds the release commit with
   MSVC (C++23, static x86 runtime), checks formatting, runs the C++ and Python
   tests, then builds both archives, checking the executable and running
   `tmloader.exe --check` in the extracted full archive.
4. When both archives pass, they are attached and the draft is published.

Tests that need a real graphics device are excluded from hosted CI. No CI step
starts the game or a server.

While a draft is unpublished, no new release PR is opened. If a release build
or upload fails, rerun the failed jobs rather than tagging manually.

The workflow uses the repository's `GITHUB_TOKEN`. The repository's Actions
settings must allow Actions to create pull requests. Because bot-created PRs
do not trigger workflows, the release job dispatches CI for the release branch
itself, and publishing runs in the same workflow.

## Release contents

| File | Contents |
|---|---|
| `tmloader-VERSION-windows-x86.zip` | `tmloader.exe`, a sample `tmloader.ini` and license notices. No game files, HD textures or server. |
| `tmloader-VERSION-windows-x86-full.zip` | The same, plus the supported game and Viewer DLLs, original data and audio, factory Viewer settings and the HD texture pack. |
| `SHA256SUMS.txt` | Archive hashes |
| `validation.json` | Verification results |

CI runs also upload both archives as build artifacts, kept for seven days.

## Building locally

```powershell
python tools/build_release.py --loader build/Release/tmloader.exe
```

Output goes to `out/release/` and existing archives are never overwritten. The
full archive's inputs are described in [assets/README.md](../assets/README.md);
the package layout and checks are in [distribution](distribution.md).
