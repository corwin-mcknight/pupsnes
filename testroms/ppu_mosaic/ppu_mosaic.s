.include "pupsnes.inc"

; BG1 4bpp tile whose four 4x4 source-block origins are red, green, blue, and
; white. MOSAIC=4x4 makes every screen tile render as those four solid blocks.
; The non-origin pixels deliberately differ, so bypassing mosaic cannot produce
; the expected framebuffer.

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
    sta $2100           ; Forced blank while programming PPU state.
    stz $4200
    stz $420C

    ; VMAIN: increment after VMDATAH. Upload tile 0 at BG1's $1000 word base.
    lda #$80
    sta $2115
    stz $2116
    lda #$10
    sta $2117
    ldx #$0000
upload_tile:
    lda tile, x
    sta $2118
    inx
    lda tile, x
    sta $2119
    inx
    cpx #32
    bne upload_tile

    ; CGRAM colors 0..6. Tilemap VRAM is reset to zero, so every entry selects
    ; tile 0 with palette 0.
    stz $2121
    ldx #$0000
upload_palette:
    lda palette, x
    sta $2122
    inx
    cpx #14
    bne upload_palette

    lda #$01
    sta $2105           ; Mode 1, BG1 4bpp.
    stz $2107           ; BG1 tilemap at word $0000.
    lda #$01
    sta $210B           ; BG1 character data at word $1000.
    sta $212C           ; Enable BG1 on the main screen.
    lda #$31
    sta $2106           ; 4x4 mosaic blocks, BG1 enabled.

    lda #$0F
    sta $2100           ; Display on, full brightness.
forever:
    bra forever

.segment "RODATA"
palette:
    .word $0000, $001F, $03E0, $7C00, $7FFF, $03FF, $7FE0

; 4bpp tile: plane 0/1 rows, then plane 2/3 rows. The four mosaic sources
; at (0,0), (4,0), (0,4), and (4,4) are palette indices 1, 2, 3, and 4.
tile:
    .byte $B4, $6D
    .byte $69, $DB
    .byte $D2, $B6
    .byte $B4, $6D
    .byte $B4, $92
    .byte $69, $24
    .byte $D2, $49
    .byte $B4, $92
    .byte $02, $00
    .byte $04, $00
    .byte $09, $00
    .byte $02, $00
    .byte $6F, $00
    .byte $DF, $00
    .byte $BF, $00
    .byte $6F, $00

.segment "HEADER"
    .byte "PUPSNES MOSAIC TEST", $00
    PUPSNES_LOROM_HEADER_BYTES

PUPSNES_RESET_VECTORS start
