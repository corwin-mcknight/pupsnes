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

## Address decoding

Address decoding uses a flat page table: a 256×256 array indexed by bank and page (256-byte granularity). Each entry contains a target device reference and a device-relative offset. The table is precalculated at ROM load time based on the cartridge mapper type (LoROM, HiROM, etc.).

- Mirrors are free — multiple entries point to the same backing memory/handler
- I/O register ranges (`$2100`–`$44FF`) are flagged as device handlers (not raw memory)
- Co-processor cartridges overlay their regions by modifying table entries
- This is the hottest path in the emulator — O(1) lookup, no branching

**BusPlan** carries the resolved target device and device-relative offset (not a raw address), so mapping changes after planning don’t affect what the plan refers to.

---

## Plan outcomes

Three possible plan outcomes, determined by the target’s clock domain and type:

1. **InlineComplete**: resolves the transaction synchronously. Used for:
   - Memory-backed regions (ROM, WRAM) where no side effects or hazards exist
   - Same-clock MMIO targets (e.g., PPU registers): the target device is **caught up** to the current master cycle via the scheduler before the transaction is applied, ensuring the target’s internal state is consistent at the exact cycle of access
2. **ScheduledComplete**: produces a token with a completion time, resolved later during the scheduler’s Commit phase. Available for targets that require deferred completion; the current APU port implementation does not use this path.
3. **Rejected**: indicates an invariant violation / invalid plan usage and is treated as a crash-level emulator error.

---

## MMIO vs memory-backed resolution

- **Memory-like regions** (ROM, WRAM, SRAM): always **InlineComplete**. These are passive memory with no side effects.
- **Same-clock MMIO** (PPU registers at `$2100`–`$213F`, CPU registers at `$4200`–`$44FF`): **InlineComplete** with lazy-replay. Reads trigger catch-up so the target samples current state; writes queue into the target's pending-write log without catch-up overhead.
- **APU ports** (`$2140`–`$2143`, mirrored through `$217F`): **InlineComplete**. Page `$21` still routes through the PPU handler, which forwards these accesses immediately. The APU catches up its independent clock before both reads and writes, then accesses separate directional latches. Its private bus cannot block the main CPU bus, so no scheduled token is required. See [APU bring-up](apu.md).

## Lazy-replay catch-up

For PPU registers, the contract is **writes queue, reads catch up**. APU port writes instead catch up synchronously in their own handler before changing the input latch. `SystemBus::FollowInline` gates its catch-up call on access type:

### Read path

1. The issuing device (CPU or DMA) is in its Run phase, executing a bus access micro-op.
2. The SystemBus decodes the address via the page table and identifies the target device.
3. For same-clock MMIO reads, the scheduler advances the target by running its `tick()` until `local_time >= current_time`.
4. `ReadRegister(offset, current_time) → MmioReadResult { value, driven_mask }` runs. The target also drains any pending-write-log entries with cycle `≤ current_time` inside the read handler, defending against the PPU's per-dot drain stopping slightly short.
5. The bus merges open-bus: `data = (value & driven_mask) | (last_data_bus_value_ & ~driven_mask)`, and the merged byte becomes the new latched value.
6. Control returns to the issuing device.

### Write path

1. Same as steps 1–2 above.
2. No catch-up. The bus calls `WriteRegister(offset, data, current_time)` on the target; the target appends `{cycle=current_time, offset, data}` to its pending-write log.
3. The target's next `tick()` replays the log in order as it advances time, interleaving replay with per-device work (dot emission for the PPU).
4. Control returns immediately.

Enqueues are monotonic in cycle because the CPU's master time advances monotonically. Overflow of a fixed-size log (e.g., the PPU's 16384 entries) triggers a synchronous soft-limit flush: the device drains up to the new entry's cycle before appending, so no write is lost.

### Device API signatures

Every `Device` exposes:

```cpp
struct MmioReadResult { uint8_t value; uint8_t driven_mask; };
virtual MmioReadResult ReadRegister(uint32_t offset, TimeMasterT current_time);
virtual void WriteRegister(uint32_t offset, uint8_t data, TimeMasterT current_time);
```

`driven_mask` is `0xFF` for fully-driven bits, `0x00` for pure open-bus registers (reads return the bus latch), or partial (e.g., CGRAM high-byte reads drive only bits 6:0). Storage-only devices (WRAM, Cartridge ROM) always drive `0xFF`.

`HandleDebugRead` / `HandleDebugWrite` are out-of-band debugger-facing accessors; they do not participate in the lazy-replay contract and do not receive `current_time`.

Cascading catch-ups are not a concern on the SNES: the PPU (the primary same-clock MMIO target) never initiates system bus transactions during its internal advancement — it has its own dedicated VRAM bus.

---

## Scheduler integration

- The scheduler is the only system that advances global time.
- External effects become observable only at defined times (usually at scheduled completion during Commit), and devices sample during Wake, preventing early observation and time travel.
