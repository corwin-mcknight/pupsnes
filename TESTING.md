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

Use `ci-verify.sh` for local CI-style verification:

```sh
./ci-verify.sh
```

The script can also run individual phases. GitHub Actions invokes the build and
test phases separately so each test group has its own step:

```sh
./ci-verify.sh configure
./ci-verify.sh build
./ci-verify.sh test-unit
./ci-verify.sh test-integration
./ci-verify.sh test-additional
```

The default all-in-one command also runs lint locally and treats the existing
lint backlog as non-blocking. Run `./ci-verify.sh lint` to get the standalone
linter status. GitHub Actions does not run lint.

GitHub Actions runs for pull requests and pushes to `main`. It caches Conan
packages and Ccache compiler outputs; builds on `main` seed the compiler cache
for future pull requests. Local builds already reuse Ninja's `build/`
directory; developers with Ccache installed can additionally set
`CMAKE_CXX_COMPILER_LAUNCHER=ccache` when configuring a fresh build directory.

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
