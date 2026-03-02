# Architecture

PupSNES simulates the SNES as real hardware: independent devices, explicit signals, and edge-driven time, with correctness and determinism as first-class goals. We avoid proprietary SNES documentation; the goal is to describe behavior and scheduling, not reproduce copyrighted manuals.

Devices are clocked at a certain rate based on the clock they are connected to, and all devices are clocked at the same rates as they are on the SNES. Time is measured in integer master clock cycles and integer APU clock cycles.

Each device is modeled separately, and has its inputs, its outputs, and internal state modeled. Devices are emulated with C++ code and are not actually composed of wires.

Each device implements `Device::tick(budget)` to advance internal state by up to `budget` master clock cycles, returning a `TickResult` with the cycles consumed and a stop reason. Devices also implement `Device::on_event(event)` for non-Run scheduler events (CommitComplete, WakeSample).

See `docs/scheduler.md` for the full scheduler design specification.

## Object Model

PupSNES has a dynamic topology of interconnected objects that represent the SNES hardware. The main components are:

* **SNES**: The top-level object that contains all other components. Owns global master and APU time, and the Scheduler instance.
* **Scheduler**: Part of the SNES. Maintains an event queue ordered by `(time, subphase, type, seq)`. Dispatches events to devices via `tick()` (Run phase) or `on_event()` (CommitComplete/WakeSample phases). Computes per-device budgets capped by `MAX_CYCLES_STEP` and the next event time.
* **Device**: Base class for all hardware units (CPU, PPU, APU, etc.). Each device has internal state, a reference to the SNES, and implements `tick(budget)` and `on_event(event)`.
* **SystemBus**: Authority for CPU-visible bus transactions. Stub interface exists; Plan/Follow model described in `docs/systembus.md`. (planned)
* **Wires and Lanes**: Connections between devices. Wires can have multiple lanes for multi-bit buses. (planned)
* **Buses**: Collections of wires that connect multiple devices together, and arbitrate access when multiple devices drive the same bus. (planned)
* **Port**: Allows connecting external peripherals to the SNES, such as controllers or cartridges. (planned)

All devices will have a stable numeric id for deterministic ordering and save-state support. (planned)
