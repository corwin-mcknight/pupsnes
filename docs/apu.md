# APU and sound synthesis

PupSNES implements **all 256 SPC700 opcodes**, the three APU timers, and S-DSP synthesis. The processor runs the real 64-byte IPL program, receives bytes into its own 64 KiB RAM, and executes uploaded sound programs. The DSP produces a native **32 kHz, 16-bit stereo stream**, which can play through either frontend or be captured to WAV without an audio device. See [Audio](audio.md) for controls and commands. Complete instruction and synthesis coverage does not mean complete S-SMP hardware or timing accuracy.

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

The IPL overlays reads at `$FFC0`–`$FFFF`; writes always reach the RAM underneath. CONTROL bit 7 switches the overlay, and bits 4/5 clear the corresponding pairs of CPU-to-SPC input latches on each write. These clears leave the output latches intact. MMIO writes also update underlying RAM. `$F8`/`$F9` have separate auxiliary latches: DSP echo can overwrite the RAM beneath them without changing their SPC-visible values. Write-only registers read as zero.

Reset uses a **deterministic cold-start policy**: zeroed ARAM and port latches, enabled IPL, cleared clock phase, and a fresh CPU at `$FFC0`. Physical power-on RAM/register contents are not promised. `Spc700::Reset(State)` initializes an instruction boundary for tests; it is not a save-state API and cannot restore an instruction in progress.

Non-default TEST modes still fault explicitly. The hardware fault persists until APU reset and is reported to the emulator, debugger, or headless caller. The normal `$0A` TEST configuration is supported; TEST speed controls, RAM restrictions, and timer gating remain accuracy work.

## Timers and DSP registers

All three timers run from the SPC clock. Timers 0 and 1 have a 128-cycle prescaler; timer 2 has a 16-cycle prescaler. These prescalers continue while their timer is disabled. CONTROL bits 0–2 enable each timer, and a transition from disabled to enabled clears its 8-bit counter and 4-bit output without restarting the prescaler. Rewriting an already-set enable bit preserves the counter and output.

Targets at `$FA`–`$FC` compare against the counter after it increments. A match clears the counter and increments the output modulo 16; target zero therefore means 256 ticks. Changing a target preserves the running counter, including the long wraparound wait when the new target is below it. Reading `$FD`–`$FF` returns and clears the corresponding output. Writes have no timer-output effect, although an instruction's dummy read can clear the output before its write. Targets remain write-only and read zero.

`$F2` retains the full DSP address byte. `$F3` reads one of 128 DSP registers using the lower seven address bits; addresses `$80`–`$FF` are read-only mirrors, so writes through them leave the DSP unchanged. Ordinary register writes preserve all eight bits for readback, including bits unused by synthesis. Writing any value to ENDX (`$7C`) clears it. Cold reset zeroes the register bank and sets FLG (`$6C`) to `$E0`; this is a deterministic seed, not a claim about every physical power-on register.

The APU owns the selected `SimpleSdsp` or `AccurateSdsp` backend and advances it **one SPC clock at a time**. Register updates and ARAM accesses happen at their DSP pipeline phases. The internal DAC result is produced at phase 27 and delivered through `SNES::SetAudioSampleCallback` at the end of each 32-clock period. This gives a regular native 32 kHz stream without postponing the hardware effects until a sample boundary.

Timers and DSP advance before the SPC bus access at the same whole-cycle edge and continue during SLEEP and STOP. A halted main CPU also leaves sound running while machine time advances. Reset clears the hardware phases and counters but preserves the frontend's sample callback. Pending sound-interpolation selection takes effect at the next SNES reset.

## Synthesis and playback

Both backends use Shay Green's **blargg snes_spc 0.9.0** S-DSP core, pinned to revision `ec8ee2bbe30451614c1d02a83f7af1c97d497d45`. It supplies eight voices, all BRR filters and loop/end handling, ADSR and GAIN envelopes, key-on/key-off sequencing, noise, pitch modulation, signed stereo mixing, and echo with its FIR filter and ARAM writes. ENVX, OUTX, and ENDX now evolve with voice execution rather than remaining a static register bank. See the [vendored source notes](../src/third_party/snes_spc/README.md) for provenance, licensing, and local adaptations.

**Gaussian (SNES)** is the default and maps to `AccurateSdsp`. **Linear** maps to `SimpleSdsp`; it changes sample interpolation inside the same DSP pipeline. Both modes retain the voices, envelopes, effects, and clock sequencing. They need not produce identical PCM, OUTX, pitch-modulated voices, or echo RAM because those values depend on the interpolated samples. The mode name describes this choice, not a guarantee of complete console accuracy.

