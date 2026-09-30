# Correctness gaps

## PPU memory access and sampling phases

**The PPU lacks hardware-specific access and fetch phases.** VRAM writes currently update memory regardless of whether rendering is active. OAM and CGRAM accesses likewise use simplified port behavior. The renderer consumes state through dot-based replay without separately modeling the hardware's memory fetch and register-latching phases.

Correct implementation must account for when memory is available, which address or latch an access affects, and when each value is consumed by rendering. Placing a write at the correct dot does not by itself establish those behaviors. Investigate each port and register family against hardware evidence, including active display, blanking transitions, and writes around fetch boundaries.

**A full pending-write log can lose pixel timing.** When its 16,384-entry log fills, `Ppu::EnqueueWrite` replays queued writes immediately up to the new write's time. The writes are retained, but pixels not yet emitted can observe state too early. Preserve dot-order rendering when draining a full log, and test a write-heavy frame across the capacity boundary.

## DMA, HDMA, and refresh scheduling

**DMA still executes as an inline burst.** `DmaController::Trigger` performs the complete transfer and returns its end time; the CPU register handler then advances machine time to that endpoint. The transfer has timestamped accesses, but its interaction with events that should interleave with it needs investigation, especially HDMA and refresh. The audit did not establish a particular game failure from this interaction.

HDMA retains approximate initialization timing, incomplete CPU/DMA clock alignment and channel overhead, and a fixed last transfer line of 224 without overscan support. Refresh advances its deadline by a fixed 1364 master cycles, making its relationship to the PPU's short-line cadence another concrete investigation target.

Establish bus ownership, pending transfers, legal interruption points, alignment, and CPU resumption from the hardware rules. Prefer scanline-aware timing over independently accumulated fixed periods where the hardware follows the beam.

## APU and DSP

**The Native and Stub S-DSP backends are silent placeholders.** `NativeSdsp` inherits `StubSdsp`, which returns zero samples; the functional Third-party backend uses the imported `snes_spc` engine. The Simple interpolation option changes that engine to approximate linear interpolation. Implement native synthesis before treating Native as an audio-capable backend, and validate it against the accurate backend with register, sample, and playback comparisons.

**APU communication retains timing approximations.** Ports use whole-SPC-cycle sampling and do not model half-cycle sampling or physical port collisions. Timer target-write glitches and non-default TEST modes also remain incomplete; unsupported TEST modes currently fault explicitly. The nominal oscillator choice is a stated model parameter, not a measurement of an individual console.

## Other observable state and hardware scope

