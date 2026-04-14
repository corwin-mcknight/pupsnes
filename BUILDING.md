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

Lint targets require `clang-format` and `clang-tidy` to be installed.

```sh
cmake --build --preset dev --target format
cmake --build --preset dev --target format-check
cmake --build --preset dev --target tidy
cmake --build --preset dev --target lint
```
