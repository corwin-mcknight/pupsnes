.include "pupsnes.inc"

.segment "CODE"

start:
    lda #$7E
    pha
    plb                 ; DBR = $7E

    ldx #$10
    inx                 ; X = $11
    inx                 ; X = $12
    stx a:$0000         ; wram[0] = $12

    ldy #$20
    dey                 ; Y = $1F
    sty a:$0001         ; wram[1] = $1F

    lda #$00
    inc a               ; A = $01
    inc a               ; A = $02
    dec a               ; A = $01
    sta a:$0002         ; wram[2] = $01

    ldx #$FF
    inx                 ; X wraps to $00
    stx a:$0003         ; wram[3] = $00

    ldx #$00
    dex                 ; X wraps to $FF
    stx a:$0004         ; wram[4] = $FF

    ldy #$01
    dey                 ; Y = $00
    dey                 ; Y wraps to $FF
    sty a:$0005         ; wram[5] = $FF

loop:
    bra loop

.segment "HEADER"
    .byte "PUPSNES INC DEC SMOKE"
    PUPSNES_LOROM_HEADER_BYTES

PUPSNES_RESET_VECTORS start
