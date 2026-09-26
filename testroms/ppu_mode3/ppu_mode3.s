.include "pupsnes.inc"

; Three 80/80/64-line bands: indexed BG1, direct-color BG1, then direct
; color with saturating addition of fixed RGB(4,8,12) on BG1 only.
; BG1 uses asymmetric 16x16 8bpp motifs, BG2 uses 8x8 4bpp motifs, and
; each band contains four 16x16 OBJs at priority levels 0,1,2,3. Flipped
; tilemap entries, transparent apertures, and nonuniform pixels expose
; decoding, character stride, priority, and direct-color palette errors.
;
; Stable capture: pupsnes-screenshot --rom ppu_mode3.sfc --frames 8.

.macro UPLOAD_VRAM WordAddress, Source, Size
    ldx #WordAddress
    stx $2116
    lda #$01
    sta $4370           ; DMA mode 1: alternate VMDATAL/VMDATAH.
    lda #$18
    sta $4371
    ldx #Source
    stx $4372
    stz $4374           ; All source data is in bank zero.
    ldx #Size
    stx $4375
    lda #$80
    sta $420B
.endmacro

; Encode one 8x8 character from its logical position inside a 16x16 motif.
; The two motifs use all eight index bits, with XOR $55 distinguishing the
; second motif. This emits planar bytes, not a runtime renderer shortcut.
.macro BG1_CHARACTER Character
    .local Bits, Pixel, SourceX, SourceY, Variant
    .repeat 4, Pair
        .repeat 8, Row
            .repeat 2, Plane
                Bits .set 0
                .repeat 8, Column
                    SourceX .set (Character & 1) * 8 + Column
                    SourceY .set (Character / 16) * 8 + Row
                    Variant .set (Character / 2) & 1
                    Pixel .set ((SourceX & 7) | ((SourceY & 7) << 3) | ((SourceX / 8) << 6) | ((SourceY / 8) << 7)) ^ (Variant * $55)
                    .if ((SourceX >= 4) && (SourceX < 8) && (SourceY >= 4) && (SourceY < 8)) || (((SourceX + 2 * SourceY + Variant) .mod 11) = 0)
                        Pixel .set 0
                    .endif
                    Bits .set Bits | (((Pixel >> (Pair * 2 + Plane)) & 1) << (7 - Column))
                .endrepeat
                .byte Bits
            .endrepeat
        .endrepeat
    .endrepeat
.endmacro

.macro OBJ_CHARACTER Character
    .local Bits, Pixel, SourceX, SourceY
    .repeat 2, Pair
        .repeat 8, Row
            .repeat 2, Plane
                Bits .set 0
                .repeat 8, Column
                    SourceX .set (Character & 1) * 8 + Column
                    SourceY .set (Character / 16) * 8 + Row
                    Pixel .set 1 + ((SourceX / 4 + 4 * (SourceY / 4)) .mod 15)
                    .if (SourceX = 0) || (SourceY = 0) || (SourceX = 15) || (SourceY = 15) || (((SourceX + 2 * SourceY) .mod 9) = 0)
                        Pixel .set 0
                    .endif
                    Bits .set Bits | (((Pixel >> (Pair * 2 + Plane)) & 1) << (7 - Column))
                .endrepeat
                .byte Bits
            .endrepeat
        .endrepeat
    .endrepeat
.endmacro

.segment "CODE"
start:
    sei
    cld
    clc
    xce
    rep #$10
    .i16
    .a8
    ldx #$1FFF
    txs
    lda #$80
    sta $2100           ; Forced blank during all uploads.
    stz $4200
    stz $420C
    sta $2115           ; VRAM increments after the high-byte write.

    UPLOAD_VRAM $0000, bg1_map, bg1_map_end - bg1_map
    UPLOAD_VRAM $0400, bg2_map, bg2_map_end - bg2_map
    UPLOAD_VRAM $1000, bg1_tiles, bg1_tiles_end - bg1_tiles
    UPLOAD_VRAM $3000, bg2_tile, bg2_tile_end - bg2_tile
    UPLOAD_VRAM $6000, obj_tiles, obj_tiles_end - obj_tiles

    stz $2121
    ldx #$0000
upload_palette:
    lda palette, x
    sta $2122
    inx
    cpx #512
    bne upload_palette

    stz $2102
    stz $2103
    ldx #$0000
