.include "pupsnes.inc"

.segment "CODE"

start:
    lda #$5A
    sta $7E0000

loop:
    bra loop

.segment "HEADER"
    .byte "PUPSNES WRAM SIG ROM", $00
    PUPSNES_LOROM_HEADER_BYTES

PUPSNES_RESET_VECTORS start
