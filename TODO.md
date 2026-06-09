# TODO

## Master-cycle timing — reference

Each CPU bus micro-op retires `BusPlan::access_cycles` master cycles (6/8/12 per page `access_speed`); internal micro-ops retire `kInternalCpuCycleMaster = 6`. Unmapped reads charge 8 master cycles. `TickToTarget` is strict-no-overshoot: if the next micro-op wouldn't fit, the CPU splits it into a partial-op cycle so `master_time` lands exactly on the target. Manual joypad area (`$00-$3F:$4000-$41FF` and the `$80-$BF` mirror, i.e. bus pages `$40-$41`) is billed at 12 master cycles per access — covers `$4016`/`$4017` plus the unmapped fill inside that span. The PPU/APU register block (`$2100-$21FF`, page `$21`) and the CPU/DMA register block (`$4200-$43FF`, pages `$42-$43`) are billed at 6 (the fast bus class).

## CPU — 65C816 instruction coverage

### Follow-ups

- **Perf validation.** No microbenchmark harness exists yet. When one is
  added (tight ADC/LDA/DEX/BNE loop ROM + `samply`), re-validate the
  micro-op refactor hasn't regressed IPC / L1i miss rate.

- **Stringifier enrichment.** `Format(MicroInternalOp)` currently returns
  just the category name. For richer debugger traces, extend to
  `Format(MicroInternalOp, uint8_t params)` so e.g. `kTransferReg` decodes
  as `"TransferReg(A->X)"`. Cold path; low priority.

- **`kExchangeCarryEmulation` review.** Left as its own variant (XCE is a
  one-off). If another E-related op ever lands, reconsider folding.

## Known tech-debt items (unrelated to the refactor)

- `src/tests/microop_trace_tests.cpp` has pre-existing
  `bugprone-unchecked-optional-access` clang-tidy warnings (non-blocking; see
  ci-verify output). Cleanup candidate.

- Unused-include hints (clangd `misc-include-cleaner`) in
  `src/tests/rom_integration_tests.cpp` (`<map>`, `cartridge.h`) — pre-existing,
  non-blocking.
