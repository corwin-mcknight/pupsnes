# Scheduler

This document specifies the emulator scheduler: how time advances, how devices execute, how external effects become observable, and how we guarantee determinism across runs and save-states.

The scheduler is the single authority for simulated time. Devices never “run ahead” of global time, and no device may observe an external effect earlier than the moment it becomes observable.

This design is intended for cycle-accurate / event-accurate emulation where bus contention, open bus behavior, DMA, IRQ sampling, MMIO timing, and other cross-device interactions must be modeled without “time travel” artifacts.

---

## Goals

1. **Single source of truth for time**
   - Only the scheduler advances global time.
   - Global time is measured in master clock cycles.
2. **No time travel**
   - No device can observe any external effect before it becomes observable.
   - No device can commit an external effect “in the past” relative to other devices.
3. **Determinism**
   - Given the same initial state, the same input stream, and the same configuration, the emulator produces identical results:
     - identical event order
     - identical internal transitions
     - identical outputs
   - Reloading a save-state resumes with the same event ordering and produces the same continuation behavior.
4. **Separation of concerns**
   - Devices do not directly call into other devices.
   - Devices interact only through connectors (wires, buses, latches, etc.) that obey scheduler semantics.
5. **Bounded execution**
   - Devices are given a budget (maximum safe time to run) that never crosses a hazard boundary.
   - Devices must not exceed their budget.
   - The scheduler detects and reports zero-time livelocks (bugs).

---

## Key definitions

### Master time (t)

Global time in integer master clock cycles. There is no fractional time. If a real phenomenon behaves “analog” or “sub-cycle,” it is approximated via discrete rules (e.g., open-bus decay after N cycles).

### Device

A simulated hardware unit (CPU, PPU, APU, DMA engine, cartridge, etc.) that:
- has internal state
- interacts with connectors
- is schedulable by the kernel

### External effect

Any effect that can be observed by another device or can change another device’s future behavior. Examples:
- bus transactions (address/data/control)
- MMIO reads/writes
- IRQ/NMI lines changing level/edge
- DMA stealing cycles or performing transfers
- latches, edge-triggered behavior
- any connector that crosses device boundaries

### Internal operation

Any computation that is not externally visible until a later external effect is committed. Internal operations may run without preemption within the provided budget.

### Hazard boundary

A time boundary past which a device may not run without returning to the scheduler, because something external could change. Common hazards:
- the next scheduled event time
- device-local timing boundary (e.g., PPU dot boundary, timer tick boundary)
- completion of a pending bus request
- an external sampling point (e.g., IRQ sample edge)

---

## Core invariants

1. Only the scheduler advances time.
2. No device observes an external effect “early.”
3. Every external read/write is represented by a token. Same-clock tokens resolve synchronously via catch-up on access. Cross-clock tokens resolve asynchronously at a defined observability time.
4. Same-timestamp determinism requires stable tie-breakers.
5. Devices cannot interact directly; all cross-device behavior must go through the SystemBus (bus transactions), the signal region of the state block (control signals), or scheduled events.
6. A device may not execute past its granted budget.
7. The system must not livelock at the same timestamp (zero-time infinite work).

---

## High-level model: discrete-event kernel

The emulator runs as a discrete-event simulation:
- The scheduler maintains an event queue keyed by time and ordering rules.
- The scheduler pops the next event(s), advances now to that event’s time, performs commit/wake work, and runs devices within safe budgets.
- Devices yield back to the scheduler when:
  - they exhaust budget,
  - they must wait on an event token,
  - they reach a device-local boundary,
  - they must emit an external effect that requires scheduling.

This approach avoids “tick everything every cycle” while preserving cycle-level correctness where it matters.

---

## Event ordering and determinism

All events are totally ordered using a deterministic key:

```
(time,
 subphase,
 class_priority,
 device_priority,
 stable_seq)
```

**Field meanings**
- `time`: master cycle timestamp when the event is processed / becomes observable
- `subphase`: a strict ordering partition inside the same time (see Phases below)
- `class_priority`: stable ordering between broad event categories (should be used sparingly)
- `device_priority`: stable ordering within a class for specific devices (e.g., fixed ID)
- `stable_seq`: monotonic sequence number assigned at event creation (stable across save/load)

**Determinism requirements**
- Every device has a stable numeric ID (not pointer-based).
- Every event token has a stable ID (not pointer-based).
- `stable_seq` is stored in save-states.
- The event queue contents and ordering state are serialized in save-states.

