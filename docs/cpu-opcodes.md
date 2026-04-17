# 5A22 Opcode Authoring

This document describes how opcodes are authored in PupSNES today.

The important split is:

* **Authoring lives in** `src/pupsnes/5a22/cpu_opcodes.cpp`
* **Generic DSL/lowering helpers live in** `src/pupsnes/5a22/cpu_opcode_defs_internal.h`
* **Execution types and runtime behavior live in** `includes/pupsnes/hw/5a22/cpu.h` and `src/pupsnes/5a22/cpu.cpp`

If you are adding or changing an opcode, start in `cpu_opcodes.cpp`.

## Mental Model

PupSNES models the CPU as:

1. opcode fetch
2. a sequence of per-cycle micro-ops after that fetch

Each authored cycle slot has:

* one bus action
* one post-bus internal operation
* an optional timing rule guard
* an optional human-readable label

The authored specs are lowered at compile time into:

* a compact execution table used by `CPU::Tick()`
* a parallel metadata table for labels and future tooling

Unspecified opcodes default to explicit unimplemented faults rather than silent placeholder behavior.

## Where To Edit

### `src/pupsnes/5a22/cpu_opcodes.cpp`

This is the source-of-truth file for authored opcodes.

It contains:

* reusable local fragments such as `FetchLongAddr()` or `BranchSequence()`
* family-grouped authored opcode arrays:
  * `MakeMiscSpecs()`
  * `MakeLoadSpecs()`
  * `MakeStoreSpecs()`
  * `MakeBranchSpecs()`
* the final concatenated spec list: `kExplicitOpcodeSpecs`
* the compile-time validation and lowering call

Keep opcode authorship here, grouped by family rather than opcode byte order.

### `src/pupsnes/5a22/cpu_opcode_defs_internal.h`

This file is not where new opcodes should normally be written.

It exists to provide the generic mechanism:

* `Opcode(...)`
* `FetchPc(...)`
* `ReadAddr(...)`
* `WriteA8Addr(...)`
* `Internal(...)`
* `Always()`, `Condition(...)`, `Not(...)`, `AllOf(...)`, `AnyOf(...)`
* lowering helpers and compile-time validation

Edit this file only when the authoring language itself needs to grow.

## Authoring Pattern

The normal workflow is:

1. Decide which opcode family the instruction belongs to.
2. Add the authored spec to the corresponding `Make*Specs()` array in `cpu_opcodes.cpp`.
3. Reuse an existing local fragment if one matches the bus-cycle pattern.
4. If no fragment exists, add a small local fragment in `cpu_opcodes.cpp`.
5. Only add new DSL primitives to `cpu_opcode_defs_internal.h` if the current authoring language cannot express the instruction cleanly.

Example: immediate loads are authored directly in `MakeLoadSpecs()`:

```cpp
Opcode(0xA9, "LDA", "immediate")
    .Then(FetchPc(MicroInternalOp::kLoadALowUpdateNz, Always(),
                  "fetch immediate"))
    .Build(),

Opcode(0xA2, "LDX", "immediate")
    .Then(FetchPc(MicroInternalOp::kLoadXLowUpdateNz, Always(),
                  "fetch immediate"))
    .Build(),
```

Example: `STA long` reuses a local fragment:

```cpp
Opcode(0x8F, "STA", "absolute long")
    .Then(FetchLongAddr())
    .Then(WriteA8Addr(MicroInternalOp::kNone, Always(), "write A low"))
    .Build(),
```

Example: guarded branch timing reuses a shared branch fragment:

```cpp
Opcode(0xD0, "BNE", "relative")
    .Then(BranchSequence(MicroInternalOp::kSetBranchTakenIfNotZero))
    .Build(),
```

## Choosing The Right Layer

Use these rules to keep authorship readable:

