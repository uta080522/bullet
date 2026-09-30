# Contributing to bullet

Thank you for helping improve bullet! Bug reports, documentation fixes, tests,
and feature contributions are welcome.

## Choose the right workflow

Use the amount of discussion that the change needs:

| Change                                                           | Suggested workflow                                                                                                                 |
| ---------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------- |
| Typo, documentation fix, or small bug fix                        | Create a branch, make and verify the change, then open a pull request. An issue is not required.                                   |
| Small new feature                                                | Open a pull request directly if the scope and behavior are clear. If unsure, discuss it in an issue first.                         |
| Large new feature, CLI interface change, or substantial redesign | Open an issue to discuss the problem and proposed behavior, and wait for maintainer feedback on the direction before implementing. |

Before opening an issue or pull request, search existing open and closed issues
and pull requests for related work. Keep each contribution focused on one problem;
avoid unrelated formatting, dependency updates, or refactoring.

### Report a bug

Include your operating system, bullet version or commit, the command you ran,
reproduction steps, expected behavior, actual behavior, and relevant error output.
A small reproducible example is especially helpful. Remove credentials and private
information from logs, chat files, and media before sharing them.

### Propose a feature

Explain the user problem, give a concrete usage example, describe the proposed
behavior, and mention alternatives or compatibility concerns. Start with the
smallest useful scope; an issue is a place to agree on direction, not a requirement
to design every implementation detail in advance.

## Work on a branch

Branch from the latest `main`. If you do not have write access, fork the repository
and push your branch to your fork. Otherwise, push it to this repository.

Open a pull request targeting `main` before merging your branch into `main`.
Use a draft pull request when you want early feedback on unfinished work.

## Set up and verify

The documented build environment is Windows x64. You need CMake 3.24+, a C
compiler, vcpkg, and FFmpeg and ffprobe on `PATH` for the tests. Set `VCPKG_ROOT`
to your vcpkg checkout and use a Visual Studio Developer PowerShell:

```powershell
cmake -S . -B build "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
clang-format --dry-run --Werror bullet.c test.c
```

CMake enables warnings as errors. Use the dependencies and baseline in
`vcpkg.json`; do not update them unless your change requires it.

Tests use Meiryo Bold on Windows by default. If that font is unavailable, set
`BULLET_TEST_FONT` to the path of a suitable font, such as Noto Sans CJK JP Bold.
Tests use local media fixtures and fake downloaders; real downloads require the
additional tools described in [README.md](README.md).

For changes to C code, also configure, build, and test with sanitizers in a separate
build directory:

```powershell
cmake -S . -B build/sanitize "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows-static -DSANITIZE=ON
cmake --build build/sanitize --config Release
ctest --test-dir build/sanitize -C Release --output-on-failure
```

MSVC uses AddressSanitizer; other supported compilers use AddressSanitizer and
UndefinedBehaviorSanitizer. A sanitizer-capable compiler is required.

## Follow the development conventions

[AGENTS.md](AGENTS.md) contains the shared development rules. In particular:

- Keep the C99 application in `bullet.c`, native tests in `test.c`, and build files
  at the repository root. Use tabs and the repository's `.clang-format`.
- Keep build artifacts in `build/` and downloaded media and emote caches in `data/`.
- Add regression coverage for bug fixes and tests for new behavior. Use test
  fixtures, not user media or caches, for destructive cases.
- Preserve source files and existing output on failure; check sizes, arithmetic,
  process exits, and file operations.
- Update all four READMEs when changing commands, dependencies, or behavior that
  users need to know.

For documentation-only changes, check the affected examples, commands, and local
links. A sanitizer run is not required when C code is unchanged.

## Open a pull request

Use a descriptive title and explain:

- The user problem and what changed.
- Related issues, if any. Use `Fixes #...` only when the change fully resolves
  that issue.
- The checks you ran and their results, including anything you could not verify.
- Any compatibility changes or known limitations.

Review the diff and run `git diff --check` before submitting. Exclude build
artifacts, downloaded media, caches, secrets, and unrelated changes. Wait for CI
and maintainer review before merging, and push review fixes to the same branch.

Contributions are made under the repository's [MIT license](LICENSE).
