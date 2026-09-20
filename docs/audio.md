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
| **DSP implementation** | Stub, **Third-party** (default), or Native; reset the game or restart the track to apply |
| **Sound interpolation** | **Gaussian (SNES)** by default, or Simple (linear); available for Third-party and applied on reset/restart |

The Third-party backend's Gaussian and linear interpolation share the same voices, sample decoding, envelopes, noise, pitch modulation, and echo. Gaussian uses the SNES interpolation filter. Linear changes how values between decoded samples are estimated; it can also affect voice output, pitch modulation, and echo. The interpolation preference is retained when selecting another backend. Seeking an SPC track preserves the active backend; restart applies pending settings.

Playback is active only during continuous execution at **100% emulation speed**. Slow motion and fast-forward are muted; audio time stretching is not implemented. Pausing, reset, loading another ROM, and changing playback speed discard queued sound so resuming does not play the previous moment. Hardware STOP instructions do not themselves silence the DSP while the machine keeps advancing.

**Refresh devices** updates the device list. If a device disconnects or cannot open, choose another output or use **Retry output**. The error remains in the audio panel and emulation can continue. If sound breaks up, try a higher latency. If nothing is audible, check that output is enabled, mute is off, volume is above zero, and the game is running at 100% speed.

For a known signal, run the synthetic **500 Hz stereo tone**:

```sh
./build/dev/pupsnes build/dev/test-roms/apu_audio.sfc
```

The normal development build produces this ROM with the other [test ROMs](test-roms.md).

## SPC music playback

Open **File → Load ROM or SPC...** in the emulator, or pass an SPC file at startup:

```sh
./build/dev/pupsnes "roms/Arcana SPC/33 Second Armageddon.spc"
```

The music panel displays the **album title above the smaller track title**, using the game name when no album tag is available. The **seek bar** shows elapsed and total time, including the tagged fade. Drag or click it and release to move to that position. Seeking preserves whether playback was paused; long jumps may briefly show “Seeking...” while the audio state is reconstructed silently. Files without a tagged length show elapsed time and “Track length unavailable.” **Pause/Resume** and **Space** control playback; **Restart** returns to the saved beginning. **Play again** restarts a finished track.

**Volume and Mute** are available directly in the music panel. Changes also appear in the Audio menu/settings and are saved with the existing preferences. The **Audio settings...** button opens device, latency, and interpolation options; restart the track to apply an interpolation change. Opening an SPC selects 100% speed. If output is disabled or playback speed mutes audio, the panel offers a button to restore sound.

The player reads **ID666 and xid6 duration and fade tags**, fades the output, and stops advancing the sound hardware at the tagged endpoint. Extended timing takes precedence and combines introduction, loop, and ending lengths; an omitted loop count defaults to two. Files without a usable duration continue until paused or replaced. Restarting preserves the pause state.

SPC playback runs the existing APU independently of the main CPU and PPU. It accepts v0.30 snapshots, including short legacy dumps containing the complete RAM and DSP images. The format omits internal DSP state and timer phases, so loading initializes those deterministically using the DSP's snapshot loader and snes_spc's first-cycle timer convention. Writable echo history is cleared on load so stale audio in the dump does not produce a startup burst; read-only echo memory is preserved. Restart and backward seeking use the same initialization. This is a portable music snapshot, not an exact emulator save state.

This first version supports individual files in the emulator and WAV tool. Playlists, compressed soundtrack archives, and SPC loading in the debugger are not implemented. Metadata display replaces non-ASCII characters with `?`; xid6 amplification and channel-mute tags are not applied. Historical ID666 text/binary timing fields are ambiguous, so the reader prefers valid decimal fields and otherwise reads binary values.

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

Select an implementation with `--dsp stub|third-party|native`. The default is
`third-party`; `--quality gaussian|linear` (also `simple` for linear) controls
only that backend. Stub and Native currently produce silent WAV files while
the sound CPU continues executing.

