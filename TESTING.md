# Testing

This project uses Catch2 and CTest. Tests live under `src/tests` and are built when `BUILD_TESTING` is enabled (default).
Purpose-built test ROM sources live under `testroms/` and are assembled with `ca65`/`ld65` into `build/<preset>/test-roms/` during test builds.

## Local (Presets)

Configure, build, and run all tests:

```sh
cmake --preset dev
cmake --build --preset dev
ctest --test-dir build/dev
```

CTest reports three non-overlapping groups: unit tests, integration tests, and
additional legacy/topical tests that do not yet carry either high-level tag.
Together, these groups cover every Catch2 test case.

Run tests by tag (Catch2):

```sh
build/dev/pupsnes_tests "[unit]"
build/dev/pupsnes_tests "[integration]"
```

## CI

CI should use the `ci-verify.sh` script (preferred):

```sh
./ci-verify.sh
```

The script can also run individual phases, which the GitHub Actions workflow
uses to show each test group and lint as separate steps:

```sh
./ci-verify.sh configure
./ci-verify.sh build
./ci-verify.sh test-unit
./ci-verify.sh test-integration
./ci-verify.sh test-additional
./ci-verify.sh lint
```

The default all-in-one command treats the existing lint backlog as
non-blocking. The standalone `lint` phase returns the linter status so CI can
display it while allowing the verification job to pass.

If you need to run steps manually, use the `ci` preset and run CTest:

```sh
# install deps for ci preset
conan install . --output-folder=build/ci --build=missing -s build_type=RelWithDebInfo

cmake --preset ci
cmake --build --preset ci
ctest --test-dir build/ci
```

## AI Verification

AI should run the CI verification script after every code change:

```sh
./ci-verify.sh
```

This script configures, builds, runs every test group, and runs the lint suite (`clang-format`, `clang-tidy`, `cpplint`) against the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html). Lint is currently non-blocking — see [BUILDING.md](BUILDING.md#linting) for details and individual lint targets.
