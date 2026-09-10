.include "pupsnes.inc"

; SPC700 bytes are explicit because ca65 assembles the host 65C816.
; Opcode/addressing reference:
; https://github.com/ares-emulator/ares/blob/master/ares/component/processor/spc700/instruction.cpp
; https://github.com/ares-emulator/ares/blob/master/ares/component/processor/spc700/instructions.cpp
.macro SPC_SAVE Index
    .byte $C5              ; MOV !($1000+Index),A
    .word $1000 + Index
.endmacro

.segment "CODE"
start:
    sei
    cld
    clc
    xce
    rep #$10
    .i16
    .a8
    ldx #$1FFF
    txs
    lda #$00
    sta $7E0000
    sta $7E0001
    sta $7E0002
    sta $7E0003
    sta $7E0004

wait_signature:
    lda $2140
    cmp #$AA
    bne wait_signature
    lda $2141
    cmp #$BB
    bne wait_signature
    lda #$00
    sta $2142
    lda #$02
    sta $2143
    lda #$01
    sta $2141
    lda #$CC
    sta $2140
wait_upload_ready:
    cmp $2140
    bne wait_upload_ready

    ldx #$0000
upload_byte:
    lda spc_program,x
    sta $2141
    txa
    sta $2140
wait_byte_ack:
    cmp $2140
    bne wait_byte_ack
    inx
    cpx #(spc_program_end - spc_program)
    bne upload_byte

    ; Two pages uploaded: last counter $FF, so launch with $01.
    lda #$00
    sta $2141
    sta $2142
    lda #$02
    sta $2143
    lda #$01
    sta $2140
wait_launch_ack:
    cmp $2140
    bne wait_launch_ack
wait_reply:
    lda $2141
    cmp #$A5
    bne wait_reply
    lda $2143
    cmp #$23
    bne fail
    sta $7E0004
    lda #'P'
    sta $7E0000
    lda #'A'
    sta $7E0001
    lda #'S'
    sta $7E0002
    sta $7E0003
    stp
fail:
    lda #'F'
    sta $7E0000
    stp

    ; Stable ROM offset for comparing the uploaded program against ARAM.
    .res $0100 - (* - start), $EA
