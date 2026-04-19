.include "pupsnes.inc"

.segment "CODE"

start:
    lda #$7E
    pha
    plb                 ; DBR = $7E

    ; Go native and widen A/X/Y so the 16-bit transfers are observable.
    clc
    xce                 ; E=0, C=1
    rep #$30            ; M=0, X=0
    .a16
    .i16

    ; Seed A, then TAX and TAY to propagate the value.
    lda #$1234
    tax                 ; X = $1234
    tay                 ; Y = $1234
    stx a:$0000         ; wram[0]=$34, wram[1]=$12
    sty a:$0002         ; wram[2]=$34, wram[3]=$12

    ; TXY: copy X to Y after mutating X.
    lda #$BEEF
    tax                 ; X = $BEEF
    txy                 ; Y = $BEEF
    sty a:$0004         ; wram[4]=$EF, wram[5]=$BE

    ; TXA / TYA: observed via stores.
    lda #$0000
    ldx #$CAFE
    txa                 ; A = $CAFE
    sta a:$0006         ; wram[6]=$FE, wram[7]=$CA

    ldy #$ABCD
    tya                 ; A = $ABCD
    sta a:$0008         ; wram[8]=$CD, wram[9]=$AB

    ; TCD: copy 16-bit A to DP, then TDC back into A to verify.
    lda #$5678
    tcd                 ; DP = $5678
    lda #$0000
    tdc                 ; A = $5678
    sta a:$000A         ; wram[A]=$78, wram[B]=$56

    ; TCS: copy A to SP (native 16-bit), then TSC back. Also demonstrates TSX.
    lda #$1FF0
    tcs                 ; SP = $1FF0
    tsx                 ; X = $1FF0
    stx a:$000C         ; wram[C]=$F0, wram[D]=$1F
    tsc                 ; A = $1FF0
    sta a:$000E         ; wram[E]=$F0, wram[F]=$1F

    ; Restore a sane SP before returning to emulation.
    lda #$01FF
    tcs

    ; Back to emulation. TXS in emulation forces SH=$01.
    sep #$30
    .a8
    .i8
    sec
    xce                 ; E=1

loop:
    bra loop

.segment "HEADER"
    .byte "PUPSNES XFER SMOKE"
    PUPSNES_LOROM_HEADER_BYTES

PUPSNES_RESET_VECTORS start
