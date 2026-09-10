# Milestones

Updated September 10, 2026.

PupSNES has working backgrounds, sprites, controller input, battery-backed saves, and **stereo sound synthesis and playback**. The next major feature work is graphics completeness, followed by save states and support for more cartridges.

This roadmap describes where the project stands and what each milestone will make possible. The milestones overlap: work on accuracy and compatibility continues throughout development.

## Where we are

The CPU implements all 256 instructions, and the core supports cartridge loading, memory, interrupts, and DMA/HDMA transfers. Mode 0 and Mode 1 graphics work, along with sprites and color blending. LoROM, HiROM, and ExHiROM cartridges are supported, and the debugger provides live inspection, execution traces, and screenshots.

Super Castlevania IV and The Legend of Zelda: A Link to the Past reached playable gameplay with the former APU handshake stub, though neither was tested through to completion. The stub has been replaced by a complete SPC700 instruction set, timers, and the S-DSP voice and effects pipeline. Both frontends can play the resulting audio, and a command-line tool can capture it to WAV. Commercial-game sound still needs broader validation; several graphics effects are missing, and controller support is limited to player one.

| Milestone | Status |
| --- | --- |
| 0. Execution core | Established |
| 1. Booting ROMs | Complete |
| 2. CPU execution | Baseline complete; accuracy work continues |
| 3. Graphics | **Next major feature priority** |
| 4. Audio | Synthesis and playback implemented; compatibility work continues |
| 5. Controllers and cartridges | Partially complete |
| 6. Save states and rewind | Planned |
| 7. Accuracy and compatibility | Ongoing |

## Milestone 0: Execution core

**Established.** The foundation keeps the CPU, graphics hardware, memory, and other devices moving together on the console’s timeline. Events happen in a predictable order, and memory accesses account for the time they take on the hardware.

This makes repeatable emulation possible and provides the foundation for accurate interactions between devices. The initial SPC700 now runs on its own nominal clock with deterministic conversion from master time.

## Milestone 1: Booting ROMs

**Complete.** PupSNES loads cartridges, starts the CPU from the ROM’s reset vector, and runs programs against the console’s memory map. Small test ROMs exercise this process automatically, and command-line tools can run the emulator without opening a window.

This milestone established a working machine capable of running software. That boot path now supports both game development and emulator testing.

## Milestone 2: CPU execution

**Baseline complete.** All 256 instructions are implemented, including the CPU’s addressing modes, arithmetic, stack operations, branches, and interrupts. Games can run substantial programs using both native and emulation modes.

The remaining work concerns accuracy in unusual cases and interactions with the rest of the console. Diagnostic ROMs and game failures will continue to reveal timing details that need refinement; complete instruction coverage does not mean CPU accuracy work is finished.

## Milestone 3: Graphics

**Partially complete.** Mode 0 and Mode 1 backgrounds, sprites, priorities, color blending, brightness, and overscan are implemented. The PPU also handles its video-memory ports, beam-position latches, and Mode 7 multiplication registers.

The biggest missing feature is **Mode 7 rendering**, which provides the rotating and scaling backgrounds used by games such as F-Zero and Super Mario Kart, as well as the world map in A Link to the Past. The multiplication registers are ready, but the graphics themselves are not yet drawn.

Other remaining features include windows and masking, mosaic effects, background Modes 2–6, direct color, offset-per-tile effects, and high-resolution and interlaced output.

**The goal:** games using these features display their scenes and effects correctly, while existing Mode 0/1 games retain their current behavior. Small graphics test ROMs and comparisons of known scenes will help establish progress.

## Milestone 4: Audio

**Synthesis and playback implemented.** Games can upload and run sound programs through the real IPL boot path. The sound CPU has all 256 instructions and three working timers. The S-DSP produces eight voices with sample decoding, envelopes, mixing, noise, pitch modulation, and echo.

The emulator and debugger provide volume, mute, output-device, and latency controls. Gaussian interpolation follows the SNES sound filter; optional linear interpolation uses the same voices and effects. Playback runs at normal speed, and the command-line audio tool records native stereo WAV files without a sound device. An uploaded test ROM produces a repeating 500 Hz tone through this complete path. See [Audio](audio.md) for how to use it.

**The remaining work:** establish music and sound-effect compatibility across more games, refine sound-hardware timing, and improve playback where real devices reveal problems. Fast-forward and slow-motion playback, PAL timing, and analog output behavior are still outside the current implementation. The technical boundaries are documented in [APU and sound synthesis](apu.md).

## Milestone 5: Controllers and cartridges

**Partially complete.** DMA/HDMA, interrupts, FastROM, and the main cartridge formats already support running games. Player-one input works through both serial reads and automatic-read registers, although automatic polling still needs the hardware’s capture and busy timing.

The next input improvements are accurate automatic polling and a second standard controller. Multitap, mouse, and Super Scope support remain further possibilities.

Cartridge support currently covers LoROM, HiROM, and ExHiROM, including battery-backed SRAM saves. Additional layouts and enhancement chips such as SA-1, SuperFX, and DSP-n will open up more of the SNES library. Each chip family is a substantial project with its own compatibility targets.

**The goal:** broaden the games and multiplayer experiences PupSNES can support, with dependable input, cartridge behavior, and persistent saves.

## Milestone 6: Save states and rewind

**Planned.** Games can already keep their normal battery-backed saves. Save states will let a player pause at any moment and resume from that exact point; rewind will make it possible to move backward through recent gameplay.

Both features depend on capturing the entire running machine, including work in progress inside the CPU, graphics, sound, and transfer hardware. Restoring a state should reproduce the same behavior as uninterrupted play, including the picture and sound.

**The goal:** reliable save and load at arbitrary points during gameplay, followed by smooth rewind built on the same foundation. The proposed state design is described in [architecture](architecture.md).

## Milestone 7: Accuracy and compatibility

**Ongoing.** Every new feature expands what can run, but correctness also depends on the small details: timing, register behavior, interactions between chips, and unusual software techniques.

Progress here means more diagnostic tests passing, fewer visual and audio errors, and more games working reliably beyond their opening scenes. Compatibility reports should make clear whether a game boots, reaches gameplay, or has been played through to completion.

**The goal:** an emulator whose behavior is both faithful to the hardware and dependable across a growing part of the SNES library. Reproducible tests and recorded examples of failures help keep fixed problems from returning.

## What comes next

The current order is **graphics completeness, save states and rewind, then broader cartridge support**. Controller improvements, audio compatibility, and fixes for known game problems can progress alongside those larger efforts.

**Mode 7 rendering** is the clearest next feature: it would make rotating and scaling backgrounds visible in more games. Audio now has an end-to-end path from uploaded sound programs to speakers and WAV files; further work there should build on concrete music, effect, and timing problems found in games.
