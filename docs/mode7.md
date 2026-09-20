# Mode 7

Mode 7 draws a transformed 1024×1024 background through a signed 8.8 matrix. PupSNES implements the matrix and center registers, shared scroll latches, screen flips, wrapping, transparent overflow, and character-zero overflow. EXTBG adds the second background interpretation and its sprite priorities. Both screens support independent layer-window masking and participate in color-window clipping and color math; BG1 supports direct color.

The tilemap uses the low bytes of the first 16K VRAM words; packed 8×8 characters use their high bytes. Ordinary background tilemap bases, character bases, and tile-size flags do not affect Mode 7. Palette index zero is transparent. EXTBG BG2 uses seven color bits and the top bit as priority, so index $80 is transparent on BG2 while still opaque on BG1.

The transform uses the hardware vertical counter (the first visible line is 1), signed 13-bit centers and scroll, clipped scroll-minus-center differences, and separate six-bit truncation of the origin products. The arithmetic and priority ordering were checked against the primary [ares Mode 7 renderer](https://github.com/ares-emulator/ares/blob/master/ares/sfc/ppu-performance/mode7.cpp) and [register implementation](https://github.com/ares-emulator/ares/blob/master/ares/sfc/ppu-performance/io.cpp).

## Validation

`build/ci/pupsnes_tests '[mode7]'` checks register latch sharing, addressing, transforms, fractional arithmetic, overflow, transparency, sprite priorities, both screens, direct color, and timed writes. It also boots `ppu_mode7.sfc` through the CPU and scheduler, compares the complete frame at two scheduler slice sizes, and verifies the picture remains unchanged after ten seconds. HDMA follows the PPU's alternating NTSC frame lengths and begins its writes after the visible pixels.

The original test ROM uses DMA to initialize the tilemap and HDMA to change the scale in three horizontal bands. Its four-color checkerboard is sheared, with progressively smaller checks from top to bottom. To view it:

```sh
build/ci/pupsnes-screenshot --rom build/ci/test-roms/ppu_mode7.sfc --frames 8 --output /tmp/mode7.ppm
```

## Remaining work

Commercial-game compatibility still needs scene-by-scene validation. Interlace remains unimplemented. Mode 7 register and VRAM writes follow the existing PPU dot replay model; hardware-specific fetch and register sampling phases are not modeled separately. HDMA CPU/DMA clock alignment, full channel overhead, and overscan transfers remain timing follow-ups. The test ROM establishes ordinary scanline HDMA behavior, not every mid-scanline hardware timing edge case.
