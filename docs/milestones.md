# Milestones

This document defines the major delivery milestones for PupSNES in dependency order. The goal is to keep the project pointed at the next meaningful vertical slice instead of expanding isolated subsystems without a bootable machine.

The architecture and subsystem design documents describe how the emulator should work. This file answers a different question: what should be built next, and how do we know a milestone is actually done?

## Current Position

The project already has strong core infrastructure in place:

- scheduler event ordering and authoritative device time ownership
- token-based async I/O plumbing
- system bus page table and plan/follow contract
- initial 5A22 CPU micro-op execution scaffold
- unit/integration coverage for scheduler, tokens, bus, and early CPU behavior

The main gap is that PupSNES still cannot load a cartridge, reset into a real memory map, or boot even a tiny ROM. That makes machine bring-up the next highest-leverage milestone.

## Milestone 0: Execution Core

Status: substantially complete

Objective:
Establish the timing and bus contracts that all later devices rely on.

Exit criteria:

- scheduler orders events deterministically
- devices run through `tick(budget)` with scheduler-owned committed time
- same-clock catch-up works through the scheduler contract
- cross-clock accesses can block and resume through tokens
- system bus supports page mapping and plan/follow behavior
- CI coverage exists for these core invariants

Notes:
Most of this is already present. Remaining work in this area should be driven by concrete integration needs, not speculative abstraction.

## Milestone 1: Headless Bring-Up

Status: next

Objective:
Boot a tiny ROM through the real reset path in a headless environment.

Scope:

- add cartridge loading for a minimal supported mapper, starting with LoROM
- add WRAM as a real device mapped through the system bus
- implement CPU reset/power-on entry behavior, including reset vector fetch
- add a small headless run loop in the CLI or a dedicated harness
- add integration tests that execute a handcrafted ROM instead of seeding CPU state directly

Exit criteria:

- PupSNES loads a tiny test ROM from disk or fixture bytes
- CPU starts from the reset vector rather than test-only register setup
- ROM code can read/write mapped memory through the system bus
- a test ROM can leave a known signature in WRAM and halt/spin predictably
- tests verify final observable state after a bounded run

Why this milestone matters:
This is the first point where the emulator becomes a machine instead of a collection of subsystems.

## Milestone 2: CPU Execution Baseline

Objective:
Implement enough of the 5A22 to run non-trivial diagnostic ROMs and small commercial boot sequences.

Scope:

- expand opcode coverage with correct addressing modes and flag behavior
- model stack operations, branches, jumps, subroutines, and interrupt entry/exit
- add direct page, data bank, program bank, emulation/native mode behavior
- improve reset/interrupt/vector handling
- grow test coverage from instruction micro-tests into ROM-driven execution tests

Exit criteria:

- CPU can pass a curated baseline of headless instruction/behavior tests
- basic reset and interrupt flows are modeled correctly enough for system bring-up
- integration tests no longer depend on ad hoc CPU register seeding for ordinary execution scenarios

Notes:
Do not chase full CPU completeness before Milestone 1 is done. Bootability comes first.

## Milestone 3: Video Path Skeleton

Status: scaffold landed (2026-04-22); advanced rendering deferred.

Objective:
Produce a deterministic frame pipeline, even if rendering is initially incomplete.

Scope:

- add PPU device scheduling and timing boundaries
- model enough PPU state to generate frame boundaries correctly
- wire the frontend `onFrameReady` callback
- begin implementing VRAM, OAM, and CGRAM storage with disciplined timing ownership

Exit criteria:

- emulator produces deterministic frame-ready events — **done**
- frame timing is scheduler-driven rather than frontend-driven — **done** (dot-major `Tick`; each scanline returns `kReachedLocalBoundary`)
- PPU register accesses flow through same-clock MMIO and catch-up correctly — **done** (lazy-replay: writes queue, reads catch up; see `docs/systembus.md`)
- framebuffer output exists, even if only for a limited rendering subset — **done** (backdrop-only: `cgram[0] × INIDISP.brightness`, or black under forced blank)

What landed in the scaffold:

- SPPU device with dot-major Tick using fullsnes timing (338 × 4 + 2 × 6 = 1364 mcyc per normal NTSC line, 1360 mcyc on the short V=240 line of the odd field)
- Per-device pending-write log (16384 entries, soft-limit flush on overflow) replaying writes in cycle order as the PPU advances dot-by-dot
- Real port protocols: INIDISP, CGRAM (write-twice + read-twice with bit-7 open-bus on the high byte), VRAM (VMAIN step / translate / inc-on-port + RDVRAM trailing-prefetch quirk), OAM (write-twice below $200, byte-wise above), SETINI overscan, STAT77/78 with field toggle
- Double-buffered 341 × 313 BGR555 framebuffer with `SNES::SetFrameReadyCallback(FrameBufferView)` fired at end of V=261
- Debugger PPU panel: decoded register table, pending-write log count, force-overscan toggle, GL-texture framebuffer preview

