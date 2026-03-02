# System Bus

The System Bus is the authority for transactions. It owns address resolution (SNES mapping rules), arbitration/locking, and timing for CPU-visible accesses. Smaller internal buses can exist behind devices, but anything that crosses devices goes through the System Bus rules.

---

## Core rules

- All reads/writes go through a two-step contract: **Plan → Follow**.
- A device requests an access and receives a **BusPlan** (a POD “transaction descriptor”).
- The device must immediately follow the plan in the same run slice. Plans are not storable or cancellable. A plan that isn’t followed right away is invalid and treated as a hard emulator error.
- **BusPlan** is pure; **Follow** is where state changes.
- Creating a **BusPlan** has no side effects and does not advance time.
- Following a plan is the act that locks/reserves the bus and either schedules a completion or completes inline, depending on safety.
- **BusPlans** are time-aware and deterministic.

---

## Time model

- Each device maintains an absolute-time cursor (`master_clock_t`) that starts at the scheduler’s current time when the device begins running and advances as the device consumes cycles.
- Bus planning and locking use the device’s cursor time (same global timeline), enabling multiple accesses per slice without “time offsets.”

---

## Resolved targets

- **BusPlan** carries resolved target info.
- Plans include the resolved target device and a device-relative offset (not a raw address), so mapping changes later don’t affect what the plan refers to.

---

## Plan outcomes

Three possible plan outcomes:

1. **InlineComplete**: returns data/ack immediately only if it’s provably safe (no hazards violated, completion within the slice, no external observability issues).
2. **ScheduledComplete**: produces a token/completion-time to be resolved later during the scheduler’s Commit phase (before devices sample and before further run work).
3. **Rejected**: indicates an invariant violation / invalid plan usage and is treated as a crash-level emulator error.

---

## MMIO vs memory-backed optimization

- Memory-like regions (ROM/WRAM/etc.) can use **InlineComplete** when the bus can prove they can’t change within the slice.
- MMIO-backed accesses are treated as observability hazards and normally go through **ScheduledComplete** (with strict timing and side-effects handled at completion).

---

## Scheduler integration

- The scheduler is the only system that advances global time.
- External effects become observable only at defined times (usually at scheduled completion during Commit), and devices sample during Wake, preventing early observation and time travel.
