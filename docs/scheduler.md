# Scheduler

PupSNES uses a **signal-horizon scheduler**: a priority queue of signal events
keyed by absolute `master_time`. The CPU is the sole master-clock driver;
passive devices catch up to master time on demand.

---

## Types

### `SignalEvent`

```cpp
struct SignalEvent {
  TimeMasterT      master_time;  // absolute master cycle when event fires
  SignalKind       kind;
  SignalEventHandler handler;    // may be empty for pure timing marks
  uint64_t         seq;          // stable secondary ordering key
};
```

### `SignalKind`

Explicit-value enum (`uint16_t`) so existing values are stable across releases:

| Value | Constant | Purpose |
|-------|----------|---------|
| 0 | `kFrameEnd` | PPU frame wrap; swap buffers, fire frame callback |
| 1 | `kVblankNmiBoundary` | Start of vblank; check NMI enable and raise CPU NMI |
| 2 | `kHIrqMatch` | H/V timer match; check IRQ enable and raise CPU IRQ |
| 16 | `kApuSampleDeadline` | Reserved — not fired in v1 |
| 32 | `kDmaBurstComplete` | Reserved — not fired in v1 |
| 33 | `kHdmaFire` | HDMA frame initialization and per-scanline transfers |

### `SignalEventHandler`

```cpp
using SignalEventHandler = std::function<void(TimeMasterT master_time)>;
```

Invoked when the scheduler fires the event. At that point `MachineSync` has
already advanced every registered device to `master_time`, so handlers see a
fully-synced machine.

### `SignalEventView`

```cpp
struct SignalEventView { TimeMasterT master_time; SignalKind kind; };
```

Read-only snapshot row — used by the debugger to display the pending event
queue without exposing the handler.

---

## Public API

```cpp
class Scheduler {
 public:
  explicit Scheduler(SNES& snes);
  void Reset();

  // --- Signal-event API ---

  // Enqueue a one-shot event at master_time. Events with the same master_time
  // are ordered by SignalKind value, then by monotonic seq.
  void ScheduleSignal(TimeMasterT master_time, SignalKind kind,
                      SignalEventHandler handler);

  // Return the master_time of the earliest pending event, or
  // std::numeric_limits<TimeMasterT>::max() if the queue is empty.
  [[nodiscard]] TimeMasterT NextEventMasterTime() const;

  // Fire every event whose master_time <= through_time, in order.
  // MachineSync must have been called for through_time before this.
  void FireEventsThrough(TimeMasterT through_time);

  // True if the signal queue is non-empty.
  [[nodiscard]] bool HasPendingEvents() const;

  // Return a snapshot of (master_time, kind) pairs in queue order (earliest
  // first). Cheap enough for debugger polling; allocates a temporary vector.
  [[nodiscard]] std::vector<SignalEventView> SnapshotSignalQueue() const;

  // --- Token API (same-clock bus back-pressure) ---

  TokenIdT CreateToken(const TokenCreateParams& params);
  [[nodiscard]] const Token* GetToken(TokenIdT id) const;
  void RemoveToken(TokenIdT id);
};
```

---

## The RunControl loop

Every `TickFrame` iteration:

```
target = scheduler.NextEventMasterTime()       // event horizon (or max() if empty)
result = cpu.TickToTarget(target)              // CPU runs; writes master_time as work retires
now    = snes.GetMasterTime()                  // actual time reached (may be < target)
snes.MachineSync(now)                          // CatchUpTo(now) on every registered passive device
scheduler.FireEventsThrough(now)               // drain events at or before now, fully-synced
```

Steps in detail:

1. **`target = scheduler.NextEventMasterTime()`** — event horizon. If the queue
   is empty, `target` is `max()` and the CPU runs until a debugger stop.
2. **`result = cpu.TickToTarget(target)`** — the CPU runs micro-op by micro-op,
   calling `snes_->SetMasterTime(t)` as each bus access retires. It stops early
   at a breakpoint (`kBreakpoint`), a debugger-step retire (`kRetiredStepTarget`),
   a fault (`kFault`), or when `master_time >= target` (`kReachedTarget`).
   Strict no-overshoot: the CPU retains partial-cycle progress when a micro-op
   would exceed the target. Its bus action and register changes complete once
   the remaining cycles have elapsed in a later call.
3. **`snes.MachineSync(now)`** — calls `Device::CatchUpTo(now)` on every
   registered passive device. This advances the PPU, APU, and other passive
   devices to `now` before handlers fire.
4. **`scheduler.FireEventsThrough(now)`** — drains all events whose
   `master_time <= now` in priority order. Each handler runs against a
   fully-synced machine.

---

## Device roles

### `MasterClockDriver : public Device`

Adds a pure-virtual method:

```cpp
virtual TickResult TickToTarget(TimeMasterT target_master_time) = 0;
```

The driver writes `snes_->SetMasterTime(t)` directly as work retires; it does
not override `CatchUpTo` (the default no-op is correct — the driver is never a
catch-up target). Currently only `CPU` implements this role; it also consumes
the bus-stall time requested by DMA/HDMA. Separate DMA or coprocessor clock
drivers remain possible future extensions.

### `Device` — passive (catch-up) devices

Passive devices override `CatchUpTo(TimeMasterT target)` to advance internal
state to `target`. The default implementation is a no-op, which is correct for
stateless bus responders.

Current overrides:
- **PPU** (`Ppu`): advances dot emission and the drawn-mask bitmap to `target`.
  Schedules `kFrameEnd` at each frame wrap; the handler reschedules the next.
- **APU** (`Apu`): advances its SPC700 one cycle at a time using an integer rational conversion with retained fractional phase. Port accesses also catch it up before accessing their latches. Timers and the DSP pipeline advance at each SPC edge before its bus access. The DSP's register and ARAM effects retain their individual phases; one native stereo sample is delivered through the SNES callback after every 32 SPC cycles. This internal cadence needs no scheduler sample event. The host audio callback consumes a queue without advancing emulated time. See [APU and sound synthesis](apu.md).
- Stateless devices: default no-op.

`CpuMmio` remains a `Device` subclass for bus-page dispatch and keeps the
default no-op `CatchUpTo`. Its interrupt boundaries use scheduled signals;
reads consult the relevant device state at the access timestamp.

---

## Properties

- **No same-time deadlock**: the scheduler never dispatches mid-handler; signal
  handlers cannot re-yield or block each other.
- **Single-master-cycle precision**: signals fire at the exact `master_time`
  they were scheduled for.
- **Partial-frame debugger rendering**: the PPU's drawn-mask bitmap records
  which pixels have been emitted in the current frame. The debugger overlays
  the in-progress frame on the last completed frame while paused.

---

## Out of scope (v1)

- **Audio sample deadlines and DMA completion events**: `SignalKind` placeholders (`kApuSampleDeadline`,
  `kDmaBurstComplete`) exist; handlers are not wired. Current audio synthesis and
  delivery run inside APU catch-up and do not depend on the unused sample-deadline event.
- **Coprocessor speculation (GSU, SA-1, DSP-n)**: each will implement
  `CatchUpTo` by running its own program on a worker thread and surfacing
  signals via `ScheduleSignal`. No scheduler surgery required.
- **Threading**: when coprocessor workers land, they feed the scheduler via a
  lock-free queue; the public `Scheduler` API is unchanged.
- **Rollback / checkpoint**: deferred until a concrete need emerges.
