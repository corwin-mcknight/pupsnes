# Building

This project uses CMake with the Ninja generator.

## Presets

Configure and build with presets

```sh
cmake --preset dev
cmake --build --preset dev
```

Available presets: `dev`, `ci`, `release`.

The `pupsnes` executable will be in the relevant build directory under `build/`.

## Conan

Each configure preset expects a matching Conan output folder so the toolchain file exists at
`build/<preset>/conan_toolchain.cmake`.

```sh
# dev preset
conan install . --output-folder=build/dev --build=missing -s build_type=Debug

# ci preset
conan install . --output-folder=build/ci --build=missing -s build_type=RelWithDebInfo

# release preset
conan install . --output-folder=build/release --build=missing -s build_type=Release
```

After installing dependencies for a preset:

```sh
cmake --preset dev
cmake --build --preset dev
```

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