upload_oam:
    lda sprites, x
    sta $2104
    inx
    cpx #544
    bne upload_oam

    lda #$13
    sta $2105           ; Mode 3, BG1 16x16, BG2 8x8.
    sta $212C           ; Main screen: BG1, BG2, OBJ.
    stz $212D
    stz $2106
    stz $2107           ; BG1 map at word $0000, 32x32 entries.
    lda #$04
    sta $2108           ; BG2 map at word $0400, 32x32 entries.
    lda #$31
    sta $210B           ; BG1/BG2 characters at word $1000/$3000.
    lda #$03
    sta $2101           ; OBJ word base $6000, size pair 8x8/16x16.
    stz $210D
    stz $210D
    stz $210E
    stz $210E
    stz $210F
    stz $210F
    stz $2110
    stz $2110
    stz $2123
    stz $2124
    stz $2125
    stz $212E
    stz $212F
    stz $2130
    stz $2131
    stz $2133
    lda #$24
    sta $2132           ; Fixed red 4.
    lda #$48
    sta $2132           ; Fixed green 8.
    lda #$8C
    sta $2132           ; Fixed blue 12.

    ; HDMA mode 1 writes CGWSEL and CGADSUB together before each band.
    lda #$01
    sta $4300
    lda #$30
    sta $4301
    ldx #color_bands
    stx $4302
    stz $4304
    lda #$01
    sta $420C
    lda #$0F
    sta $2100
forever:
    bra forever

.segment "RODATA"
color_bands:
    .byte 80, $00, $00   ; y=0..79: indexed, no math.
    .byte 80, $01, $00   ; y=80..159: direct, no math.
    .byte 64, $01, $01   ; y=160..223: direct, BG1 fixed-color addition.
    .byte 0

palette:
    .repeat 256, Index
        .if Index = 0
            .word $1042 ; Dark backdrop, distinguishable from transparency.
        .elseif Index >= 192
            ; OBJ palettes 4..7: yellow, magenta, cyan, white gradients.
            .if Index < 208
                .word 31 | ((16 + (Index & 15)) << 5)
            .elseif Index < 224
                .word (16 + (Index & 15)) | (31 << 10)
            .elseif Index < 240
                .word (31 << 5) | ((16 + (Index & 15)) << 10)
            .else
                .word (16 + (Index & 15)) * $421
            .endif
        .else
            .word ((Index * 3 + 5) & 31) | ((((Index / 8) * 5 + 9) & 31) << 5) | ((((Index / 32) * 7 + Index * 3 + 13) & 31) << 10)
        .endif
    .endrepeat

bg1_map:
    .repeat 32, Row
        .repeat 32, Column
            ; 64px motif variants; palette bits matter only in direct mode.
            .word (((Column / 4 + Row / 4) & 1) * 2) | (((Column + Row * 3) & 7) << 10) | (((Column / 2) & 1) << 13) | ((Column & 1) << 14) | ((Row & 1) << 15)
        .endrepeat
    .endrepeat
bg1_map_end:

bg2_map:
    .repeat 32, Row
        .repeat 32, Column
            .word ((2 + ((Column / 4 + Row / 4) & 1)) << 10) | (((Row / 2) & 1) << 13) | ((Column & 1) << 14) | ((Row & 1) << 15)
        .endrepeat
    .endrepeat
bg2_map_end:

bg1_tiles:
    .repeat 20, Character
        .if (Character & 15) < 4
            BG1_CHARACTER Character
        .else
            .res 64, 0
        .endif
    .endrepeat
bg1_tiles_end:

bg2_tile:
    .repeat 2, Pair
        .repeat 8, Row
            .repeat 2, Plane
                Bg2Bits .set 0
                .repeat 8, Column
                    Bg2Pixel .set 1 + ((Column / 2 + 3 * (Row / 2)) .mod 15)
                    .if ((Column + Row) .mod 4) = 0
                        Bg2Pixel .set 0
                    .endif
                    Bg2Bits .set Bg2Bits | (((Bg2Pixel >> (Pair * 2 + Plane)) & 1) << (7 - Column))
                .endrepeat
                .byte Bg2Bits
            .endrepeat
        .endrepeat
    .endrepeat
bg2_tile_end:

obj_tiles:
    .repeat 18, Character
        .if (Character & 15) < 2
            OBJ_CHARACTER Character
        .else
            .res 32, 0
        .endif
    .endrepeat
obj_tiles_end:

sprites:
    .repeat 3, Band
        .repeat 4, Priority
            ; Each sprite crosses a BG1 priority boundary at x=32+64*p.
            .byte 24 + 64 * Priority, 24 + 80 * Band, 0
            .byte (Priority << 4) | ((4 + Priority) << 1)
        .endrepeat
    .endrepeat
    .repeat 116
        .byte 0, 240, 0, 0   ; Hide every unused 8x8 OBJ below the screen.
    .endrepeat
    .byte $AA, $AA, $AA      ; Large-size bit for the twelve 16x16 OBJs.
    .res 29, 0

.segment "HEADER"
    .byte "PUPSNES MODE3 TEST", $00, $00, $00
    PUPSNES_LOROM_HEADER_BYTES

PUPSNES_RESET_VECTORS start
