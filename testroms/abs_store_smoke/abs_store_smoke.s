.include "pupsnes.inc"

.segment "CODE"

start:
    lda #$7E
    pha
    plb                 ; DBR = $7E

    lda #$11
    sta a:$0000         ; $8D — write A via DBR to $7E:0000

    ldx #$22
    stx a:$0001         ; $8E — write X via DBR to $7E:0001

    ldy #$33
    sty a:$0002         ; $8C — write Y via DBR to $7E:0002

loop:
    bra loop

.segment "HEADER"
    .byte "PUPSNES ABS STORES  ", $00
    PUPSNES_LOROM_HEADER_BYTES

PUPSNES_RESET_VECTORS start
