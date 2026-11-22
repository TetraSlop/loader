# Berkeley SoftFloat 3e (subset)

The extended-precision (`extF80`) and 128-bit (`f128`) parts of John R. Hauser's Berkeley SoftFloat, Release 3e,
with the `8086` specialization (the x87's NaN rules and integer indefinites). `browser/f80` builds the x87 on it.

- Source: https://github.com/ucb-bar/berkeley-softfloat-3, commit `07ebd29db0cfb55723a79fbbffe99103ef38e305`
  (2026-11-03; the extF80 and f128 files are unchanged since 3e).
- Licence: BSD 3-Clause, in `COPYING.txt`. It is compatible with tmloader's GPL-3.0.
- Only the 82 files those functions need are copied, unmodified. `platform.h` is ours: build options shared by
  the native and Emscripten builds.