`--seconds` accepts whole numbers from 1 to 600; `--skip-seconds` accepts 0 to 600. Quality is `gaussian` by default, with `linear` as the alternative. Existing output files are preserved, so choose an unused filename. `--help` lists the options.

Use **`--spc`** instead of `--rom` to capture a standalone track:

```sh
./build/dev/pupsnes-audio --spc "roms/Arcana SPC/33 Second Armageddon.spc" --seconds 30 --output second-armageddon.wav
```

WAV capture always follows the explicit `--seconds` and `--skip-seconds` arguments. It records raw native PCM without applying metadata duration or fade, making it suitable for comparing emulator output.

## Saved preferences

The apps save audio preferences alongside their existing settings:

| Frontend | Default file |
| --- | --- |
| Emulator | `~/.pupsnes_emulator.ini` |
| Debugger | `~/.pupsnes_config.ini` |

The files store output enable, mute, volume, latency, a stable device identifier, and the pending DSP backend and interpolation mode. `sdsp_backend` stores `stub`, `third-party`, or `native`; the existing `sdsp_mode=0` (linear) / `1` (Gaussian) values remain compatible. Older files without a backend selection use Third-party. Invalid backend names are ignored. Invalid numeric text and nonfinite values are rejected; valid volume and latency values are bounded to supported ranges. An unavailable saved device produces a recoverable output error instead of silently selecting another device.

Set **`PUPSNES_CONFIG_DIR`** to use an existing directory for these files, keeping their filenames:

```sh
mkdir -p /tmp/pupsnes-prefs
PUPSNES_CONFIG_DIR=/tmp/pupsnes-prefs ./build/dev/pupsnes path/to/game.sfc
```

An unset or empty override uses the normal home-directory location, falling back to the current directory if `HOME` is unavailable. ImGui window-layout preferences remain separate in the working directory's `imgui.ini`; use a separate working directory as well when isolating GUI checks completely.

## Implementation and current limits

Third-party synthesis uses the pinned **blargg snes_spc 0.9.0** DSP core with local interpolation and address-wrap adaptations. **Stub** supplies DSP register readback, reset, ENDX clearing, and silent samples at the normal cadence. **Native currently shares the stub implementation**, reserving an independent backend for future homegrown synthesis. Neither runs imported DSP code or writes echo memory; ENVX/OUTX return to zero at each sample boundary. Neither models dynamic voice/end behavior, so games polling DSP status may behave differently. All three retain the real SPC700, IPL, timers, ARAM, and CPU-facing ports. The shared binaries still link the third-party library for selecting Third-party.

Host playback uses **miniaudio 0.11.25**, a bounded sample queue, and linear resampling to the selected device's rate. Only playback devices are opened. The source and licenses are recorded in the [DSP source notes](../src/third_party/snes_spc/README.md) and [miniaudio notes](../third_party/miniaudio/README.md).

When a delayed frame drains the queue, playback fades out and back in over **3 ms** to soften clicks. Each underrun adds **17 ms** to the refill target, up to the selected latency plus 34 ms, helping subsequent playback tolerate larger frame batches. Pausing, resetting, or otherwise flushing audio restores the normal startup target. The fixed queue reserves four video-frame batches beyond the selected latency, leaving room for recovery and incoming samples without delaying normal startup. Sustained emulation below real-time speed can still produce gaps, and the host resampler does not yet adapt to long-term device clock drift.

Sound generation and deterministic sample delivery are implemented, but broad game compatibility and some S-SMP timing details remain work in progress. Current limits include PAL timing, non-default TEST modes, half-cycle port behavior, and the analog output stage. See [APU and sound synthesis](apu.md) for the hardware boundary and verification approach.

SPC validation includes synthetic loading, port/timer restoration, hidden IPL RAM, repeatable playback, malformed metadata, and duration/fade boundaries. Ten-second Gaussian captures of all **35 Arcana SPC tracks** matched unmodified snes_spc 0.9.0 PCM exactly after accounting for its four-frame output-buffer delay. This comparison covers those excerpts, not every soundtrack or entire track. The development frontend was also exercised with **Second Armageddon** for playback and pause/resume.
