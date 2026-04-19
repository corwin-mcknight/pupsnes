.include "pupsnes.inc"

.segment "CODE"

start:
    lda #$7E            ; A9 — LDA immediate
    pha                 ; 48 — PHA
    plb                 ; AB — PLB (DBR = $7E)
    phb                 ; 8B — PHB (re-push DBR; SP settles at $01FE)

    ; ---- One-time "features" block exercising newer batches ----
    clc                 ; 18 — CLC
    sec                 ; 38 — SEC
    clv                 ; B8 — CLV

    ; ALU + store
    lda #$05            ; A9 — LDA imm
    clc
    adc #$03            ; 69 — ADC imm; A=$08
    sta a:$0010

    and #$0F            ; 29 — AND imm; A=$08
    ora #$30            ; 09 — ORA imm; A=$38
    eor #$FF            ; 49 — EOR imm; A=$C7
    sta a:$0011

    ; CMP + BEQ
    lda #$42
    cmp #$42            ; C9 — CMP imm; Z=1
    beq feat_eq         ; F0 — BEQ taken
    lda #$FF            ; skipped
    sta a:$0012
feat_eq:
    lda #$11
    sta a:$0012

    ; CPX + BCC
    ldx #$10
    cpx #$20            ; E0 — CPX imm; C=0
    bcc feat_lt         ; 90 — BCC taken
    lda #$FF
    sta a:$0013
    bra feat_done
feat_lt:
    lda #$C0
    sta a:$0013
feat_done:

    ; Transfers
    lda #$42
    tax                 ; AA — TAX; X=$42
    tay                 ; A8 — TAY; Y=$42
    stx a:$0014
    sty a:$0015

    ; JSR + RTS
    jsr sub_aa          ; 20 — JSR abs
    sta a:$0016         ; A = $AA after RTS

    ; PHP/PLP round-trip
    php                 ; 08 — PHP
    plp                 ; 28 — PLP

    ; PHX/PLX round-trip
    ldx #$33
    phx                 ; DA — PHX
    plx                 ; FA — PLX
    stx a:$0017

    ; PEA then unwind
    pea $BEEF           ; F4 — PEA
    plx                 ; low -> X
    ply                 ; 7A — PLY (high -> Y)
    stx a:$0018
    sty a:$0019

main_loop:
    nop                 ; EA — NOP
    lda #$00
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

sub_aa:
    lda #$AA
    rts                 ; 60 — RTS

.segment "HEADER"
    .byte "PUPSNES ALL INSTR   ", $00
    PUPSNES_LOROM_HEADER_BYTES

PUPSNES_RESET_VECTORS start
