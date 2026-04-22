# Architecture

PupSNES simulates the SNES as real hardware: independent devices, explicit signals, and edge-driven time, with correctness and determinism as first-class goals. We avoid proprietary SNES documentation; the goal is to describe behavior and scheduling, not reproduce copyrighted manuals.

Devices are clocked at a certain rate based on the clock they are connected to, and all devices are clocked at the same rates as they are on the SNES. Time is measured in integer master clock cycles and integer APU clock cycles.

Each device is modeled separately, and has its inputs, its outputs, and internal state modeled. Devices are emulated with C++ code and are not actually composed of wires.

Devices fall into two roles:

- **`MasterClockDriver`** (CPU; future DMA, HDMA, coprocessors): implements
  `TickToTarget(TimeMasterT target)`, writes global master time as work retires,
  and drives the RunControl loop forward.
- **Passive `Device`** (PPU, bus responders): implements `CatchUpTo(TimeMasterT target)`
  to advance internal state on demand. The default is a no-op for stateless devices.

See `docs/scheduler.md` for the full scheduler design.

## Object Model

PupSNES has a dynamic topology of interconnected objects that represent the SNES hardware. The main components are:

* **SNES**: The top-level object that contains all other components. Owns global master and APU time, and the Scheduler instance.
* **Scheduler**: Part of the SNES. Maintains a signal-event priority queue ordered by `(master_time, SignalKind, seq)`. Exposes `ScheduleSignal`, `NextEventMasterTime`, `FireEventsThrough`, and `SnapshotSignalQueue`. Does not dispatch device execution — devices run via `TickToTarget` or `CatchUpTo` outside the queue.
* **Device**: Base class for all hardware units. Holds `local_time_`, a `SNES*` back-pointer, and a `DeviceIdT`. Passive devices override `CatchUpTo(target)`; master-clock drivers subclass `MasterClockDriver` and implement `TickToTarget(target)`. `CpuMmio` subclasses `Device` for bus-page dispatch but keeps the default no-op `CatchUpTo`.
* **SystemBus**: Authority for CPU-visible bus transactions. Decodes addresses via a flat page table (256×256 bank×page → device+offset), precalculated at ROM load. Same-clock targets are synchronized via catch-up on access; cross-clock targets use async tokens. See `docs/systembus.md`. (planned)
* **State Block**: A single contiguous memory allocation containing all device state — registers, counters, VRAM, WRAM, APU RAM. Save state = `memcpy`. Rewind = ring buffer of snapshots. Determinism verification = `memcmp`. (planned)
* **Signal Region**: Part of the State Block. Models control signals (NMI, IRQ, HALT) as flat fields rather than wire objects. Edge-triggered signals (NMI) use two fields (current + previous level). Level-triggered signals (IRQ, HALT) use one field. (planned)
* **Port**: Allows connecting external peripherals to the SNES, such as controllers or cartridges. (planned)

All devices have a stable numeric ID (`device_id_t`) for deterministic ordering and save-state support.

## Synchronization Model

PupSNES uses **lazy-replay catch-up** for same-clock-domain synchronization. The rule is: **writes queue, reads catch up.**

- **Reads catch up**: when a bus master issues a read targeting a same-clock device, `CatchUpTo` is called on the target before the read samples state. The target advances its `local_time_` to match the bus cycle.
- **Writes queue**: writes append to a per-device pending-write log `{cycle, offset, data}` tagged with the cycle they arrived on. No catch-up fires on the write path. `CatchUpTo` replays the log in order as it advances (dot-by-dot for the PPU). Any queued writes at or before the read cycle are also drained inside the read handler, so the lazy-replay contract holds even when the per-dot drain fell slightly short.

This approach matches the cost profile of real workloads: DMA and HDMA bursts write hundreds of MMIO bytes per frame, and forcing a catch-up on every write would dominate the inner loop. Correctness is preserved because replay order equals write order (enqueues are monotonic in cycle) and reads never sample stale state.

The `RunControl` loop calls `SNES::MachineSync(now)` — which invokes `CatchUpTo(now)` on every registered passive device — before firing signal events. This ensures handlers always see a fully-synced machine.

**Bus masters** (CPU, DMA) initiate transactions and drive the clock forward. **Bus targets** (PPU registers, WRAM) respond to transactions and can be caught up. Catch-up applies only to targets — devices whose internal state evolution does not require issuing bus transactions. On the SNES, the PPU qualifies because it uses its own VRAM bus during rendering and never initiates system bus transactions.

Cross-clock-domain transactions (e.g., CPU ↔ APU via ports `$2140`–`$2143`) use asynchronous tokens. The token is queued and resolved when the target is caught up to the equivalent time in its own clock domain.

Tokens are the universal abstraction for external I/O. Same-clock tokens resolve synchronously (via lazy-replay catch-up). Cross-clock tokens resolve asynchronously. The token type is the same; the resolution semantics differ by clock domain.

## Clock Domains

Two independent timing domains:

* **Master clock** (~21.477 MHz): drives CPU, PPU, DMA. All same-clock synchronization uses catch-up on access.
* **APU clock** (~1.024 MHz): independent crystal. The APU tracks its own cycle count with a known conversion ratio to master time (~20.97 master cycles per APU cycle). On port access (`$2140`–`$2143`), the APU is caught up by converting the current master time to APU cycles.

Time is measured in integer cycles (no fractional time) within each domain.

## State Management

All device state resides in a single contiguous **State Block** allocation. This includes:

* Device registers and internal counters
* Large memory arrays (128KB WRAM, 64KB VRAM, 64KB APU RAM, OAM, CGRAM)
* Scheduler state (event queue as fixed-size array, sequence counters)
* Signal region (NMI, IRQ, HALT fields)
* Token table (fixed-size, bounded by max outstanding tokens)

Save state is `memcpy` of the block. Rewind is a ring buffer of block snapshots. Determinism verification is `memcmp` of two blocks.

Device code accesses state through typed overlays (POD structs placed at known offsets in the block), providing natural field access with zero overhead.

## Memory Map

Address decoding uses a flat **page table**: a 256×256 array indexed by bank and page (256-byte granularity). Each entry contains a target device pointer and a device-relative offset. The table is precalculated at ROM load time based on the cartridge mapper type (LoROM, HiROM, etc.).

* Mirrors are free — multiple entries point to the same backing memory/handler.
* I/O register ranges (`$2100`–`$44FF`) point to device handlers with a flag distinguishing them from raw memory.
* Co-processor cartridges overlay their regions by modifying table entries.

This is the hottest path in the emulator (called on every bus cycle), so O(1) lookup with no branching is critical.

## Frontend

The emulation core uses a **callback-based** interface. The core owns the master clock; the frontend is a passive consumer:

* `onFrameReady(buffer)`: PPU signals frame completion at V-blank
* `onAudioSample(left, right)`: APU emits samples at the exact cycle produced
* `pollInput() → buttons`: core polls controller state when needed

For headless/CI testing, stub callbacks capture output for assertions without requiring a display or audio device.

## Verification

Accuracy verification is layered:

* **Determinism**: run the same ROM twice from the same state, `memcmp` state blocks at every frame boundary
* **Framebuffer comparison**: run test ROMs headless, hash the framebuffer, compare against known-good values
* **Execution trace diffing**: log CPU bus cycles with timestamps, diff against reference traces from hardware or other emulators

Instrumentation hooks are built into the core from day one (conditional trace logging in the CPU micro-op stepper, compiled out in release builds). The full test suite is built incrementally as hardware knowledge grows.
