# APU bring-up

PupSNES now implements **all 256 SPC700 opcodes**. The processor runs the real 64-byte IPL program, receives bytes into its own 64 KiB RAM, and executes uploaded programs, including arithmetic, logical operations, branches, subroutines, and software interrupts. Timers and DSP synthesis are still missing, so there is no audio output yet. Complete opcode coverage does not mean complete S-SMP hardware or timing accuracy.

## Data-transfer coverage

The data-transfer family includes all 41 byte `MOV` forms, both `MOVW` forms, both `MOV1` bit transfers, and the eight `PUSH`/`POP` forms.

| Family | Implemented behavior |
| --- | --- |
| Register and immediate moves | A, X, Y, and stack-pointer transfers; immediate loads into A, X, and Y |
| Direct-page moves | Plain and indexed loads/stores, immediate-to-memory and memory-to-memory copies |
| Absolute moves | A/X/Y loads and stores; A loads/stores indexed by X or Y |
| Indirect moves | `(X)`, `(X)+`, `[dp+X]`, and `[dp]+Y` addressing |
| Word moves | `MOVW YA,dp` and `MOVW dp,YA` |
| Bit moves | Carry to/from one bit in a 13-bit memory address |
| Stack transfers | Push/pop A, X, Y, and PSW through page `$01`, with 8-bit stack-pointer wrapping |

Byte loads and register transfers update N/Z except for `MOV SP,X`. Stores and memory-to-memory moves preserve flags. Register pops preserve flags; popping PSW restores the saved byte. `MOV1` changes only carry or the selected memory bit. Word loads derive N/Z from the full 16-bit result.

Direct-page indexing and pointer fetches wrap inside the selected page. Absolute indexed addresses wrap at 16 bits. Store forms retain their required dummy reads, while `(X)+` stores and memory-to-memory moves follow their distinct access sequences. `(X)+` loads update A/X during the read cycle and N/Z on the following idle cycle. These boundaries remain observable when execution pauses inside an instruction.

## Comparison and arithmetic coverage

All 18 byte `CMP` forms, all 12 `ADC` forms, all 12 `SBC` forms, and all 12 byte `INC`/`DEC` forms are implemented. Word operations include `CMPW`, `ADDW`, `SUBW`, `INCW`, and `DECW`. `MUL`, `DIV`, `DAA`, and `DAS` complete this arithmetic step.

Comparisons preserve their operands and update N/Z/C without writing memory. Addition and subtraction update N/Z/C/H/V, while increment and decrement change only N/Z. Word addition and subtraction ignore incoming carry and report half-carry across bit 11. Word increments and decrements write the low byte before reading and writing the high byte, wrapping within the selected direct page.

Multiplication sets N/Z from the product's high byte. Division sets N/Z from the quotient and models overflow and zero-divisor behavior without a host divide-by-zero exception. Decimal adjustment uses the preceding carry and half-carry flags. Each operation retains its read, write, and idle cycles, including the final idle cycle after memory comparisons.

## Logical operations and control flow

`OR`, `AND`, and `EOR` support all twelve addressing forms. Shifts and rotates operate on A, direct-page memory, indexed direct-page memory, and absolute memory. Bit operations include every `SET1`/`CLR1` bit position, carry/bit combinations, `NOT1`, and `TSET1`/`TCLR1`. `XCN` exchanges A's nibbles. Flag instructions preserve unrelated PSW bits.

All conditional branches, bit branches, `CBNE`, and `DBNZ` forms are implemented, including their extra cycles when taken and wrapping relative addresses. Subroutines support `CALL`, `PCALL`, all sixteen `TCALL` vectors, and `RET`. `BRK` pushes the return PC and old PSW before setting B and clearing I; `RETI` restores PSW and PC through the wrapping page-$01 stack.

`SLEEP` and `STOP` retain distinct halted states. Both perform a PC read and idle cycle repeatedly without fetching another instruction. CPU port writes do not wake them; reset clears both states. No external interrupt/wake source is connected in the current S-SMP model. This halt behavior and the I flag do not imply a general external interrupt controller.

