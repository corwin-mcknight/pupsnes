# PupSNES 🐾

<!-- HERO IMAGE (pending): clean game-only frame (e.g. Super Castlevania IV / LTTP) at docs/images/hero.png -->

> PupSNES is a cycle-accurate **Super Nintendo (SNES)** emulator that **truly** prioritizes correctness and disavows hacks.

PupSNES is an accuracy-focused emulator that aims to run games not only quickly, but correctly. It models hardware behavior directly instead of approximating only the hardware's output.

The ultimate goal is to become the most accurate Super Nintendo emulator.

It's written in modern C++23, with a full debugger for inspecting the live system.

> **Status:** Under active development. The CPU, DMA/HDMA, and PPU Modes 0/1 are mature enough to run commercial games such as *Super Castlevania IV* and *The Legend of Zelda: A Link to the Past*. There is no audio yet, and not all PPU features are emulated. See the feature matrix below.

## What it is

The SNES is a multi-chip machine: a 65C816 CPU, a two-chip PPU for video, an SPC700 + S-DSP audio subsystem, DMA controllers, and cartridge-resident hardware — all running on their own clocks.

PupSNES mirrors that structure rather than flattening it: each chip is an independent device, time is counted in real master/APU clock cycles, and chips talk only through explicit buses and a signal-event scheduler. Accuracy is the non-negotiable: where a shortcut would diverge from real hardware, PupSNES takes the slower, correct path.

## Status

Legend: ✅ implemented · ⚠️ partial · ❌ not yet

| Subsystem | Status | Notes |
|---|---|---|
| **CPU — 65C816 (5A22)** | ✅ | All 256 opcodes, all modes, cycle-accurate memory timing |
| **DMA / HDMA** | ✅ | All channels and modes, cycle-accurate CPU stall |
| **PPU — backgrounds** | ⚠️ | Mode 0 & Mode 1 only |
| **PPU — sprites (OBJ)** | ✅ | Fully implemented. 32-per-line hardware cap enforced |
| **PPU — color math & screen** | ⚠️ | No windows, mosaic, hi-res, direct color |
| **Audio — APU (SPC700 + S-DSP)** | ❌ | **Silent.** Stub implementation allows games to boot. |
| **Input** | ⚠️ | Player 1 standard controller ✅; P2, multitap, mouse, Super Scope ❌ |
| **Cartridge / mappers** | ⚠️ | No coprocessors (SA-1, SuperFX, DSP-n…) |
| **Save states / rewind** | ❌ | Designed in [docs/architecture.md](docs/architecture.md), not yet implemented |
| **Deterministic scheduling** | ✅ | Signal-horizon scheduler with a total order over events |

## What runs today

PupSNES boots and runs **Mode 0 / Mode 1** games, rendering backgrounds, sprites, color math, and HDMA raster effects. The following titles boot into gameplay and are playable — they have **not** been tested through to completion:

- **Super Castlevania IV**
- **The Legend of Zelda: A Link to the Past**

<!-- DEBUGGER SCREENSHOT (pending re-drop): docs/images/debugger.png -->

What to expect right now:

- 🔇 **No audio** — everything is silent.
- 🌀 **No Mode 7** — affine/rotation effects don't render (e.g. *F-Zero*, *Super Mario Kart*, the LTTP world-map screen).
- 🎮 **Player 1 only** — no second controller or peripherals.
- 💾 **No save states or rewind** yet (battery SRAM *does* persist to `.srm`).
- 📦 **LoROM / HiROM / ExHiROM only** — no SA-1 / SuperFX / DSP enhancement-chip games.

## Feature support

A finer-grained checklist for the curious.

**CPU — 65C816 (5A22)** · complete & cycle-accurate
- ✅ All 256 opcodes — no illegal/unimplemented gaps
- ✅ Emulation (E=1) and native (E=0) modes; 8/16-bit accumulator & index (M/X) handling
- ✅ Every addressing mode (direct page, stack-relative, long, indexed, indirect, block-move…)
- ✅ Decimal (BCD) `ADC`/`SBC`, 8- and 16-bit, with correct overflow behavior
- ✅ Interrupts & vectors: NMI, IRQ, BRK, COP, RESET (ABORT scaffolded), with proper entry/exit
- ✅ `MVN`/`MVP` block moves, `WAI`/`STP`, all read-modify-write forms
- ✅ Per-access bus timing (FastROM / slow ROM / WRAM / MMIO), DRAM refresh, branch & page-cross penalties

