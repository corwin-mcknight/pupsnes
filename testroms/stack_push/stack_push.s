.include "pupsnes.inc"

.segment "CODE"

start:
    lda #$AB
    pha
    phb

loop:
    bra loop

.segment "HEADER"
    .byte "PUPSNES STACK PUSH  ", $00
    PUPSNES_LOROM_HEADER_BYTES

PUPSNES_RESET_VECTORS start