The emulator and debugger send samples to a bounded queue and a miniaudio playback device. The host callback consumes that queue, applies user volume/mute, and linearly resamples to the device's native rate; it never advances the emulated machine. Queue underflow produces silence, and overflow drops new frames rather than replacing unread samples. Pause, reset, ROM load, debugger stepping, and speed changes discard queued sound as needed. Playback is enabled only during continuous execution at **100% speed**. WAV capture receives the native stream before these host controls and resampling.

Remaining limits include whole-cycle S-SMP port sampling, physical port collision behavior, non-default TEST modes, timer target-write glitches, oscillator variation, and PAL timing. The DSP integration also does not model the console's analog output stage or the upstream mute-toggle transient. Sound-driver compatibility needs continued game and diagnostic testing; the former handshake stub's gameplay results do not establish that every driver now behaves correctly.

## Verification

`testroms/apu_upload/apu_upload.s` boots the 65C816 from its reset vector, waits for the IPL signature, uploads a program through the normal counter handshake, launches it, and waits for a computed reply. The first payload crosses a 256-byte boundary, and a second block verifies continuation between transfers. The SPC program increments a command and returns it on a different port; the 65C816 records `PASS` and the reply in WRAM. Replacing the uploaded increment instruction with NOP must produce the wrong reply and the CPU's failure verdict.

`testroms/apu_transfers/apu_transfers.s` uploads a second self-contained program through the IPL. It executes all 41 byte `MOV` forms, both bit transfers, and all eight stack transfers, leaving an independently checked 28-byte signature at `$1000` and a computed reply for the 65C816. It exercises both direct pages, wrapped pointers and indexed addresses, RAM beneath the disabled IPL, and stack-pointer wrapping. Changing the final absolute load's source address must change the reply and trigger the CPU's failure verdict.

`testroms/apu_arithmetic/apu_arithmetic.s` uploads a 768-byte sound program and records 27 independently calculated result/flag triples. It chains carry and borrow, modifies byte and word operands in memory, compares values, adjusts decimal results, and divides a product. The CPU expects a reply of `$62`; changing the final addition to subtraction must produce `$5D` and the CPU's failure verdict. This program also compares full machine state across large, irregular, and one-master-cycle execution slices.

`testroms/apu_control/apu_control.s` uploads 1,024 bytes and exercises all sixteen TCALL vectors, nested calls, PCALL, BRK/RETI, conditional and bit branches, loop instructions, logical operations, and shifts. It records sixteen independently checked result bytes and returns `$92`; changing its final XOR operand produces `$93` and the CPU's failure verdict. Both SLEEP and STOP endings run across large, irregular, and single-cycle master slices.

`testroms/apu_hardware/apu_hardware.s` uploads a 512-byte program and records an 18-byte hardware signature. It exercises DSP readback, read-only upper mirrors, ENDX clearing, all three timer targets and outputs, and read-clear/ignored-write behavior. The CPU expects a computed `$38` reply. Disabling timer 0 in the uploaded bytes must take a bounded timeout path and produce an explicit failure verdict. Its tests compare the full DSP register bank, every timer field, CPU/SPC state, ARAM, ports, and clock phases across large, irregular, and single-master-cycle slices, plus both DSP backends.

`testroms/apu_audio/apu_audio.s` uploads a 512-byte program that writes a BRR sample and directory into ARAM, sets voice registers, and keys on a looping **500 Hz tone** with different left/right volumes. Both CPUs then STOP while the DSP continues playing. Tests check nonzero periodic PCM, channel differences, ENVX/ENDX evolution, and sample/state equality across execution slices in each interpolation mode. Removing the uploaded KON value leaves the CPU's normal acknowledgment intact but must produce silence, so a synthetic handshake cannot satisfy the audio check.

The integration tests compare uploaded bytes against the assembled ROM and compare final CPU registers, SPC registers, ARAM, ports, and clocks across large, irregular, and one-master-cycle slices. Focused tests check all newly added opcodes, flags, direct-page and absolute wrapping, individual bus-access cycles, dummy reads, bit preservation, and stack behavior. Existing APU tests cover word transfers, IPL overlay, directional ports, control clears, reset, and unsupported-operation diagnostics.

