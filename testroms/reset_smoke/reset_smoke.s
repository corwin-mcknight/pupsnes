.include "pupsnes.inc"

.segment "CODE"

start:
    lda #$42
    nop

loop:
    bra loop

.segment "HEADER"
    .byte "PUPSNES RESET SMOKE", $00, $00
    PUPSNES_LOROM_HEADER_BYTES

PUPSNES_RESET_VECTORS start
