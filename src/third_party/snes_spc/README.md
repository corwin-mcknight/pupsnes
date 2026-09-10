# snes_spc DSP core

This directory vendors Shay Green's **snes_spc 0.9.0** accurate S-DSP core,
distributed under **LGPL-2.1-or-later**. The full license is in `LICENSE.txt`;
the original copyright notices remain in the source.

Source repository: <https://github.com/blarggs-audio-libraries/snes_spc>

Pinned revision: `ec8ee2bbe30451614c1d02a83f7af1c97d497d45`

Imported files: `snes_spc/SPC_DSP.cpp`, `snes_spc/SPC_DSP.h`,
`snes_spc/blargg_common.h`, `snes_spc/blargg_config.h`,
`snes_spc/blargg_endian.h`, `snes_spc/blargg_source.h`, and `license.txt`.

PupSNES functional adaptations are marked in the source. Trailing whitespace
was also removed from the C++ sources and headers; the license is unchanged:

- Added a per-instance interpolation selector. Accurate mode retains the
  original Gaussian calculation. Simple mode linearly interpolates the two
  central samples from the same window, retaining even-valued output and the
  complete DSP pipeline. It can change OUTX, pitch modulation, and echo data.
- Wrapped the sample-directory address to 16 bits, matching the correction in
  [bsnes](https://github.com/bsnes-emu/bsnes/blob/7d5aa1e656b9171524d01b1b22917197d8121cb4/bsnes/sfc/dsp/SPC_DSP.cpp).

The PupSNES wrapper advances this core one SPC clock at a time. The core
produces a stereo DAC sample during phase 27; the wrapper delivers that sample
at the end of each 32-clock period. Register and ARAM effects retain their
individual clock timing. Reset uses an all-zero register seed except
`FLG=$E0`, preserving PupSNES's deterministic cold-start policy.

The upstream limitation around the small analog transient when toggling DSP
mute remains; no analog output-stage filter or host resampling is implemented
inside this core. Save-state support is retained upstream but is not exposed
by the PupSNES wrapper.

The Accurate-mode goldens in `src/tests/sdsp_synthesis_tests.cpp` were generated
with a separate build of the **unmodified** files from the pinned revision.
The driver used the test's fixture writes and event sequence, loaded the cold
register seed above, and called `run(32)` 1,024 times per scenario. Fingerprints
use 64-bit FNV-1a (offset `14695981039346656037`, prime `1099511628211`) over
little-endian signed 16-bit interleaved stereo samples, then separately over
all 65,536 ARAM bytes and all 128 DSP registers in ascending address order.
The source-directory wrap adaptation is tested separately because the
unmodified upstream source does not implement that wrap.
