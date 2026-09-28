# C++ style

The loader is C++23, built with MSVC's `/std:c++23preview`, and follows the
[LandSandBoat](https://github.com/LandSandBoat/server/wiki/Development-Guide)
conventions.

## Formatting

`.clang-format` is LandSandBoat's configuration plus `InsertBraces: true`. It
requires **clang-format 22** and sets four-space indentation, Allman braces,
aligned declarations, left-aligned pointers and references, and west `const`.
There is no column limit; split long expressions where it helps readability.

```powershell
py tools/run_clang_format.py          # format
py tools/run_clang_format.py --check  # verify
```

The runner needs Python 3.10+ and finds clang-format 22 on `PATH` or in the
usual LLVM install location; `--clang-format PATH` selects a specific binary.
It formats first-party `.cpp`, `.h` and `.hpp` files under `src/` and `tests/`.
The same commands are available as the CMake targets `format` and
`format-check`.

## Conventions

- **Layout.** Files live in their owning `src/` subsystem, and tests mirror
  that folder under `tests/`. Includes use the source-root path, such as
  `"ui/window.h"`, even within the same folder.
- **Headers.** Class declarations go in self-contained headers and method
  bodies in `.cpp` files. Templates and small inline helpers may stay in
  headers. Private helper types use `tmloader::detail`, never an anonymous
  namespace in a header.
- **Internal linkage.** Implementation constants, helpers and module state go
  in an anonymous namespace in their `.cpp`. Native offsets are named
  constants next to the hook that uses them.
- **Hooks.** Replacement functions are named `hooked<ApiName>`, such as
  `hookedCreateFileA` or `hookedPresent`. Each subsystem exposes one install
  function, declared in `app/hooks.h`.
- **Declarations.** Prefer `const auto`, `constexpr` and one variable per
  declaration. Make locals `const` unless they change, are output parameters,
  own a resource or are returned by move. Distinguish `auto* const` (constant
  binding) from `const auto*` (read-only pointee). Use named casts and
  `nullptr`.
- **Return types.** Non-void functions use trailing return types, including
  Windows callbacks and COM methods: `auto WINAPI callback(...) -> HRESULT`.
  Keep exact Windows and COM calling conventions and ABI types.
- **Control flow.** Every control statement has braces. Do not use the ternary
  operator; use `if` or an immediately invoked lambda.
- **Classes.** Private members use a trailing underscore (`member_`).
  Single-argument constructors are `explicit`; use `override` and `final`.
  Follow the rule of zero or five, delete copy and move for owners that cannot
  transfer ownership, and mark `noexcept` only where it holds.

`NOMINMAX`, `WIN32_LEAN_AND_MEAN` and `TMLOADER_VERSION` are global compile
definitions; do not redefine them in sources.

Windows SDK headers have ordering dependencies: Winsock before Windows, Windows
before BCrypt, and multimedia before DirectSound. Keep these include groups
separated by blank lines so include sorting preserves the order.

## Static analysis

`.clang-tidy` enforces trailing return types and const local bindings, and
rejects C-style casts, `NULL` and `typedef`. Its pointer fixes only make
bindings const; they do not change pointee types.
