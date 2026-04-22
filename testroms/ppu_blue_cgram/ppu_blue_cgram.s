.include "pupsnes.inc"

.segment "CODE"

; Minimal PPU bring-up:
;   1. Clear forced blank, set full brightness via INIDISP ($2100).
;   2. Point CGADD ($2121) at palette index 0.
;   3. Write the BGR555 word $7C00 (max blue) to CGRAM[0] via two
;      byte writes to CGDATA ($2122) — low byte first, high byte second.
start:
    sei

    lda #$0F
    sta $2100           ; INIDISP: forced blank off, brightness 15

    lda #$00
    sta $2121           ; CGADD = palette index 0

    lda #$00
    sta $2122           ; CGDATA low  — R=0, G<2:0>=0
    lda #$7C
    sta $2122           ; CGDATA high — B=31, G<4:3>=0, bit 15=0 → $7C00 (blue)

loop:
    bra loop

.segment "HEADER"
    .byte "PUPSNES PPU CGRAM BL", $00
    PUPSNES_LOROM_HEADER_BYTES

PUPSNES_RESET_VECTORS start
