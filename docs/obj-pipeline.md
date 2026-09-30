# Sprite selection and fetch timing

**OBJ preparation runs one line ahead of display.** During physical scanline V, the PPU selects sprites and fetches their tile rows for physical line V+1. The first displayed line is V=1, so OAM Y=0 still corresponds to framebuffer row 0. Preparing those rows is independent of TM/TS and window visibility.

The implementation uses these dot phases:

| Operation | Dots on the preparation line | Retained state |
| --- | --- | --- |
| Read sprite X/Y, high OAM bits, and size | 0, 2, …, 254 | Position for the following range check |
| Check eligibility and the 32-sprite limit | 1, 3, …, 255 | Selected OAM indices; range-over on the 33rd eligible sprite |
| Read the next tile's sprite position | 270, 272, …, 338 | Position and size at fetch time |
| Read attributes and form the tile address | 271, 273, …, 339 | Tile address, X, flip, palette and priority; time-over on the 35th eligible tile |
| Fetch graphics planes 0/1 | 272, 274, …, 338 | First VRAM word |
| Fetch graphics planes 2/3 | 273, 275, …, 339 | Complete eight-pixel tile row |

Selection starts at the rotated OAM index latched at dot 0. Fetching walks selected sprites backwards and their screen-space tile columns left to right. Fully clipped columns do not cost a fetch; X=-256 remains the hardware exception. The graphics and attributes of a fetched row are retained even if OAM, OBSEL, or VRAM changes before that row is displayed. Rectangular sprites flip their square halves vertically without exchanging the halves.

**The cache stores sampled graphics, not live memory and not final RGB.** Two sets of at most 34 fetched rows separate current-line output from next-line preparation. When a visible screen first needs OBJ, the PPU composites those rows into a 256-entry buffer of palette indices and priorities. Subsequent pixels use a direct lookup. CGRAM, brightness, main/sub enables, windows and color math remain live in the ordinary pixel-composition path. Hidden OBJ therefore avoids rasterization without losing the data needed if a screen or window enables it later.

## Batching without changing sampling order

`SyncObjPipeline(end_dot)` executes the scheduled operations strictly before `end_dot`. The main dot loop normally calls it at line start and the last dot. Before replay changes INIDISP, OBSEL, OAMDATA or either VRAM data port, it synchronizes the sprite operations preceding that write. STAT77 reads synchronize before sampling flags. A line-end fence completes preparation before the next line can consume its fetched rows.

This is safe because no intervening operation can change an input being sampled without passing a fence. The cursor and partially fetched word persist across calls. Batching changes when host code runs, while preserving the order of emulated reads and writes. Adding another mutation path for OAM, VRAM or sprite configuration requires extending these fences. CGRAM and visibility writes do not invalidate sampled rows: their effects belong to pixel composition instead.

The PPU retains its existing completed-dot contract: writes are replayed through each dot's nominal start, and operations become observable after that dot completes. Catch-up may stop at a partial dot without consuming its operation. Forced blank suppresses selection and graphics reads, and overflow flags persist through subsequent lines, reads and blanking until frame start.

## Evidence and remaining limits

The broad selection and fetch windows are supported by [paulb_nl's real-console OAM access investigation](https://forums.nesdev.org/viewtopic.php?t=18447). The split between position/attribute setup and the two graphics words follows [Mesen2's sprite pipeline](https://github.com/SourMesen/Mesen2/blob/master/Core/SNES/SnesPpu.cpp), particularly `EvaluateNextLineSprites`, `FetchSpriteData`, and `RenderScanline`. [Ares's accurate PPU](https://github.com/ares-emulator/ares/blob/master/ares/sfc/ppu/object.cpp) independently supports selection/fetch separation, reverse sprite fetch order, the clipped-column exception and rectangular flipping; it uses coarser setup timing, so agreement between implementations is not claimed for every edge. Sources were inspected September 27, 2026.

The limit and dropout fixtures are based on [Marcus Rowe's hardware diagnostic](https://github.com/undisbeliever/snes-test-roms/blob/master/src/hardware-tests/object-dropout-test.asm). The repository has 23 focused OBJ limit/timing tests, including word-fetch boundaries, changes before and after selection, late display enabling, live palette/window changes, long-dot catch-up partitioning, and overflow lifetime. Existing priority fixtures now allow the pipeline to sample their completed setup before inspecting the result.

**This does not implement every PPU bus quirk.** Active-display OAM address redirection, VRAM port access restrictions, separate PPU open-bus domains, odd-address rotation quirks, interlaced OBJ, and sub-dot forced-blank collision behavior remain outside this change. The timed-write tests deliberately exercise the current port model; they do not establish that every tested CPU write would reach the same memory address on a physical console. Exact phase edges are reference-informed and have not been newly measured on hardware.

The remaining work is tracked in these focused follow-ups:

| Limitation | Issue |
| --- | --- |
| Active-display OAM address redirection | [#13](https://github.com/corwin-mcknight/pupsnes/issues/13), under the memory-port investigation in #9 |
| VRAM port access restrictions | [#14](https://github.com/corwin-mcknight/pupsnes/issues/14), under #9 |
| Complete PPU open-bus domains: STAT77 bit 4 uses a PPU1 read latch; write-only PPU1 readback and PPU2 remain | [#15](https://github.com/corwin-mcknight/pupsnes/issues/15) |
| Odd-address OAM priority rotation | [#16](https://github.com/corwin-mcknight/pupsnes/issues/16) |
| Interlaced OBJ selection and fetching | [#17](https://github.com/corwin-mcknight/pupsnes/issues/17), coordinated with interlaced presentation in #6 |
| Sub-dot forced-blank collisions and hardware validation of phase edges | [#18](https://github.com/corwin-mcknight/pupsnes/issues/18) |

Performance experiments and the temporary every-dot differential reference belong under ignored `build/profiles/obj-timing/`; they are not alternate production renderers or user-selectable accuracy modes.

The measured preparation overhead in hidden and forced-blank scenes is tracked in [#20](https://github.com/corwin-mcknight/pupsnes/issues/20). Any optimization must retain selection, overflow reporting, sampled graphics, and late display enabling.