---

## Phases inside a timestamp

Within a single master cycle timestamp `T`, work is processed in explicit phases to prevent early observation and to produce a consistent ordering of effects.

### Phase 1: Commit / Complete

Purpose: make external effects observable.

This phase applies all scheduled “becomes observable at time T” effects:
- complete bus transactions (read data becomes valid, write commits)
- release bus locks
- commit staged connector writes
- resolve event tokens and wake blockers

Rules:
- Commit must run before any device is allowed to sample the new external state.
- If completing one event causes new same-time commits to be generated, they must also be committed before sampling.

### Phase 2: Wake / Sample

Purpose: devices observe external state at well-defined sampling points.

This phase allows devices to:
- sample shared lines (IRQ/NMI, status lines, etc.)
- detect edges (if modeled)
- update latched “input view” state derived from committed connectors

Rules:
- Sampling happens only after commit has quiesced.
- Sampling does not consume time.
- If sampling results in the creation of same-time completion work (rare but possible in some models), it must be committed before sampling continues.

### Quiescence rule (Phase 1 & 2 loop)

Phases 1 and 2 repeat until no new same-time work is created:

1. Commit/Complete
2. Wake/Sample

(repeat) until stable

This ensures that within timestamp `T`, all “observable at T” effects settle before devices proceed.

### Phase 3: Run

Purpose: devices consume time, executing internal operations and issuing external requests.

During Run:
- devices may execute internal logic up to their granted budget
- devices may queue staged connector writes (not immediately visible)
- devices may request external actions (bus reads/writes), which produce event tokens

Rules:
- Run is the only phase where time can be consumed.
- Devices must not cross hazards within a single run slice.

---

## Device interface: ISchedulable

Every device that can be scheduled implements the schedulable interface:

```
tick(budget)
```

The scheduler calls `tick(budget)` where:
- `budget` is the maximum number of master cycles the device may safely consume
- `budget` is chosen so the device cannot cross a hazard boundary

`tick(budget)` returns:
1. `consumed_cycles`: integer in `[0, budget]`
2. wait token / event (optional): if the device is blocked waiting for an external event
3. stop reason + metadata: why execution ended and what the scheduler should do next

Device status is updated accordingly:
- running (wants reschedule at a time)
- blocked (waiting for token)
- idle (no work until a future time or external wake)
- error (contract violation, etc.)

**Stop reasons (typical)**
- `BudgetExhausted`: used entire budget; scheduler may reschedule later
- `ReachedLocalBoundary`: hit device-internal boundary; scheduler schedules at that boundary
- `BlockedOnToken(token_id)`: device must sleep until token resolves
- `IssuedExternalRequest(token_id, observable_time)`: device requested bus/MMIO action and must block or yield
- `NoWork`: device has nothing to do until something changes

---

## Budget calculation

A device budget is always the maximum “safe” amount of time to execute without missing hazards:

```
budget = min(
  next_event_time - now,
  device_local_boundary_time - now
)
```

- `next_event_time` is the time of the earliest event in the global queue that could affect external state.
- `device_local_boundary_time` is the device’s next internal boundary (if applicable), e.g.:
  - PPU dot / H/V boundary
  - timer tick edge
  - APU DSP frame boundary
  - scheduled self-wake time

Rules:
- If `budget == 0`, the device must not be run in Phase 3; instead the scheduler should process same-time commit/wake work or advance time to the next event.
- Devices are not allowed to consume time without being scheduled.
- Devices must never consume more cycles than granted.

---

## External reads/writes and tokens

### Token model

All external I/O is represented by tokens. A token represents a bus transaction (read or write) with a stable ID, type, and payload/result slot.

Token resolution semantics depend on the clock domain of the target:

