# Milestones

Updated September 9, 2026.

PupSNES has working backgrounds, sprites, controller input, and battery-backed saves. **Sound is the next major milestone**, followed by the missing graphics features, save states, and support for more cartridges.

This roadmap describes where the project stands and what each milestone will make possible. The milestones overlap: work on accuracy and compatibility continues throughout development.

## Where we are

The CPU implements all 256 instructions, and the core supports cartridge loading, memory, interrupts, and DMA/HDMA transfers. Mode 0 and Mode 1 graphics work, along with sprites and color blending. LoROM, HiROM, and ExHiROM cartridges are supported, and the debugger provides live inspection, execution traces, and screenshots.

Super Castlevania IV and The Legend of Zelda: A Link to the Past reached playable gameplay with the former APU handshake stub, though neither was tested through to completion. The stub has now been replaced by a real SPC700 core with all instructions implemented; commercial sound programs can still stop on missing APU hardware. Games are still silent, several graphics effects are missing, and controller support is limited to player one.

| Milestone | Status |
| --- | --- |
| 0. Execution core | Established |
| 1. Booting ROMs | Complete |
| 2. CPU execution | Baseline complete; accuracy work continues |
| 3. Graphics | Partially complete |
| 4. Audio | **Next priority** |
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

**In progress.** The SPC700 now implements all 256 instructions, executes the real IPL boot program, and receives and runs uploaded sound programs. Test programs exercise calculations, branches, subroutines, and communication across different execution slice sizes. Timers and sound synthesis are still missing; accesses to unimplemented hardware stop with a diagnostic. See [APU bring-up](apu.md).

Real sound support has three main parts:

1. **Run the sound CPU.** Implement the SPC700, its memory and timers, and communication with the main CPU so games can upload and execute their sound programs.
2. **Generate sound.** Implement the S-DSP’s voices, sample decoding, envelopes, mixing, and effects such as echo.
3. **Play it back.** Connect the generated audio to the frontend with stable playback that stays synchronized with the game.

**The goal:** games initialize the sound hardware normally and produce music and sound effects, with repeatable output and reliable timing. Boot, upload, and all SPC700 instructions are implemented. The next steps are timers and DSP synthesis.

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

The current order is **audio, graphics completeness, save states and rewind, then broader cartridge support**. Controller improvements and fixes for known game problems can progress alongside those larger efforts.

Real sound-program upload and all SPC700 instructions are now implemented. The immediate focus is timers and DSP behavior so commercial sound programs can initialize the hardware, followed by hearing the first synthesized sound.