## Execution and synchronization

`SNES` owns an `Apu` passive device. Its `Spc700` core advances **one SPC cycle per call**, preserving operands and instruction phase between calls. Opcode fetches, operand reads, dummy reads, and writes occur on their respective cycles. A short scheduler slice never executes a future store early.

The current NTSC timing policy uses a nominal **1,024,000 Hz** SPC clock and a `236250000/11 Hz` master clock, giving the exact reduced ratio `5632/118125`. `CatchUpTo` retains the integer remainder across calls. Each SPC cycle is observed at the first integer master timestamp at or after its edge. This is a nominal oscillator choice, not a measurement of a particular console, and PAL conversion is not implemented.

Both CPU reads and writes of `$2140`–`$2143` (mirrored through `$217F` in banks `$00`–`$3F` and `$80`–`$BF`) first catch up the APU. Reads sample SPC-to-CPU output latches; writes update separate CPU-to-SPC input latches. At an equal timestamp, the completed SPC edge precedes the CPU port access. Main-bus accesses still cost six master cycles. The page-$21 PPU handler forwards APU accesses with their original timestamp; the APU's private bus needs no main-bus token or blocking transaction.

**Timing limits:** port reads currently sample at whole SPC-cycle boundaries. Half-cycle port sampling, physical read/write collision behavior, oscillator variation, and TEST-register wait-state modes remain future accuracy work. This implementation establishes deterministic cycle sequencing without claiming complete S-SMP timing accuracy.

## Memory and hardware boundary

The IPL overlays reads at `$FFC0`–`$FFFF`; writes always reach the RAM underneath. CONTROL bit 7 switches the overlay, and bits 4/5 clear the corresponding pairs of CPU-to-SPC input latches on each write. These clears leave the output latches intact. MMIO writes also update underlying RAM. `$F8`/`$F9` provide auxiliary storage, and write-only registers read as zero.

Reset uses a **deterministic cold-start policy**: zeroed ARAM and port latches, enabled IPL, cleared clock phase, and a fresh CPU at `$FFC0`. Physical power-on RAM/register contents are not promised. `Spc700::Reset(State)` initializes an instruction boundary for tests; it is not a save-state API and cannot restore an instruction in progress.

Every opcode has an implementation. Timer enabling, DSP data access, and non-default TEST modes still fault explicitly. The hardware fault persists until APU reset and is reported to the emulator, debugger, or headless caller. Disabled timer outputs read zero; writing a timer target alone does not enable it. Both existing S-DSP backend classes remain unused scaffolding.

The former stub's commercial-game boot behavior is therefore **not a compatibility guarantee for the current APU**. Timers and DSP behavior are the next work before voices, mixing, and host playback.

## Verification

`testroms/apu_upload/apu_upload.s` boots the 65C816 from its reset vector, waits for the IPL signature, uploads a program through the normal counter handshake, launches it, and waits for a computed reply. The first payload crosses a 256-byte boundary, and a second block verifies continuation between transfers. The SPC program increments a command and returns it on a different port; the 65C816 records `PASS` and the reply in WRAM. Replacing the uploaded increment instruction with NOP must produce the wrong reply and the CPU's failure verdict.

`testroms/apu_transfers/apu_transfers.s` uploads a second self-contained program through the IPL. It executes all 41 byte `MOV` forms, both bit transfers, and all eight stack transfers, leaving an independently checked 28-byte signature at `$1000` and a computed reply for the 65C816. It exercises both direct pages, wrapped pointers and indexed addresses, RAM beneath the disabled IPL, and stack-pointer wrapping. Changing the final absolute load's source address must change the reply and trigger the CPU's failure verdict.

`testroms/apu_arithmetic/apu_arithmetic.s` uploads a 768-byte sound program and records 27 independently calculated result/flag triples. It chains carry and borrow, modifies byte and word operands in memory, compares values, adjusts decimal results, and divides a product. The CPU expects a reply of `$62`; changing the final addition to subtraction must produce `$5D` and the CPU's failure verdict. This program also compares full machine state across large, irregular, and one-master-cycle execution slices.

