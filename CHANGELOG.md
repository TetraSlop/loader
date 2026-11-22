# Changelog

## [0.1.0](https://github.com/TetraSlop/loader/compare/v0.1.0...v0.1.0) (2026-11-22)


### ⚠ BREAKING CHANGES

* sign in with a username; the server assigns the POL ID
* sign in with LOGIN before the game starts
* remove hosted mode and Discord sign-in
* remove the game files from the repository
* ship only the archive without game files
* remove the HD texture pack
* remove Discord Rich Presence
* rename the hdTextures preference to hdArtwork
* **release:** ship lean archives from a runtime-shaped assets tree
* restructure the loader for open source
* remove the BoxedWine browser experiment

### Features

* --check verifies every file in retail-files.txt ([b6a6f14](https://github.com/TetraSlop/loader/commit/b6a6f147d43b44b5c606f535105554c3958cd609))
* automate client releases with minimal and full asset archives ([bae0bcc](https://github.com/TetraSlop/loader/commit/bae0bcc681623db11b2d912f94c99141684b9104))
* Discord Rich Presence ([d7338d7](https://github.com/TetraSlop/loader/commit/d7338d71e812a65f573705426bb1e439d59cdf9e))
* hosted play with Discord sign-in and authenticated streams ([5c12209](https://github.com/TetraSlop/loader/commit/5c1220964947aecfe4f01f3270f7042cd263491f))
* **hosted:** game-styled launcher with Discord profile ([b20252b](https://github.com/TetraSlop/loader/commit/b20252bdeb93473608417c79e85a883f0fb585fc))
* **hosted:** show the Supabase user ID for account enrollment ([87465d7](https://github.com/TetraSlop/loader/commit/87465d75e05f7cd57c53610c4b01880808ed9831))
* **login:** the game port's key exchange and the LOGIN verb ([3aac8f9](https://github.com/TetraSlop/loader/commit/3aac8f96c4d472bd4ec8eca06189b90eaef9a3b3))
* Rich Presence follows the game phase ([2400660](https://github.com/TetraSlop/loader/commit/2400660a7f6357462b1aa25810084c328e538097))
* sign in with a username; the server assigns the POL ID ([52de7b5](https://github.com/TetraSlop/loader/commit/52de7b5ab0e8a70fa985fb872032310303255336))
* sign in with LOGIN before the game starts ([d9cbdc7](https://github.com/TetraSlop/loader/commit/d9cbdc72a0b561f8c4bda77fb7b8c0f13cd72c6d))
* write a minidump when the game crashes ([43b332a](https://github.com/TetraSlop/loader/commit/43b332a2a7e29d035735421239c80b05e02d1b0f))


### Bug Fixes

* **ci:** select the Visual Studio toolchain installed on the runner ([c1c36d1](https://github.com/TetraSlop/loader/commit/c1c36d19de366ffa674a01ebc5f09d50e9292542))
* **hosted:** end a stream quietly when the loader closes it ([471014f](https://github.com/TetraSlop/loader/commit/471014f2739229a11d32ab6ce94df1989a4fcaea))
* **hosted:** keep the login on temporary errors, cancel play on close ([f6e5562](https://github.com/TetraSlop/loader/commit/f6e55629ac3da2110f8f29b77647624716496246))


### Miscellaneous Chores

* remove the BoxedWine browser experiment ([264bef1](https://github.com/TetraSlop/loader/commit/264bef10d8776b442978d6fe7a4748a957d4355e))
* remove the game files from the repository ([e020cb8](https://github.com/TetraSlop/loader/commit/e020cb85c3555d569f843a9741f4a7be34ef0711))
* start private releases at 0.1.0 ([9516a50](https://github.com/TetraSlop/loader/commit/9516a5089610d6a34d45692beafade190cba32c3))


### Code Refactoring

* **release:** ship lean archives from a runtime-shaped assets tree ([c4928b9](https://github.com/TetraSlop/loader/commit/c4928b9a908dc469e3a97320816a1532fe5eaa33))
* remove Discord Rich Presence ([b866ec6](https://github.com/TetraSlop/loader/commit/b866ec637b9595aed80b0154f0041d3f3fcbd34f))
* remove hosted mode and Discord sign-in ([b916c4e](https://github.com/TetraSlop/loader/commit/b916c4e99539e206f55c2cf339c43270d5145657))
* remove the HD texture pack ([e610fea](https://github.com/TetraSlop/loader/commit/e610fea7437efa069212fe03f02209a5adef6c43))
* rename the hdTextures preference to hdArtwork ([c491119](https://github.com/TetraSlop/loader/commit/c491119cb9bdba553ee321f0c703fae703d882df))
* restructure the loader for open source ([64fba84](https://github.com/TetraSlop/loader/commit/64fba84db8bed941e276d44868dfbaa773edf3a8))


### Build System

* ship only the archive without game files ([32bd7df](https://github.com/TetraSlop/loader/commit/32bd7dff67ff7581939eca85cdb7d6d470f24135))

## 0.1.0 (2026-09-28)


### Features

* automate client releases with minimal and full asset archives ([186bb03](https://github.com/sruon/tmloader/commit/186bb03511104c84e5a034f6aefe430d8bfadac9))


### Bug Fixes

* **ci:** select the Visual Studio toolchain installed on the runner ([d916412](https://github.com/sruon/tmloader/commit/d916412d6a2d7f71193e3b32c3d14bda869f41eb))


### Miscellaneous Chores

* start private releases at 0.1.0 ([3a1c779](https://github.com/sruon/tmloader/commit/3a1c77926806b694dbd1143f443878628a64100e))