Deferred (covered by the Milestone 3 scope but not shipped in the scaffold):

- BG / OBJ / window / color-math / mode-7 rendering (only backdrop emits today)
- Interlace / hi-res (mode 5/6) actual output — dot-width table hooks exist, renderer still emits 256 logical columns
- NMI, H-IRQ, V-IRQ signals (these belong to Milestone 5's signal-region work; PPU frame boundaries don't depend on them)
- H/V-counter latch (SLHV, OPHCT, OPVCT) and mode-7 multiplier registers stay open-bus

PPU scheduling:

- `Ppu::Reset` auto-schedules the first `ScheduleDeviceRun` at the end of scanline 0 (1364 mcyc). `Tick` runs through as many scanlines as budget allows and yields at VSYNC (end of V=261) with `next_wake = committed_time`, so the scheduler chains subsequent frame dispatches without time-skipping. HBlank sync is internal to the dot loop — HV counters advance dot-by-dot and `DrainPendingWritesUpTo` fires at each dot's nominal start cycle.
- Strict-no-overshoot on both devices: `CPU::Tick` peeks the next micro-op's cost via `SystemBus::Plan` and refuses to start a step that would exceed budget (returns kNoWork with a `kMaxCyclesStep`-ahead wake on a tight slice, so the PPU's event gets a meaningful budget to run). `Ppu::Tick` uses sub-dot partial-cycle accounting to split atomic dots across multiple Ticks without overshooting.

Why this milestone matters:
Once frames exist, the project can start validating end-to-end console behavior rather than only CPU-local behavior.

## Milestone 4: Audio and Cross-Clock Integration

Objective:
Exercise the multi-clock design with real APU-facing behavior.

Scope:

- add APU-side scheduling and memory ownership
- implement CPU/APU port communication through cross-clock MMIO
- add audio sample callbacks
- validate token completion semantics against real cross-domain interactions

Exit criteria:

- CPU/APU ports behave through asynchronous completion rather than shortcuts
- emulator can emit deterministic audio samples or sample events
- scheduler behavior across master/APU domains is covered by integration tests

## Milestone 5: System Features Required For Real Software

Objective:
Add the hardware behavior that real games depend on beyond straight CPU execution.

Scope:

- DMA/HDMA
- IRQ/NMI/control signal region behavior
- controller input plumbing
- cartridge mapper expansion beyond the initial mapper
- bus-visible timing corner cases needed for software compatibility

Exit criteria:

- representative software can reach title or gameplay states headlessly or interactively
- interrupts and DMA participate in the scheduler without violating determinism
- controller polling works through the frontend contract

Notes:
This milestone should be split internally into smaller implementation plans once bring-up reveals the actual blockers.

## Milestone 6: State, Rewind, and Determinism Tooling

Objective:
Deliver the project-level features that depend on stable whole-machine state management.

Scope:

- contiguous state block or equivalent serialized state ownership
- save/load state support
- rewind ring buffer
- determinism verification harnesses
- trace and framebuffer comparison workflows

Exit criteria:

- emulator can save and restore machine state reliably
- repeated runs from the same state and input stream are byte-for-byte deterministic
- rewind works across meaningful gameplay intervals

## Milestone 7: Accuracy and Compatibility

Objective:
Turn a functioning emulator into a trustworthy one.

Scope:

- targeted timing corrections
- hardware edge cases
- compatibility triage against test ROMs and real games
- regression suites for CPU, bus, PPU, APU, DMA, and save-state behavior

Exit criteria:

- curated compatibility targets boot and behave correctly
- regression suites protect against timing and determinism regressions
- project decisions about accuracy vs. optional enhancements are documented explicitly

## Prioritization Rules

When choosing the next task, prefer work that:

1. unlocks a new end-to-end capability
2. validates an existing architecture claim with a real integration path
3. removes test-only scaffolding in favor of real machine behavior
4. exposes the next concrete bottleneck

Avoid spending major time on:

- polishing abstractions that have not yet been exercised by a boot path
- frontend UX before the core can boot a real ROM
- advanced features like rewind before machine state is real and stable

## Immediate Recommendation

The next build target should be Milestone 1: Headless Bring-Up.

If that milestone is broken into implementation steps, the recommended order is:

1. cartridge device plus minimal LoROM loader
2. WRAM device and initial system-bus memory map
3. CPU reset vector bring-up
4. tiny ROM-driven integration test
5. minimal CLI path to load and run a ROM headlessly
