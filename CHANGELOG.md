# Changelog

## 0.2.0

First public release.

* Players sign in with a username and password through tmserver's `LOGIN`;
  the server creates the account on first login and assigns its POL ID.
  Hosted mode and Discord sign-in are gone.
* `tmloader.exe --check` verifies every game file against `retail-files.txt`.
* Releases ship no game files. Supply them from your own PlayOnline install of
  Tetra Master; `tools/prepare_runtime.py` copies and checks them.