spc_program:
    .byte $8F, $00, $F1      ; MOV $F1,#0: disable IPL, keep timers disabled

    ; Immediate, register and absolute transfers. Results $1000-$1004;
    ; the Y<-A and X<-absolute results are also retained at $101A-$101B.
    .byte $E8, $23           ; MOV A,#$23
    .byte $5D                ; MOV X,A
    .byte $8D, $8C           ; MOV Y,#$8C
    .byte $C9, $00, $10      ; MOV !$1000,X
    .byte $CC, $01, $10      ; MOV !$1001,Y
    .byte $7D                ; MOV A,X
    SPC_SAVE 2
    .byte $FD                ; MOV Y,A
    .byte $CC, $1A, $10      ; MOV !$101A,Y
    .byte $E9, $01, $10      ; MOV X,!$1001
    .byte $C9, $1B, $10      ; MOV !$101B,X
    .byte $EC, $01, $10      ; MOV Y,!$1001
    .byte $DD                ; MOV A,Y
    SPC_SAVE 3
    .byte $C5, $00, $12      ; MOV !$1200,A
    .byte $E5, $00, $12      ; MOV A,!$1200
    SPC_SAVE 4

    ; Direct and indexed direct transfers use page 1. Index arithmetic
    ; must wrap inside that page, without touching the page-0 I/O overlay.
    .byte $40                ; SETP
    .byte $8F, $5A, $FF      ; MOV $FF,#$5A -> $01FF
    .byte $8F, $C3, $00      ; MOV $00,#$C3 -> $0100
    .byte $CD, $01           ; MOV X,#1
    .byte $F4, $FE           ; MOV A,$FE+X -> $01FF
    SPC_SAVE 5
    .byte $E8, $6B           ; MOV A,#$6B
    .byte $D4, $FF           ; MOV $FF+X,A -> $0100
    .byte $E4, $00           ; MOV A,$00
    SPC_SAVE 6
    .byte $D8, $02           ; MOV $02,X -> $0102 = 1
    .byte $EB, $FF           ; MOV Y,$FF
    .byte $CB, $03           ; MOV $03,Y -> $0103 = $5A
    .byte $F8, $00           ; MOV X,$00
    .byte $D9, $04           ; MOV $04+Y,X -> $015E = $6B
    .byte $CD, $A2           ; MOV X,#$A2
    .byte $8D, $02           ; MOV Y,#2
    .byte $D9, $FD           ; MOV $FD+Y,X -> $01FF
    .byte $F9, $FD           ; MOV X,$FD+Y
    .byte $7D                ; MOV A,X
    SPC_SAVE 7
    .byte $CD, $02           ; MOV X,#2
    .byte $8D, $B4           ; MOV Y,#$B4
    .byte $DB, $FE           ; MOV $FE+X,Y -> $0100
    .byte $FB, $FE           ; MOV Y,$FE+X
    .byte $DD                ; MOV A,Y
    SPC_SAVE 8
    .byte $FA, $00, $01      ; MOV $01,$00: source precedes destination
    .byte $E4, $01           ; MOV A,$01
    SPC_SAVE 9

    ; Both postincrement forms wrap X=$FF to $00 in selected page 1.
    .byte $CD, $FF           ; MOV X,#$FF
    .byte $E8, $D6           ; MOV A,#$D6
    .byte $AF                ; MOV (X)+,A -> $01FF, X=0
    .byte $E8, $E7           ; MOV A,#$E7
    .byte $C6                ; MOV (X),A -> $0100
    .byte $E6                ; MOV A,(X)
    SPC_SAVE 10
    .byte $CD, $FF           ; MOV X,#$FF
    .byte $BF                ; MOV A,(X)+ -> $D6, X=0
    SPC_SAVE 11
    .byte $BF                ; MOV A,(X)+ -> $E7, X=1
    SPC_SAVE 12
    .byte $7D                ; MOV A,X
    SPC_SAVE 13
    .byte $20                ; CLRP

    ; Absolute indexed accesses cross the 16-bit address boundary. Reading
    ; $FFFF here also proves that the IPL overlay was actually disabled.
    .byte $E8, $91           ; MOV A,#$91
    .byte $C5, $FF, $FF      ; MOV !$FFFF,A
    .byte $CD, $01           ; MOV X,#1
    .byte $F5, $FE, $FF      ; MOV A,!$FFFE+X
    SPC_SAVE 14
    .byte $E8, $82           ; MOV A,#$82
    .byte $D5, $FF, $FF      ; MOV !$FFFF+X,A -> $0000
    .byte $F5, $FF, $FF      ; MOV A,!$FFFF+X
    SPC_SAVE 15
    .byte $8D, $02           ; MOV Y,#2
    .byte $E8, $73           ; MOV A,#$73
    .byte $D6, $FE, $FF      ; MOV !$FFFE+Y,A -> $0000
    .byte $F6, $FE, $FF      ; MOV A,!$FFFE+Y
    SPC_SAVE 16

    ; The pointer's high byte wraps from $01FF to $0100. Indirect-Y then
    ; crosses $FFFF; indexed-indirect reaches high RAM without truncation.
    .byte $40                ; SETP
    .byte $8F, $FE, $FF      ; MOV $FF,#$FE
    .byte $8F, $FF, $00      ; MOV $00,#$FF: wrapped pointer = $FFFE
    .byte $8D, $03           ; MOV Y,#3
    .byte $E8, $64           ; MOV A,#$64
    .byte $D7, $FF           ; MOV [$FF]+Y,A -> $0001
    .byte $F7, $FF           ; MOV A,[$FF]+Y
    SPC_SAVE 17
    .byte $CD, $02           ; MOV X,#2
    .byte $E8, $55           ; MOV A,#$55
    .byte $C7, $FD           ; MOV [$FD+X],A -> $FFFE
    .byte $E7, $FD           ; MOV A,[$FD+X]
    SPC_SAVE 18
    .byte $20                ; CLRP

    ; MOV1 operands pack the bit index above the 13-bit absolute address.
    ; Start with $55, set bit 1 from bit 7, then clear bit 0 from bit 0.
    .byte $E8, $80           ; MOV A,#$80
    .byte $C5, $00, $11      ; MOV !$1100,A
    .byte $E8, $55           ; MOV A,#$55
    .byte $C5, $01, $11      ; MOV !$1101,A
    .byte $AA, $00, $F1      ; MOV1 C,!$1100.7
    .byte $CA, $01, $31      ; MOV1 !$1101.1,C
    .byte $E5, $01, $11      ; MOV A,!$1101
    SPC_SAVE 19
    .byte $AA, $00, $11      ; MOV1 C,!$1100.0
    .byte $CA, $01, $11      ; MOV1 !$1101.0,C
    .byte $E5, $01, $11      ; MOV A,!$1101
    SPC_SAVE 20

    ; All push/pop forms wrap in page 1. Restoring PSW must restore P=1;
    ; pulling A/X/Y must leave flags alone. Save the restored PSW as data.
    .byte $CD, $01           ; MOV X,#1
    .byte $BD                ; MOV SP,X
    .byte $E8, $2A           ; MOV A,#$2A
    .byte $CD, $3B           ; MOV X,#$3B
    .byte $8D, $4C           ; MOV Y,#$4C
    .byte $40                ; SETP: pushed PSW is $20
    .byte $2D                ; PUSH A
    .byte $4D                ; PUSH X
    .byte $6D                ; PUSH Y
    .byte $0D                ; PUSH PSW
    .byte $E8, $00           ; MOV A,#0
    .byte $CD, $00           ; MOV X,#0
    .byte $8D, $00           ; MOV Y,#0
    .byte $20                ; CLRP: force POP PSW to restore P
    .byte $8E                ; POP PSW
    .byte $0D                ; PUSH PSW
    .byte $AE                ; POP A: inspect saved PSW
    SPC_SAVE 21
    .byte $EE                ; POP Y
    .byte $CE                ; POP X
    .byte $AE                ; POP A
    SPC_SAVE 22
    .byte $C9, $17, $10      ; MOV !$1017,X
    .byte $CC, $18, $10      ; MOV !$1018,Y
    .byte $9D                ; MOV X,SP
    .byte $7D                ; MOV A,X
    SPC_SAVE 25
    .byte $20                ; CLRP: expose CPU ports in page 0 again

    ; Fixed response position lets the negative integration test change
    ; only this MOV's source address. NOP padding is executable SPC code.
    .res $01E0 - (* - spc_program), $00
spc_response:
    .byte $E5, $00, $10      ; MOV A,!$1000: return transferred value $23
    .byte $C4, $F7           ; MOV $F7,A
    .byte $8F, $A5, $F5      ; MOV $F5,#$A5: response ready
    .byte $FF                ; STOP, PC=$03E9
    .res $0200 - (* - spc_program), $A6
spc_program_end:

.segment "HEADER"
    .byte "PUPSNES APU MOV ROM", $00, $00
    PUPSNES_LOROM_HEADER_BYTES

PUPSNES_RESET_VECTORS start