* If the pattern is specific to a few opcodes in one family, put it in `cpu_opcodes.cpp` as a local fragment.
* If the pattern is a generic authoring primitive that many future opcodes will need, add it to `cpu_opcode_defs_internal.h`.
* If the instruction needs a new post-bus CPU behavior, add a new `MicroInternalOp` in `cpu.h` and implement it in `cpu.cpp`.
* If the instruction needs a new named timing fact, add a new `TimingCondition` and populate it through CPU execution state.

Avoid pushing ordinary opcode authorship into the internal header. The point of the system is to keep the authored opcode list easy to find and review.

## Bus Actions, Internal Ops, And Guards

### Bus actions

Bus actions describe what the CPU does on the bus during that cycle.

Current authored helpers include:

* `FetchPc(...)`: read `PBR:PC`, then increment `PC`
* `ReadAddr(...)`: read from resolved effective address
* `WriteA8Addr(...)`: write the low byte of `A` to resolved effective address
* `Internal(...)`: no bus access this cycle

Operand fetches from the instruction stream should stay explicit. The opcode fetch itself is implicit in the main CPU execution loop.

### Internal operations

Internal ops mutate CPU state after the bus action completes.

Examples:

* `kLoadALowUpdateNz`
* `kLoadXLowUpdateNz`
* `kSetAddrLowFromFetch`
* `kSetBranchTakenIfNotZero`
* `kBranchRelative8`

If a new instruction cannot be expressed with the existing internal ops, add one intentionally instead of hiding logic in ad hoc code.

### Guards

Guards are timing rules attached to cycle slots.

Stage 1 currently uses:

* `Always()`
* `Condition(TimingCondition::kBranchTaken)`

Rule expressions are declarative. They should describe architectural timing conditions, not arbitrary executor details.

## Labels And Metadata

Each authored cycle slot may carry a short label like:

* `"fetch immediate"`
* `"fetch address low"`
* `"apply branch"`

These labels are not for execution. They lower into the metadata table so traces and tooling can later describe what cycle the CPU is on.

Use short labels that describe the cycle's intent, not the implementation detail.

## Adding A New Opcode

When adding an opcode:

1. Add the authored definition in the correct family array in `cpu_opcodes.cpp`.
2. Reuse or add a local fragment if that improves readability.
3. Add any missing `MicroInternalOp` support in `cpu.h` and `cpu.cpp`.
4. Add or update structural lowering coverage in `src/tests/cpu_opcode_defs_tests.cpp`.
5. Add or update CPU execution coverage in `src/tests/cpu_tests.cpp`.

For straightforward instructions, the execution test should normally prove:

* register result
* flag result
* program counter result
* expected cycle count through `Tick()`

## Validation Rules

Opcode specs are validated at compile time before the final table is emitted.

Current validation covers:

* duplicate opcode assignment
* malformed timing rules
* oversized cycle sequences
* invalid rule-count lowering

If validation fails, the build should fail. This is intentional: malformed opcode specs should not reach runtime.

## Testing

Two test layers matter:

### Structural lowering tests

File: `src/tests/cpu_opcode_defs_tests.cpp`

These verify that authored specs lower into the intended runtime representation.

Use these when you want to prove:

* the chosen bus action is correct
* the chosen internal op is correct
* a guarded cycle lowers with the expected rule
* an authored family refactor did not change emitted table shape

### CPU behavior tests

File: `src/tests/cpu_tests.cpp`

These verify actual execution behavior through `CPU::Tick()` and scheduler integration.

Use these to prove:

* the instruction updates registers and flags correctly
* the instruction advances `PC` correctly
* the instruction consumes the expected number of cycles
* fault behavior or scheduling behavior remains correct

## Style Guidelines

Prefer these conventions:

* Group authored specs by instruction family, not opcode byte order.
* Keep one opcode entry visually compact when possible.
* Extract a local fragment only when it removes repeated cycle structure.
* Keep cycle labels short and concrete.
* Keep authorship in `cpu_opcodes.cpp`; keep mechanism in `cpu_opcode_defs_internal.h`.

If an opcode addition makes `cpu_opcodes.cpp` harder to scan, that is usually a sign to improve the local family fragment structure, not to move the authored specs elsewhere.
