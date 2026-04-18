.include "pupsnes.inc"

.segment "CODE"

start:
    lda #$7E            ; A9 — LDA immediate
    pha                 ; 48 — PHA
    plb                 ; AB — PLB (DBR = $7E)
    phb                 ; 8B — PHB (re-push DBR; SP settles at $01FE)

main_loop:
    nop                 ; EA — NOP
    lda #$00            ; A = $00
    ldx #$03            ; A2 — LDX immediate (iter counter)
    ldy #$10            ; A0 — LDY immediate

iter:
    inc a               ; 1A — INC A
    iny                 ; C8 — INY
    dex                 ; CA — DEX
    bne iter            ; D0 — BNE (3 iterations: A=$03, X=$00, Y=$13)

    dec a               ; 3A — DEC A  (A = $02)
    dey                 ; 88 — DEY    (Y = $12)
    inx                 ; E8 — INX    (X = $01)

    sta a:$0000         ; 8D — STA absolute -> $7E:0000 = $02
    stx a:$0001         ; 8E — STX absolute -> $7E:0001 = $01
    sty a:$0002         ; 8C — STY absolute -> $7E:0002 = $12
    sta f:$7E0003       ; 8F — STA long     -> $7E:0003 = $02

    bra main_loop       ; 80 — BRA back to the top of the work loop

.segment "HEADER"
    .byte "PUPSNES ALL INSTR   ", $00
    PUPSNES_LOROM_HEADER_BYTES

PUPSNES_RESET_VECTORS start