Arithmetic unit tests check every addressing form, both direct pages, operand latching, and the cycle on which results and flags become visible. Aggregate sweeps compare 524,288 byte addition/subtraction combinations, 131,072 comparisons, and 40,000 two-digit decimal calculations against independent arithmetic expectations. Word, multiplication, division, and invalid-decimal boundary cases have explicit expected results and bus traces.

An independent 256-entry instruction-length/cycle table checks that every opcode reaches its expected first boundary and fetches the next instruction, or enters its appropriate halt state. Focused logic tests cover 393,216 byte-logical combinations, 4,096 shift/rotate combinations, and 512 nibble-exchange cases, alongside explicit bit-operation and flag traces. Control-flow tests cover taken and untaken paths, all bit and TCALL indices, vector and stack ordering, operand latching, and signed address wrapping. All 124 opcodes supported before the final expansion retain their cycle counts.

Focused hardware tests cover exact timer prescaler edges, enabling/disabling and repeated CONTROL writes, zero targets, 4-bit overflow, target changes below the current count, independent read clearing, and timer-output dummy reads. DSP tests cover all 128 register addresses, reset state, ENDX, upper address mirrors, backend selection, sample cadence during SLEEP/STOP, and slicing determinism.

Synthesis tests compare Gaussian-mode PCM, ARAM, and DSP-register fingerprints against a separate build of the unmodified pinned blargg core. Fixtures cover BRR filters and ranges, envelopes, noise, pitch modulation, clipping, key events, and echo. These comparisons validate the wrapper against its reference engine; they are not independent measurements of physical hardware. Separate tests check cycle-visible register behavior, wrapping sample-directory addresses, and the linear mode.

Host-output tests exercise stereo resampling, bounded buffering, gain/mute, underflow, overflow, flushes, callback slicing, and producer/consumer concurrency. Config tests reject malformed or nonfinite settings. Those tests run without opening a sound device; native playback is checked separately.

The September 10, 2026 verification passed **853 test cases and 315,490 assertions** across the full suite. `ci-verify.sh` passed its build and 654 unit cases; repository-wide lint still reports the existing backlog, with no diagnostics in changed files. Separate sanitizer runs covered the DSP wrapper and concurrent audio queue. A 24-second Core Audio check at 48 kHz had no dropped frames or underruns after startup/resume priming, and pause/resume discarded queued audio correctly. Super Mario World produced stereo PCM and was confirmed audible by manual listening; the emulator controls and saved audio preferences were also exercised. These are smoke checks, not game-completion or broad compatibility claims.

```sh
cmake --preset ci
cmake --build --preset ci
./build/ci/pupsnes_tests '[apu]'
./build/ci/pupsnes_tests '[audio_output]'
./build/ci/pupsnes_tests '[audio][config]'
./ci-verify.sh
./build/ci/pupsnes_tests
```

## References

The IPL bytes and upload protocol are documented in [Anomie's SPC700 reference](https://github.com/gilligan/snesdev/blob/master/docs/spc700.txt). Instruction cycle ordering was checked against the [ares SPC700 implementation](https://github.com/ares-emulator/ares/blob/master/ares/component/processor/spc700/instructions.cpp), with arithmetic flags compared to its [ALU algorithms](https://github.com/ares-emulator/ares/blob/master/ares/component/processor/spc700/algorithms.cpp). The [ares S-SMP memory implementation](https://github.com/ares-emulator/ares/blob/master/ares/sfc/smp/memory.cpp) and [I/O implementation](https://github.com/ares-emulator/ares/blob/master/ares/sfc/smp/io.cpp) provide comparisons for memory overlays, directional latches, and control behavior.

Timer division and read-clear behavior are described in [Anomie's SPC700 reference](https://github.com/gilligan/snesdev/blob/master/docs/spc700.txt) and the [ares timer implementation](https://github.com/ares-emulator/ares/blob/master/ares/sfc/smp/timing.cpp). DSP register readback and ENDX write behavior were checked against [ares DSP memory handling](https://github.com/ares-emulator/ares/blob/master/ares/sfc/dsp/memory.cpp); the deterministic FLG reset seed follows [blargg's SPC_DSP as shipped by bsnes](https://github.com/bsnes-emu/bsnes/blob/master/bsnes/sfc/dsp/SPC_DSP.cpp).

The synthesis engine and local changes are documented in [snes_spc source notes](../src/third_party/snes_spc/README.md). Host playback uses [miniaudio 0.11.25](../third_party/miniaudio/README.md), with its low-level playback API and no capture device.
