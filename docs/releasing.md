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
   tests, then builds the archive. It checks the executable and runs
   `tmloader.exe --check` in the extracted archive against placeholder DLLs,
   which must be rejected at the hash check.
4. When the archive passes, it is attached and the draft is published.

CI skips tests that need a real graphics device. No CI step needs game files,
or starts the game or a server.

While a draft is unpublished, no new release PR is opened. If a release build
or upload fails, rerun the failed jobs; do not tag by hand.

The workflow uses the repository's `GITHUB_TOKEN`. The repository's Actions
settings must allow Actions to create pull requests. Because bot-created PRs
do not trigger workflows, the release job dispatches CI for the release branch
itself, and publishing runs in the same workflow.

## Release contents

| File | Contents |
|---|---|
| `tmloader-VERSION-windows-x86.zip` | `tmloader.exe`, a sample `tmloader.ini`, `retail-files.txt` and license notices. No game files or server. |
| `SHA256SUMS.txt` | Archive hash |
| `validation.json` | Verification results |

CI runs also upload the archive as a build artifact, kept for seven days.

## Building locally

```powershell
python tools/build_release.py --loader build/Release/tmloader.exe
```

Output goes to `out/release/` and existing archives are never overwritten. The
package layout and checks are in [distribution](distribution.md).
