# Correctness gaps

## Automatic controller polling

**The automatic-read path is a stub.** The result registers return the current button state directly. They do not honor the automatic-polling enable bit, capture a timed snapshot, perform the polling sequence, or expose its busy period. This can appear correct when someone holds a button for several frames while still exposing behavior that differs from the console.

Implement the polling state machine, including enable transitions, capture and shift timing, result-register updates, busy status, and interaction with manual controller reads. Tests should change input around polling boundaries and observe the registers during the sequence, rather than checking only a completed button word.

## Sprite evaluation and status

**Sprite evaluation uses a simplified rendering model.** `EvaluateObjLine` collects up to 32 sprites in ascending OAM order. The priority-rotation bit is stored but does not affect that evaluation. The separate 34-tile fetch limit is absent. `STAT77` currently drives only the version bits instead of reporting the sprite overflow flags.

Implement sprite selection and tile-fetch limits separately, with the correct ordering, priority rotation, and status-latch behavior. The current comment claiming that visible behavior matches hardware is too broad: dropping sprites after a 32-sprite cap does not reproduce all hardware resource limits. Validation should include wide sprites, off-screen positions, rotation, and cases that exceed one limit without exceeding the other.

## PPU memory access and sampling phases

**The PPU lacks hardware-specific access and fetch phases.** VRAM writes currently update memory regardless of whether rendering is active. OAM and CGRAM accesses likewise use simplified port behavior. The renderer consumes state through dot-based replay without separately modeling the hardware's memory fetch and register-latching phases.

Correct implementation must account for when memory is available, which address or latch an access affects, and when each value is consumed by rendering. Placing a write at the correct dot does not by itself establish those behaviors. Investigate each port and register family against hardware evidence, including active display, blanking transitions, and writes around fetch boundaries.

## DMA, HDMA, and refresh scheduling

**DMA still executes as an inline burst.** `DmaController::Trigger` performs the complete transfer and returns its end time; the CPU register handler then advances machine time to that endpoint. The transfer has timestamped accesses, but its interaction with events that should interleave with it needs investigation, especially HDMA and refresh. The audit did not establish a particular game failure from this interaction.

HDMA retains approximate initialization timing, incomplete CPU/DMA clock alignment and channel overhead, and a fixed last transfer line of 224 without overscan support. Refresh advances its deadline by a fixed 1364 master cycles, making its relationship to the PPU's short-line cadence another concrete investigation target.

Establish bus ownership, pending transfers, legal interruption points, alignment, and CPU resumption from the hardware rules. Prefer scanline-aware timing over independently accumulated fixed periods where the hardware follows the beam.

## Other observable state and hardware scope

**PPU open bus uses the wrong level of shared state.** Floating bits are merged from the CPU's data-bus latch instead of separate PPU1 and PPU2 latch domains. Model the relevant latch ownership and updates so cross-register read sequences produce the appropriate values.

**Reset policies are conflated.** Current reset paths use cold-start initialization, including deterministic memory/register seeds in several devices. Separate power-on, reset-button, and deterministic test initialization policies, preserving state where the selected hardware reset requires it.

**APU communication retains timing approximations.** Ports use whole-SPC-cycle sampling and do not model half-cycle sampling or physical port collisions. Timer target-write glitches and non-default TEST modes also remain incomplete; unsupported TEST modes currently fault explicitly. The nominal oscillator choice is a stated model parameter, not a measurement of an individual console.

**Some hardware configurations and graphics features remain unsupported.** PAL/interlace timing, background Modes 2–6, and some cartridge-board layouts are incomplete. ExHiROM currently shares the HiROM SRAM placement despite a documented board variant requiring another mapping.

## Recently resolved

**WRAM data and address ports — September 12, 2026.** `$2180–$2183` now access the same memory as directly addressed WRAM, with a shared 17-bit address, automatic increment and wrap, and write-only address registers. DMA and forward HDMA suppress conflicting WRAM-to-WRAM port accesses without advancing the port address. Ten regression tests cover port behavior, CPU execution of uploaded code, valid DMA transfers, and WRAM transfer conflicts. The broader DMA scheduling gaps above remain open.

**Final Fantasy III's startup failure was traced to discarded WRAM-port writes.** The game decompressed executable code through `$2180`, then jumped to `$7E:5000`; the missing writes left zero-filled memory, causing BRK execution, an unmapped-access flood, and screen corruption. Both local USA revisions now reach a clean title screen, with no BRK or unmapped accesses during the verified 600-frame startup runs. The user also confirmed that **Aerobiz and Chrono Trigger now boot** after this fix. Their exact dependency on the ports has not been traced; a shared code-upload or decompression mechanism is plausible, but remains unverified. These observations establish startup progress, not full-game compatibility.

**OAM high-table write buffering — September 12, 2026.** Even-addressed writes now update the shared write buffer in both OAM tables. This fixes `snes_oam_test/3-high` test #5, which read `00 34` instead of `12 34`. All three OAM ROMs (`1-random`, `2-low`, and `3-high`) pass, with regression coverage for buffer retention across reads and odd writes. This corrects a port-latch rule during forced blank; active-display access restrictions and rendering fetch phases remain open above.

Validation after these fixes passed **730 unit tests**. Existing non-blocking lint issues remain.
