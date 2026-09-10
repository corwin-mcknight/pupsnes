# Audio

PupSNES generates **32,000 Hz, 16-bit stereo sound** from the emulated SPC700 and S-DSP. Both the emulator and debugger can play that stream through an output device. The `pupsnes-audio` tool can also save native PCM to a WAV file without opening a window or sound device.

## Playback controls

Open **Audio → Audio settings...** in either frontend, or press **Cmd+,** on macOS (**Ctrl+,** elsewhere) when not editing text. The Audio menu also provides quick output-enable and mute controls. In the debugger, press **Run** to hear sound; paused instruction stepping remains silent.

| Setting | Behavior |
| --- | --- |
| **Enable output** | Opens or closes playback; enabled by default |
| **Mute** | Silences playback while emulation continues |
| **Volume** | 0–100%, starting at **50%** |
| **Output device** | Uses **System default** initially, or a selected device |
| **Latency** | 20, **40** (default), 80, or 160 ms buffering choices; higher settings can help with breakups |
| **Sound interpolation** | **Gaussian (SNES)** by default, or Linear; reset the game to apply a change |

Gaussian and linear interpolation share the same voices, sample decoding, envelopes, noise, pitch modulation, and echo. Gaussian uses the SNES interpolation filter. Linear changes how values between decoded samples are estimated; it can also affect voice output, pitch modulation, and echo.

Playback is active only during continuous execution at **100% emulation speed**. Slow motion and fast-forward are muted; audio time stretching is not implemented. Pausing, reset, loading another ROM, and changing playback speed discard queued sound so resuming does not play the previous moment. Hardware STOP instructions do not themselves silence the DSP while the machine keeps advancing.

**Refresh devices** updates the device list. If a device disconnects or cannot open, choose another output or use **Retry output**. The error remains in the audio panel and emulation can continue. If sound breaks up, try a higher latency. If nothing is audible, check that output is enabled, mute is off, volume is above zero, and the game is running at 100% speed.

For a known signal, run the synthetic **500 Hz stereo tone**:

```sh
./build/dev/pupsnes build/dev/test-roms/apu_audio.sfc
```

The normal development build produces this ROM with the other [test ROMs](test-roms.md).

## Native WAV capture

Capture a game's sound without a playback device:

```sh
./build/dev/pupsnes-audio --rom path/to/game.sfc --seconds 10 --output game.wav
```

The tool boots the ROM normally and records exactly the requested number of seconds of native **32 kHz, signed 16-bit stereo PCM**. Capture runs without a real-time playback requirement. It does not apply frontend volume, mute, output-device settings, or host resampling.

Use `--skip-seconds` to run through boot time before recording, and `--quality` to select interpolation:

```sh
./build/dev/pupsnes-audio --rom path/to/game.sfc --skip-seconds 5 --seconds 10 --quality gaussian --output music.wav
```

`--seconds` accepts whole numbers from 1 to 600; `--skip-seconds` accepts 0 to 600. Quality is `gaussian` by default, with `linear` as the alternative. Existing output files are preserved, so choose an unused filename. `--help` lists the options.

## Saved preferences

The apps save audio preferences alongside their existing settings:

| Frontend | Default file |
| --- | --- |
| Emulator | `~/.pupsnes_emulator.ini` |
| Debugger | `~/.pupsnes_config.ini` |

The files store output enable, mute, volume, latency, a stable device identifier, and the pending interpolation mode. Invalid numeric text and nonfinite values are rejected; valid volume and latency values are bounded to supported ranges. An unavailable saved device produces a recoverable output error instead of silently selecting another device.

Set **`PUPSNES_CONFIG_DIR`** to use an existing directory for these files, keeping their filenames:

```sh
mkdir -p /tmp/pupsnes-prefs
PUPSNES_CONFIG_DIR=/tmp/pupsnes-prefs ./build/dev/pupsnes path/to/game.sfc
```

An unset or empty override uses the normal home-directory location, falling back to the current directory if `HOME` is unavailable. ImGui window-layout preferences remain separate in the working directory's `imgui.ini`; use a separate working directory as well when isolating GUI checks completely.

## Implementation and current limits

Synthesis uses the pinned **blargg snes_spc 0.9.0** DSP core with local interpolation and address-wrap adaptations. Host playback uses **miniaudio 0.11.25**, a bounded sample queue, and linear resampling to the selected device's rate. The queue reserves room for two video-frame batches beyond the selected latency budget, without changing its playback start threshold. The host resampler does not yet adapt to long-term device clock drift. Only playback devices are opened. The source and licenses are recorded in the [DSP source notes](../src/third_party/snes_spc/README.md) and [miniaudio notes](../third_party/miniaudio/README.md).

Sound generation and deterministic sample delivery are implemented, but broad game compatibility and some S-SMP timing details remain work in progress. Current limits include PAL timing, non-default TEST modes, half-cycle port behavior, and the analog output stage. See [APU and sound synthesis](apu.md) for the hardware boundary and verification approach.
