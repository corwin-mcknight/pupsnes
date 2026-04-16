.include "pupsnes.inc"

.segment "CODE"

start:
    lda #$12
    nop
    lda #$34
    nop

loop:
    bra loop

.segment "HEADER"
    .byte "PUPSNES INSTR SMOKE", $00, $00
    PUPSNES_LOROM_HEADER_BYTES

PUPSNES_RESET_VECTORS start