`testroms/apu_control/apu_control.s` uploads 1,024 bytes and exercises all sixteen TCALL vectors, nested calls, PCALL, BRK/RETI, conditional and bit branches, loop instructions, logical operations, and shifts. It records sixteen independently checked result bytes and returns `$92`; changing its final XOR operand produces `$93` and the CPU's failure verdict. Both SLEEP and STOP endings run across large, irregular, and single-cycle master slices.

The integration tests compare uploaded bytes against the assembled ROM and compare final CPU registers, SPC registers, ARAM, ports, and clocks across large, irregular, and one-master-cycle slices. Focused tests check all newly added opcodes, flags, direct-page and absolute wrapping, individual bus-access cycles, dummy reads, bit preservation, and stack behavior. Existing APU tests cover word transfers, IPL overlay, directional ports, control clears, reset, and unsupported-operation diagnostics.

Arithmetic unit tests check every addressing form, both direct pages, operand latching, and the cycle on which results and flags become visible. Aggregate sweeps compare 524,288 byte addition/subtraction combinations, 131,072 comparisons, and 40,000 two-digit decimal calculations against independent arithmetic expectations. Word, multiplication, division, and invalid-decimal boundary cases have explicit expected results and bus traces.

An independent 256-entry instruction-length/cycle table checks that every opcode reaches its expected first boundary and fetches the next instruction, or enters its appropriate halt state. Focused logic tests cover 393,216 byte-logical combinations, 4,096 shift/rotate combinations, and 512 nibble-exchange cases, alongside explicit bit-operation and flag traces. Control-flow tests cover taken and untaken paths, all bit and TCALL indices, vector and stack ordering, operand latching, and signed address wrapping. All 124 opcodes supported before the final expansion retain their cycle counts.

**Known hardware limitation (September 9, 2026):** Super Mario World now executes the formerly unsupported `CALL` at `$0530`, enters `$0697`, and pushes the correct `$0533` return address. It then stops on the unimplemented DSP data port `$00F3`, during the dummy read of `MOV !$00F3,A` at `$069A`; the diagnostic reports the post-operand PC `$069D`. The optional commercial-ROM test `SMW title-screen color-math fills the sky region` still fails when that ROM is present, and its graphics assertions remain unchanged. DSP register handling and timers are the next hardware work before commercial gameplay can be revalidated.

**Verification snapshot (September 9, 2026):** all 99 APU cases pass (91,628 assertions). The final opcode expansion adds 12 logic cases, 17 control/decode cases, three uploaded-program cases, and a CPU-port halt test. `ci-verify.sh` passes its build and all 602 unit cases (109,850 assertions); repository-wide lint still reports its non-blocking backlog, with no diagnostics in files changed for this expansion. The full suite passes 794 of 795 cases (131,594 of 131,595 assertions), with only the DSP-access failure above.

```sh
cmake --preset ci
cmake --build --preset ci
./build/ci/pupsnes_tests '[apu]'
./ci-verify.sh
./build/ci/pupsnes_tests
```

## References

The IPL bytes and upload protocol are documented in [Anomie's SPC700 reference](https://github.com/gilligan/snesdev/blob/master/docs/spc700.txt). Instruction cycle ordering was checked against the [ares SPC700 implementation](https://github.com/ares-emulator/ares/blob/master/ares/component/processor/spc700/instructions.cpp), with arithmetic flags compared to its [ALU algorithms](https://github.com/ares-emulator/ares/blob/master/ares/component/processor/spc700/algorithms.cpp). The [ares S-SMP memory implementation](https://github.com/ares-emulator/ares/blob/master/ares/sfc/smp/memory.cpp) and [I/O implementation](https://github.com/ares-emulator/ares/blob/master/ares/sfc/smp/io.cpp) provide comparisons for memory overlays, directional latches, and control behavior.
