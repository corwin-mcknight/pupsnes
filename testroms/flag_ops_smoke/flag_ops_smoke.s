.include "pupsnes.inc"

.segment "CODE"

start:
    lda #$7E
    pha
    plb                 ; DBR = $7E

    ; Switch to native mode. CLC clears C, XCE swaps C and E, so E=0 (native),
    ; and the new C takes on the pre-swap E (=1 at reset).
    clc
    xce                 ; E=0, C=1

    ; REP #$30 clears the M and X flags (only effective because E=0).
    rep #$30            ; M=0, X=0 → 16-bit A, 16-bit X
    .a16
    .i16

    ; 16-bit immediate proves M was cleared. Store writes both bytes.
    lda #$1234
    sta a:$0000         ; wram[0]=$34, wram[1]=$12

    ; 16-bit index proves X was cleared.
    ldx #$ABCD
    stx a:$0002         ; wram[2]=$CD, wram[3]=$AB

    ; SEP #$30 sets M and X back to 1 — narrow registers.
    sep #$30            ; M=1, X=1
    .a8
    .i8
    lda #$99
    sta a:$0004         ; wram[4]=$99 (only the low byte)

    ; Flip back to emulation via SEC+XCE. In emulation, M/X are forced to 1.
    sec
    xce                 ; E=1, C=0 (old E was 0)
    lda #$77
    sta a:$0005         ; wram[5]=$77

loop:
    bra loop

.segment "HEADER"
    .byte "PUPSNES FLAG OP SMOKE"
    PUPSNES_LOROM_HEADER_BYTES

PUPSNES_RESET_VECTORS start
