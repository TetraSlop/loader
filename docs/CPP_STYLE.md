# C++ style

The loader reuses LandSandBoat's `.clang-format` unchanged and the user's
`lsb-cpp-style` conventions. The formatter requires **clang-format 22**. It owns
the four-space indentation, Allman braces, declaration alignment, left-aligned
pointers/references and west `const`. There is no fixed column limit; split long
expressions where that makes their structure easier to read.

The project remains **C++23** with MSVC's `/std:c++23preview`, as configured in
CMake. Clang-format's `Standard: Latest` controls formatting syntax only.

```powershell
py tools/run_clang_format.py
py tools/run_clang_format.py --check
```

The runner requires Python 3.10+ and works from any working directory. It selects clang-format 22 from
PATH or the usual Windows LLVM installation; `--clang-format PATH` chooses an
explicit executable, whose major version is still checked. It visits only
first-party `.cpp`, `.h` and `.hpp` files under `src/` and `tests/`. Build trees,
downloaded dependencies, runtime assets and IDE settings are outside that scope.

When CMake finds Python, the same commands are available as build targets:

```powershell
cmake --build build-modernize --target format --config RelWithDebInfo
cmake --build build-modernize --target format-check --config RelWithDebInfo
```

For new or refactored code:

- Put class declarations in self-contained `.h` files and ordinary method bodies
  in `.cpp` files. Templates and small shared inline helpers can remain in headers.
  Private helper types use a named `tmloader::detail` namespace; do not define
  header classes in an anonymous namespace.
- Keep implementation constants, helper functions and mutable module state in
  anonymous namespaces in their owning `.cpp`. Shared ABI and rendering constants
  retain named ownership in the headers that declare their contract.
- Prefer `const auto`, `constexpr`, typed initialization on the right and one
  variable per declaration. Use named casts and `nullptr`.
- Use trailing return types for new ordinary helpers, `member_` for private
  members, explicit single-argument constructors and `override`/`final` where
  appropriate. Retain exact Windows/COM calling conventions and ABI types.
- Use braced control flow and `if` or immediately invoked lambdas instead of new
  ternaries. Keep lazy evaluation and numeric conversions explicit.
- Follow the rule of zero or five. Resource-owning helpers that cannot transfer
  ownership delete copy and move operations. Use `noexcept` when supported by
  their operations, rather than promising it around potentially throwing work.

Windows SDK headers have ordering dependencies: Winsock precedes Windows, the
Windows base types precede BCrypt, and multimedia types precede DirectSound.
Keep these include groups separated by blank lines so include sorting preserves
the required order.

The current cleanup formats all owned C++, while semantic changes are confined
to the extracted classes and related implementation boundaries. Older control
flow and declarations elsewhere can adopt these conventions as they are edited.
The existing `.clang-tidy` checks cover C-style casts, `NULL` and `typedef`.

Reference: [LandSandBoat Development Guide](https://github.com/LandSandBoat/server/wiki/Development-Guide).