**Same-clock tokens** (e.g., CPU → PPU register write):
1. Device issues a bus transaction via the SystemBus
2. SystemBus decodes the address and identifies the target device
3. Target device is caught up to the current master cycle (scheduler runs target's `tick()` until its local time matches)
4. Transaction is applied to the target's state
5. Token resolves synchronously — the issuing device continues immediately

**Cross-clock tokens** (e.g., CPU → APU port write at `$2140`–`$2143`):
1. Device issues a bus transaction via the SystemBus
2. SystemBus identifies a cross-clock target
3. Token is queued with a completion time derived from the clock domain conversion
4. At completion time, the target is caught up to the equivalent time in its own clock domain
5. Transaction is applied, token resolves, and blockers are woken

### Tokens

Tokens have:
- stable token ID (not pointer-based)
- type (bus read, bus write, DMA step, etc.)
- target clock domain (same-clock or cross-clock)
- payload / result slot (for read data, etc.)
- for cross-clock: scheduled completion time

Tokens must be serializable and deterministically resolved.

### Blocking rule

If a device depends on the completion of a cross-clock external action, it must:
- return `BlockedOnToken(token_id)` and give up remaining budget

Same-clock tokens resolve synchronously and never require blocking.

---

## Device isolation and signals

Devices cannot directly access each other's state. Cross-device interaction happens through two mechanisms:

### Bus transactions (SystemBus)

All addressed read/write operations go through the SystemBus, which handles address decoding via the page table, clock domain detection, and catch-up synchronization. See `docs/systembus.md`.

### Control signals (Signal Region)

Control signals (NMI, IRQ, HALT) are modeled as flat fields in the signal region of the state block:

- **Edge-triggered signals** (NMI): two fields — `current_level` and `previous_level`. The CPU detects a rising edge by checking `current && !previous` at its interrupt sampling micro-op step. After sampling, `previous = current`.
- **Level-triggered signals** (IRQ, HALT): one field — `current_level`. The CPU checks the level directly.

Devices write signal fields during their execution (including during catch-up). The CPU reads them at defined sampling points within its micro-op table.

### No direct references

Devices are constructed with:
- their own internal state (residing in the state block)
- stable IDs
- no direct pointers to other devices

This enforces modularity and prevents accidental time travel via direct calls.

---

## Same-timestamp work and quiescence

Some effects happen at the same master cycle and must be processed without advancing time.

**Correctness requirement**

At time `T`:
- all completions that become observable at `T` must commit before sampling at `T`
- all sampling at `T` must occur before any device consumes time beyond `T`

**Quiescence guardrails**

Because Phase 1 and 2 can repeat at the same time, the scheduler must prevent livelock:
- max commit/wake iterations per timestamp
- if exceeded:
  - break execution
  - dump debug state (events at time, connectors changed, devices involved)
  - treat as a bug in modeling (combinational oscillation, improper scheduling, etc.)

---

## Zero-time livelock detection

Devices must not perform infinite work that consumes zero time.

Rules:
- A device may return `consumed_cycles == 0` only if:
  - it blocked on a token, or
  - it is purely reacting in same-time commit/wake, or
  - it legitimately has no work
- Repeated zero-time runs in Phase 3 are a bug.

**Global guardrail**
- The emulator enforces an upper bound on “events processed per frame” or “same-timestamp iterations.”
- If exceeded, the scheduler stops and emits diagnostics.

**Diagnostics should include**
- current time
- last N events processed
- device stop reasons
- pending tokens
- staged connector writes
- stable ordering keys for relevant events

---

## Save-state requirements

All emulator state resides in a single contiguous **state block**. Save state = `memcpy` of the block. Load state = `memcpy` back. This guarantees nothing is omitted.

The state block contains:

1. **Global scheduler state**: current time, event queue (fixed-size array with known upper bound), `stable_seq` counter, phase/subphase state
2. **Token table**: all outstanding tokens (fixed-size, bounded by max outstanding tokens), blocked device references by token ID
3. **Signal region**: NMI (current + previous level), IRQ level, HALT level
4. **All device state**: registers, internal counters, continuation state (micro-op index, pipeline stage), run status, local boundary times
5. **Memory arrays**: WRAM (128KB), VRAM (64KB), APU RAM (64KB), OAM (544B), CGRAM (512B)

Device code accesses state through typed POD struct overlays at known offsets in the block, providing natural field access (`state->A`, `state->scanline`) with zero overhead.

Rewind uses a ring buffer of state block snapshots. Determinism verification uses `memcmp` of two blocks run from identical initial state.

---

## Recommended event classes

To keep `class_priority` minimal and semantics-driven, use classes like:
- `CommitComplete`: applies external observability changes
- `WakeSample`: allows devices to sample
- `RunSlice`: schedules device execution
- `Boundary`: device-local timed boundaries (dot, timer, etc.)
- `TokenComplete`: token completion events (often part of `CommitComplete`)

The primary ordering should come from time + subphase. Avoid SNES-specific “CPU always before PPU” global rules unless you have a hard hardware reason; prefer to fix modeling at connector/token level instead.

---

## Practical device modeling guidance

### CPU core

Table-driven micro-ops: each instruction is a table of per-cycle bus actions, matching the WDC 65C816 datasheet cycle-by-cycle timing. One table entry per bus cycle — each entry specifies address source, read/write direction, and internal operation.

- DMA can steal the bus between any two micro-op entries
- IRQ/NMI are sampled at specific micro-op steps (not every cycle)
- Same-clock bus accesses (e.g., reads from WRAM, writes to PPU registers) resolve synchronously via catch-up
- Continuation state is the micro-op table index and pending operand — no “preempt mid-C++ stack”

### PPU

Dot-accurate from day one. The PPU advances one dot (pixel clock) at a time, with explicit modeling of:
- dot boundaries and H/V counters
- fetch phases (tile, sprite, attribute)
- output latching points
- mode register effects at the exact dot they apply

The PPU is a bus target — it never initiates system bus transactions (it has its own VRAM bus). When a CPU or DMA write targets a PPU register, the PPU is caught up to the current master cycle before the write is applied. This guarantees mid-scanline register writes take effect at the correct dot.

### DMA

DMA is inherently external:
- it should schedule transfers as tokens or boundary events
- it may affect bus lock availability and CPU wait states

### Open bus / decay

Open bus is modeled on connectors:
- retain last-driven state per line/bit
- track last-refresh time
- decay after configured duration
- resolution happens during commit (or via scheduled decay events), but must never be “observed early.”

---

## Debugging and traceability

The scheduler should be able to produce a trace that answers:
- Why did device X stop?
- What token is it waiting on?
- What became observable at time `T`?
- What was the event ordering key at time `T`?
- Which staged connector writes were committed?

**Recommended debug features**
- event log with ordering keys
- token resolution log
- per-device stop reason ring buffer
- connector commit history (recent changes)
- deterministic replay hooks (input log + save-states)

---

## Example timeline (conceptual)

At time `T`:

1. Commit completes a pending bus read token -> data becomes valid
2. Wake allows CPU to sample “read complete” and update its input view
3. Run gives CPU budget until next hazard; CPU consumes N cycles, issues next bus request token, blocks
4. Scheduler advances time to the next event

At no point does the CPU observe the read result before the token completion at `T`.

---

## Contract violations (bugs)

The following are considered bugs and should assert/fail loudly in debug builds:
- Device consumes more than its budget
- Device performs an external read/write without producing a token/event
- Device directly mutates another device’s state
- Device repeatedly yields zero cycles in Run phase without blocking
- Commit/wake loop fails to quiesce at a timestamp (oscillation)
- Event IDs or ordering keys are pointer-derived / non-stable
- Save-state omits required scheduler/token/connector state

---

## Summary

This scheduler is designed to:
- enforce strict time ownership (scheduler only)
- prevent early observation via explicit external effect tokens
- provide a deterministic total order for same-time work
- isolate devices behind connectors
- remain testable and reproducible across save/load

The result is a robust foundation for high-accuracy behavior (bus contention, decay/open bus, DMA interactions) without relying on fragile “call order discipline” or allowing time-travel artifacts.

---

## Implementation status

The following parts of this specification are implemented:

- Event queue with `(time, subphase, type, seq)` ordering (simplified from the full 5-tuple; `class_priority` and `device_priority` are deferred until needed)
- `Scheduler::step()`: pops next event, advances global time, dispatches to `Device::tick()` or `Device::onEvent()` based on subphase
- `Scheduler::computeBudget()`: `min(MAX_CYCLES_STEP, next_event_time - now)`
- `Device` base class with `tick(budget)` returning `TickResult` and `onEvent(event)`
- `TickStopReason`: `BudgetExhausted`, `BlockedOnIO`, `BlockedOnToken`
- `TickResult` includes `blocked_token` field for token-based blocking
- Three scheduler phases: CommitComplete, WakeSample, Run
- Stable device IDs (`device_id_t`) with SNES device registry (`registerDevice`/`getDevice`)
- `TokenTable` with create, complete, resolve, remove, and blocked-device tracking
- Scheduler integration: `createToken()`, `getToken()`, `removeToken()`
- Token resolution during CommitComplete with auto-wake for blocked devices
- Clock-driven (polling) wake model: devices can poll token state via `getToken()`

Not yet implemented:

- Connectors and device isolation
- Quiescence loop (commit/wake repeat until stable)
- Zero-time livelock detection
- Save-state serialization
- Device-local boundary time in budget calculation
