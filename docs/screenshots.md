# Screenshotting PPU output

PupSNES ships a headless screenshot CLI for grabbing the PPU's front buffer without running the ImGui debugger. This is the cheapest way to "look at" what a ROM puts on screen — useful for:

- Reading test-ROM result screens (e.g., `roms/cputest-basic.sfc` failure reports)
- Visually diffing PPU behaviour between branches
- Capturing what a regression looks like for a bug report
- Letting an LLM/agent read the output via the `Read` tool on the resulting PNG

The CLI is the binary `build/<preset>/pupsnes-screenshot`, source in `src/tools/screenshot_main.cpp`.

## Usage

```sh
build/ci/pupsnes-screenshot --rom <path> --frames N [--output <path>]
```

- `--rom` — path to a LoROM `.sfc` or `.smc` (the SMC copier header is stripped automatically)
- `--frames` — how many frames to run before grabbing the front buffer
- `--output` — destination PPM (default: `pupsnes-screenshot.ppm`)

The file is written as P6 PPM (binary RGB, 8 bits/channel). Width and height come from the live `FrameBufferView` — 256×224 normally, 256×239 under overscan. BGR555 is expanded to 8-bit by replicating high bits.

## Build

The CLI builds with the rest of the project:

```sh
cmake --build --preset ci --target pupsnes_screenshot_cli
```

It links only against `pupsnes_trace_runner` (which brings in `pupsnes` core), so it's fast to build and has no ImGui/GL dependency.

## Converting to PNG

The `Read` tool understands PNG/JPG but not PPM. Convert with the built-in macOS `sips`:

```sh
sips -s format png screenshot.ppm --out screenshot.png
```

Then `Read` the PNG and the model will see it directly.

## Picking a frame count

NTSC is ~60 fps, so frames map to roughly:

| Frames | Wall time | Use case                                  |
|-------:|----------:|-------------------------------------------|
|     60 |     1 sec | Title screens, intro logos                |
|    300 |     5 sec | Most test-ROM result screens              |
|    600 |    10 sec | Slow / multi-stage test ROMs              |
|   1800 |    30 sec | Games past the licensing screen           |

Start small. If the screen is still blank or "loading", bump it up. Going too far is harmless — the front buffer just holds the most recent completed frame.

## Workflow: comparing a test ROM against its expected results

For ROMs that have a sibling expected-results text file (`roms/cputest-basic.sfc` ↔ `roms/tests-basic.txt`, etc.):

1. Build the tool (`cmake --build --preset ci --target pupsnes_screenshot_cli`)
2. Run it on the ROM with enough frames for results to land:
   ```sh
   build/ci/pupsnes-screenshot --rom roms/cputest-basic.sfc --frames 600 --output /tmp/shot.ppm
   ```
3. `sips -s format png /tmp/shot.ppm --out /tmp/shot.png`
4. Read `/tmp/shot.png` to extract the displayed test number and register dump
5. Open the expected-results file at the displayed test number, e.g. line 25 of `tests-basic.txt` for "Test 0007", and cross-reference the input/expected output

Many test ROMs stop at the first failure and dump CPU state on screen, so the cross-reference is usually a single test entry, not a whole log.

## Caveats

- LoROM only today — HiROM ROMs will fail the size-mod check at load. Same constraint as `pupsnes-trace`.
- The CLI runs the CPU + scheduler loop exactly like `pupsnes-trace`; there's no controller input. If a ROM needs button presses to advance (e.g. "Press A for next tests..."), you'll only ever see the first stop screen.
- Output is the PPU front buffer at the moment the frame budget expires. If the ROM is mid-render (forced blank, mode switch, BG disabled, etc.), the image reflects that — pick a stable frame count for the ROM in question.
- No audio, no input log, no save state — this is strictly a visual snapshot.
