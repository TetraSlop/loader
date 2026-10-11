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
- **Headers.** Headers declare; every function body lives in a `.cpp` file.
  Only templates stay in headers. Private helper types use `tmloader::detail`,
  never an anonymous namespace in a header.
- **Constants.** Named constants are `kCamelCase`. Constants used by one file
  go in an anonymous namespace at the top of that `.cpp`, before any function,
  never inside a function body. Native offsets are named constants in the file
  whose hook uses them. Constants shared across files go in a header as
  `constexpr` in the `tmloader` namespace.
- **Enums.** A set of related values (vtable slots, table indices, message
  types, flag bits) is an enum, never a run of constants. ABI values that
  index or mask use an unscoped enum with a fixed underlying type; loader
  state uses `enum class`.
- **Internal linkage.** Implementation helpers and module state go in an
  anonymous namespace in their `.cpp`.
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

`.clang-tidy` is LandSandBoat's check set plus the conventions above: trailing
return types, const local bindings, `kCamelCase` constants, braces, named
casts, `nullptr`, `using` over `typedef` and ranges algorithms. Every finding
is an error. Its pointer fixes only make bindings const; they do not change
pointee types.

clang-tidy needs a compilation database, which the Ninja generator writes.
From an x86 developer prompt:

```powershell
cmake -S . -B cmake-build-tidy -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build cmake-build-tidy
py "C:\Program Files\LLVM\bin\run-clang-tidy" -p cmake-build-tidy -quiet "tmloader.(src|tests)"
```

Add `-fix` to apply the automatic fixes, then run the formatter. Do not accept
a fix that adds a virtual destructor to a COM class: it shifts the vtable.
