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
2. **ScheduledComplete**: produces a token with a completion time, resolved later during the scheduler’s Commit phase. Used for cross-clock targets (e.g., APU ports `$2140`–`$2143`) where the clock domain boundary requires asynchronous resolution.
3. **Rejected**: indicates an invariant violation / invalid plan usage and is treated as a crash-level emulator error.

---

## MMIO vs memory-backed resolution

- **Memory-like regions** (ROM, WRAM, SRAM): always **InlineComplete**. These are passive memory with no side effects.
- **Same-clock MMIO** (PPU registers at `$2100`–`$213F`, CPU registers at `$4200`–`$44FF`): **InlineComplete** with catch-up. The target device is advanced to the current master cycle before the access, ensuring side effects are applied at the correct time.
- **Cross-clock MMIO** (APU ports at `$2140`–`$2143`): **ScheduledComplete**. The clock domain boundary requires asynchronous token resolution with time conversion.

## Catch-up on access

When the SystemBus resolves a same-clock MMIO transaction via InlineComplete, it triggers catch-up:

1. The issuing device (CPU or DMA) is in its Run phase, executing a bus access micro-op
2. The SystemBus decodes the address via the page table and identifies the target device
3. The SystemBus checks the target’s clock domain — same-clock targets use catch-up
4. The scheduler advances the target device by running its `tick()` until `local_time >= current master time`
5. The transaction is applied to the target’s state (register write commits, register read returns current value)
6. Control returns to the issuing device, which continues its micro-op sequence

Cascading catch-ups are not a concern on the SNES: the PPU (the primary same-clock MMIO target) never initiates system bus transactions during its internal advancement — it has its own dedicated VRAM bus.

---

## Scheduler integration

- The scheduler is the only system that advances global time.
- External effects become observable only at defined times (usually at scheduled completion during Commit), and devices sample during Wake, preventing early observation and time travel.
