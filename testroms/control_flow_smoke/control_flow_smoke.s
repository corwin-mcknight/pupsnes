.include "pupsnes.inc"

.segment "CODE"

start:
    lda #$7E
    pha
    plb                 ; DBR = $7E

    ; Simple ALU chain: A=5, ADC#3, STA; CMP#$08 -> Z=1 -> BEQ taken
    lda #$05
    adc #$03            ; C was 0 at reset; A = $08
    sta a:$0000         ; wram[0]=$08

    cmp #$08            ; Z=1
    beq target_a
    lda #$FF            ; skipped
    sta a:$0001

target_a:
    ; JSR round trip — subroutine stores $AA and returns.
    jsr sub_store_aa
    sta a:$0002         ; after RTS, A should be $AA

    ; AND + ORA + EOR chain
    lda #$F0
    and #$0F            ; A = $00
    ora #$42            ; A = $42
    eor #$FF            ; A = $BD
    sta a:$0003

    ; CPX test with BCC (branch if less).
    ldx #$10
    cpx #$20            ; X < operand -> C = 0
    bcc cpx_less
    lda #$FF            ; skipped
    sta a:$0004
    bra done
cpx_less:
    lda #$C0
    sta a:$0004         ; wram[4]=$C0

done:
loop:
    bra loop

sub_store_aa:
    lda #$AA
    rts

.segment "HEADER"
    .byte "PUPSNES CTRL SMOKE"
    PUPSNES_LOROM_HEADER_BYTES

PUPSNES_RESET_VECTORS start
