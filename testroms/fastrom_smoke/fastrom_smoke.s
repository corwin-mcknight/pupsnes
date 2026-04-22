.include "pupsnes.inc"

.segment "CODE"

; ---------------------------------------------------------------------------
; FASTROM smoke test.
;
; Boots in the slow bank ($00:8000), enables FASTROM via MEMSEL ($420D),
; then JML's to the mirrored image of this same code in the fast bank
; ($80:8000+). Each stage writes a distinctive marker to WRAM so the harness
; can see exactly how far execution got; the final PBR=$80 proves the CPU is
; actually parked in a fast-eligible bank at the end of the run.
;
; Timing note: the ROM itself doesn't measure cycles, but running inside the
; cycle budget *while reaching* fast_loop with PBR=$80 is the FASTROM proof
; — slow-bank fetches would still complete, they just eat more of the budget.
; ---------------------------------------------------------------------------

start:
    ; Set DBR = $7E so `sta a:$xxxx` writes into WRAM bank $7E.
    lda #$7E
    pha
    plb

    ; Marker #0 — booted OK on slow bank $00.
    lda #$AA
    sta a:$0000

    ; Enable FASTROM: MEMSEL ($420D) bit 0 = 1.
    ; The mapper re-maps banks $80-$FD pages $80-$FF to 6-cycle access
    ; synchronously; the very next fetch from bank $80 will be fast.
    lda #$01
    sta f:$00420D

    ; Marker #1 — MEMSEL enabled, still executing in slow bank.
    lda #$BB
    sta a:$0001

    ; JML to the fast-bank mirror of fast_entry. CA65 + the LoROM mapper
    ; would emit a jump into bank $00; override the bank byte to $80 so
    ; the next fetch comes from the fast window.
    .byte $5C                       ; JMP absolute long opcode
    .word fast_entry                ; 16-bit target (same offset; mirrored)
    .byte $80                       ; bank override — land in $80:xxxx

fast_entry:
    ; Marker #2 — reached the fast-bank mirror of this ROM.
    lda #$CC
    sta a:$0002

    ; Exercise a short sequence in the fast bank so timing matters — this
    ; isn't asserted directly, but slow-bank execution would have used more
    ; cycles to reach fast_loop, and an undersized budget would trip it.
    ldx #$05
fast_count_down:
    dex
    bne fast_count_down

    ; Marker #3 — finished the fast-bank inner loop.
    lda #$DD
    sta a:$0003

fast_loop:
    bra fast_loop

.segment "HEADER"
    .byte "PUPSNES FASTROM SMOKE"
    PUPSNES_LOROM_HEADER_BYTES

PUPSNES_RESET_VECTORS start
