# Building

This project uses CMake with the Ninja generator. Conan dependencies are installed automatically during the configure step — no separate `conan install` command is needed.

## Quick start

```sh
cmake --preset dev
cmake --build --preset dev
```

Available presets: `dev`, `ci`, `release`.

The `pupsnes` executable will be in the relevant build directory under `build/`.

## How it works

CMake presets include `cmake/ConanAutoInstall.cmake` via `CMAKE_PROJECT_TOP_LEVEL_INCLUDES`. On configure, this script:

1. Checks if `conan_toolchain.cmake` exists in the build directory
2. Re-runs `conan install` if the toolchain is missing or `conanfile.txt` has changed
3. Includes the generated toolchain

You can still run `conan install` manually if you need to pass extra flags.

## Warnings

Builds enable common warning flags by default. The `ci` preset also enables warnings-as-errors.

## Linting

The project lints against the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html) using three tools:

- **clang-format** — formatting (`BasedOnStyle: Google`, 4-space indent, 120 col). Config: `.clang-format`.
- **clang-tidy** — semantic/style checks including `google-*` and `readability-identifier-naming` (CamelCase types/functions, `lower_case` vars, `k`-prefixed constants, `_`-suffixed members). Config: `.clang-tidy`.
- **cpplint** — Google's textual linter for header/include/comment conventions. Config: `CPPLINT.cfg`.

### Install

```sh
brew install clang-format llvm cpplint   # macOS
```

`clang-tidy` ships with LLVM (`brew install llvm`).

### Targets

```sh
cmake --build --preset dev --target format         # apply clang-format
cmake --build --preset dev --target format-check   # verify formatting
cmake --build --preset dev --target tidy           # run clang-tidy
cmake --build --preset dev --target cpplint        # run cpplint
cmake --build --preset dev --target lint           # all three
```

Third-party headers (Conan-provided: Catch2) are promoted to SYSTEM include dirs so lint warnings only fire on project code under `includes/` and `src/`.

`ci-verify.sh` runs `lint` non-blocking while the backlog is triaged; make it blocking once findings are resolved.