**PPU open bus remains partially implemented ([#15](https://github.com/corwin-mcknight/pupsnes/issues/15)).** STAT77 bit 4 now comes from a PPU1 read latch updated by reads of `$2134–$2136`, `$2138–$213A`, and `$213E`, following [Anomie's register documentation](https://raw.githubusercontent.com/gilligan/snesdev/master/docs/snes_registers.txt). CPU bus activity, PPU writes, and PPU2 reads do not replace this latch. Emulator reset initializes it to zero for determinism; this is not a measured hardware power-on value. PPU1 write-only register readback and separate PPU2 latch behavior remain unimplemented; other floating bits still use the CPU latch. Those remaining register sequences and latch ownership rules stay in #15.

**Reset policies are conflated.** Current reset paths use cold-start initialization, including deterministic memory/register seeds in several devices. Separate power-on, reset-button, and deterministic test initialization policies, preserving state where the selected hardware reset requires it.

**CPU ABORT is scaffolded but not delivered.** `CPU::SelectPendingInterrupt` handles NMI and IRQ while its ABORT branch is commented out. Model the signal and vector only when there is an actual ABORT source, with tests for interrupt priority and return state.

**Graphics and region support are incomplete.** Background Modes 0–4 and 7 have rendering paths; Modes 5 and 6 currently resolve to backdrop color. High-resolution and interlaced output are absent. The PPU uses NTSC frame timing even though cartridge detection records a PAL region, and `$213F` reports NTSC. Add the missing modes and output formats, then select region-specific timing from the loaded cartridge.

**Cartridge and controller support is limited.** Only plain LoROM, HiROM, and ExHiROM builders are registered; detected enhancement chips are rejected. ExHiROM currently uses HiROM SRAM placement despite a documented board variant requiring another mapping. When a ROM header has no usable map-mode byte or checksum, detection assumes LoROM, which can misclassify ambiguous images. The two standard controller ports are supported; other peripherals are absent. Extend these as general device and board implementations, and test ambiguous headers explicitly.

## Recently resolved

**Timed automatic controller polling — September 28, 2026.** When enabled through `$4200.0`, polling begins on a 256-master-cycle phase shortly after VBlank entry. It latches both standard controllers, clocks 16 serial bits over 4,224 master cycles, exposes the busy interval in `$4212.0`, and retains the completed `$4218–$421B` words until another enabled poll. Manual reads use the same serial position; disabling mid-poll stops further shifts. Focused tests inspect start/end boundaries, partial results, input changes after latching, enable transitions, and manual-read interference. The phase and edge model follow [measured SNES timing](https://wiki.superfamicom.org/timing) and the [bsnes poll state machine](https://github.com/bsnes-emu/bsnes/blob/master/bsnes/sfc/cpu/timing.cpp); these tests are emulator behavior checks, not a new hardware measurement.

**Sprite selection, fetch limits, and STAT77 — September 27, 2026.** Rotated OAM selection, the 32-sprite limit, reverse-order fetching with a separate 34-tile budget, off-screen clipping and X=-256 handling are implemented. Selection and two-word graphics fetches now run on the preceding line's dot timeline. Sampled tile rows remain latched; palette, screen/window enables and color math remain live. STAT77 raises overflow during preparation, preserves it through reads and VBlank, and clears it at frame start. OAM port increments affect rotation, and non-forced VBlank entry reloads the OAM address.

**Twenty-three focused tests cover sprite limits and timing.** The [pipeline design and evidence](docs/obj-pipeline.md) describe the hardware diagnostic cases, timed sampling, and batching invariant. The first-pixel scanline snapshot has been removed. Active-display OAM redirection, VRAM port restrictions, odd-address rotation quirks, interlaced OBJ and sub-dot blanking collisions remain part of the PPU access/sampling gap; the tests are not a new physical-console measurement.

**WRAM data and address ports — September 12, 2026.** `$2180–$2183` now access the same memory as directly addressed WRAM, with a shared 17-bit address, automatic increment and wrap, and write-only address registers. DMA and forward HDMA suppress conflicting WRAM-to-WRAM port accesses without advancing the port address. Ten regression tests cover port behavior, CPU execution of uploaded code, valid DMA transfers, and WRAM transfer conflicts. The broader DMA scheduling gaps above remain open.

**Final Fantasy III's startup failure was traced to discarded WRAM-port writes.** The game decompressed executable code through `$2180`, then jumped to `$7E:5000`; the missing writes left zero-filled memory, causing BRK execution, an unmapped-access flood, and screen corruption. Both local USA revisions now reach a clean title screen, with no BRK or unmapped accesses during the verified 600-frame startup runs. The user also confirmed that **Aerobiz and Chrono Trigger now boot** after this fix. Their exact dependency on the ports has not been traced; a shared code-upload or decompression mechanism is plausible, but remains unverified. These observations establish startup progress, not full-game compatibility.

**OAM high-table write buffering — September 12, 2026.** Even-addressed writes now update the shared write buffer in both OAM tables. This fixes `snes_oam_test/3-high` test #5, which read `00 34` instead of `12 34`. All three OAM ROMs (`1-random`, `2-low`, and `3-high`) pass, with regression coverage for buffer retention across reads and odd writes. This corrects a port-latch rule during forced blank; active-display access restrictions and rendering fetch phases remain open above.

Validation at the time of these fixes passed **730 unit tests**. That count is historical, not a current suite result. Existing non-blocking lint issues remain.
