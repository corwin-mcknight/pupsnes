.include "pupsnes.inc"

; Four-color Mode 7 checkerboard with three HDMA scale bands. Both matrix
; diagonals change together: 0.25, 0.5, then 1.0. B=0.5 shears the image.
.macro M7_WORD Port, Value
    lda #<Value
    sta Port
    lda #>Value
    sta Port
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
    sta $2100
    stz $4200
    stz $420C

    ; Channel 2: clear all low-byte tilemap entries, increment after low.
    stz $2115
    stz $2116
    stz $2117
    lda #$08
    sta $4320
    lda #$18
    sta $4321
    ldx #zero
    stx $4322
    stz $4324
    ldx #$4000
    stx $4325
    lda #$04
    sta $420B

    ; Tile zero's packed color indices live in high VRAM bytes.
    lda #$80
    sta $2115
    stz $2116
    stz $2117
    ldx #$0000
upload_tile:
    lda tile,x
    sta $2119
    inx
    cpx #64
    bne upload_tile
    stz $2121
    ldx #$0000
upload_palette:
    lda palette,x
    sta $2122
    inx
    cpx #10
    bne upload_palette

    lda #$07
    sta $2105
    stz $211A
    stz $2130
    stz $2131
    stz $2133
    M7_WORD $211B, $0040
    M7_WORD $211C, $0080
    M7_WORD $211D, $0000
    M7_WORD $211E, $0040
    M7_WORD $211F, $0000
    M7_WORD $2120, $0000
    M7_WORD $210D, $0000
    M7_WORD $210E, $0000
    lda #$01
    sta $212C
    stz $212D

    ; Channels 0/1 use mode 2: two bytes to the same write-twice port.
    lda #$02
    sta $4300
    sta $4310
    lda #$1B
    sta $4301
    lda #$1E
    sta $4311
    ldx #scale_table
    stx $4302
    stx $4312
    stz $4304
    stz $4314
    lda #$03
    sta $420C
    lda #$0F
    sta $2100
forever:
    bra forever

.segment "RODATA"
zero:
    .byte 0
scale_table:
    .byte 64, $40, $00
    .byte 64, $80, $00
    .byte 96, $00, $01
    .byte 0
palette:
    .word $0000, $001F, $03E0, $7C00, $7FFF
tile:
    .repeat 8, Row
        .repeat 8, Column
            .byte 1 + (Column / 4) + (Row / 4) * 2
        .endrepeat
    .endrepeat

.segment "HEADER"
    .byte "PUPSNES MODE7 HDMA", $00, $00, $00
    PUPSNES_LOROM_HEADER_BYTES

PUPSNES_RESET_VECTORS start