**PPU — video**
- ✅ Background **Mode 0**, ✅ **Mode 1** (incl. BG3 priority elevation)
- ❌ Modes **2, 3, 4, 5, 6**, ❌ **Mode 7** (affine rotation/scaling)
- ✅ Sprites/OBJ: all 8 size pairs, flips, priority, palette groups, 32-sprite/line cap
- ✅ Color math (add / subtract / half), ✅ fixed color (COLDATA), ✅ main & sub screen, ✅ INIDISP brightness, ✅ overscan
- ✅ VRAM / OAM / CGRAM port protocols (VMAIN translate & increment, RDVRAM prefetch quirk, write-twice/read-twice latches)
- ❌ Windows (W1/W2 + logic), ❌ mosaic, ❌ hi-res / interlace output (Mode 5/6), ❌ direct color, ❌ offset-per-tile

**DMA / HDMA**
- ✅ General-purpose DMA: 8 channels, transfer modes 0–7, fixed/increment/decrement addressing, both directions
- ✅ HDMA: per-scanline transfers, direct + indirect, line counter & repeat flag, table reload
- ✅ Cycle-accurate: 8 master cycles/byte with correct CPU stall; `MDMAEN`/`HDMAEN`

**Audio — APU** · not implemented
- ❌ SPC700 CPU core · ❌ S-DSP (BRR decode, ADSR, echo/FIR, voices) · ❌ audio output
- ⚠️ IPL handshake stub only: fakes the `$2140`–`$2143` boot signature so games proceed past the APU check — **no sound**, and games that drive timing from the APU may desync

**Input**
- ✅ Player 1 standard controller — auto-read (`$4218`/`$4219`) and serial (`$4016`)
- ❌ Player 2, multitap, mouse, Super Scope

**Cartridge / mappers**
- ✅ LoROM, ✅ HiROM, ✅ ExHiROM
- ✅ Battery SRAM persisted to `.srm`; ✅ FastROM; ✅ SMC-header detection & structured ROM validation
- ❌ ExLoROM; ❌ coprocessors — SA-1, SuperFX/GSU, DSP-1/2/3/4, CX4, S-DD1, SPC7110, OBC1 (detected, load-rejected)

**System**
- ✅ Deterministic signal-horizon scheduler (total order by master time, signal kind, sequence)
- ❌ Save states · ❌ rewind — the contiguous "State Block" is designed in [docs/architecture.md](docs/architecture.md) but not built

## Building & running

Requires CMake + Ninja. Conan dependencies install automatically on configure — see [BUILDING.md](BUILDING.md) for details.

```sh
cmake --preset dev
cmake --build --preset dev
```

Run a ROM in the emulator:

```sh
./build/dev/pupsnes path/to/game.sfc
```

Or launch the debugger UI with live CPU / PPU / DMA / memory panels:

```sh
./build/dev/pupsnes-debugger path/to/game.sfc
```

Tests live in [TESTING.md](TESTING.md) — e.g. `./build/ci/pupsnes_tests "[cpu]"`.

## Architecture & docs

PupSNES is built around a signal-horizon scheduler, with each chip an independent, cycle-clocked device on explicit buses.

- [docs/architecture.md](docs/architecture.md) — overall design and device model *(describes the target design; items marked "planned" aren't built yet)*
- [docs/scheduler.md](docs/scheduler.md) — signal-event scheduling and ordering
- [docs/systembus.md](docs/systembus.md) — system bus, page tables, same-clock vs. cross-clock MMIO
- [docs/cpu-opcodes.md](docs/cpu-opcodes.md) — the 5A22 opcode authoring model
- [docs/screenshots.md](docs/screenshots.md) — rendering frames from a ROM headlessly
- [docs/test-roms.md](docs/test-roms.md) — the in-repo test-ROM build pipeline

## Roadmap

In rough priority order:

1. **Audio** — SPC700 core + S-DSP (BRR, ADSR, echo), cross-clock CPU↔APU integration, sample output.
2. **PPU completeness** — Mode 7, remaining background modes, windows, mosaic, hi-res / interlace.
3. **Save states & rewind** — the contiguous State Block, snapshot/restore, and a rewind ring buffer.
4. **More cartridges** — additional mappers and enhancement chips (SA-1, SuperFX, DSP-n…).
5. **Enhancements beyond accuracy** — opt-in extras (e.g. a widescreen PPU), gated so they never compromise the accurate core.

## Contributing

Contributions are welcome. Start with [BUILDING.md](BUILDING.md) and [TESTING.md](TESTING.md), and read the architecture docs above. PupSNES values **correctness over cleverness** — changes should match real hardware behavior rather than introduce shortcuts that merely happen to work.

## License

PupSNES is licensed under the **GNU General Public License v3.0** — see [LICENSE](LICENSE). You're free to use, study, modify, and share it; derivative works must stay open under the same license.

```
PupSNES — a cycle-accurate Super Nintendo emulator
Copyright (C) 2026 Corwin McKnight

This program is free software: you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation, version 3.

This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
Public License for more details.
```
