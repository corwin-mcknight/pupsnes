.include "pupsnes.inc"

; ca65 assembles the 65C816 uploader. SPC700 instruction bytes are explicit.
; Reference: ares/component/processor/spc700/instruction.cpp and instructions.cpp.
.macro SPC_ADDRESS Target
    .word $0200 + Target - spc_program
.endmacro
.macro SPC_BRANCH Opcode, Target
    .byte Opcode
    .byte (Target - (* + 1)) & $FF
.endmacro
.macro SPC_JUMP Target
    .byte $5F
    SPC_ADDRESS Target
.endmacro
.macro SPC_PSW Value
    .byte $E8, Value, $2D, $8E ; MOV A,#value / PUSH A / POP PSW
.endmacro
.macro SPC_FLAG_BRANCH Opcode, TakenFlags, OtherFlags
    .local taken, bad, done
    SPC_PSW TakenFlags
    SPC_BRANCH Opcode, taken
    SPC_JUMP spc_fail
taken:
    .byte $AB, $60           ; Count successful taken branches.
    SPC_PSW OtherFlags
    SPC_BRANCH Opcode, bad
    SPC_BRANCH $2F, done
bad:
    SPC_JUMP spc_fail
done:
.endmacro
.macro SPC_BIT_BRANCH Bit
    .local set_ok, clear_ok
    .byte $8F, $00, $66      ; MOV $66,#0
    .byte $02 + Bit*$20, $66 ; SET1 $66.bit
    .byte $03 + Bit*$20, $66 ; BBS $66.bit,set_ok
    .byte (set_ok - (* + 1)) & $FF
    SPC_JUMP spc_fail
set_ok:
    .byte $AB, $67
    .byte $12 + Bit*$20, $66 ; CLR1 $66.bit
    .byte $13 + Bit*$20, $66 ; BBC $66.bit,clear_ok
    .byte (clear_ok - (* + 1)) & $FF
    SPC_JUMP spc_fail
clear_ok:
    .byte $AB, $67
.endmacro
.macro SPC_COPY Direct, Index
    .byte $E4, Direct
    .byte $C5
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
    cmp #$92
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

    .res $0100 - (* - start), $EA
spc_program:
    .byte $8F, $00, $F1      ; Disable IPL; timers remain off.
    .repeat 11, I
        .byte $8F, $00, $60 + I
    .endrepeat

    ; Install all sixteen TCALL vectors into RAM below the disabled IPL.
    .byte $CD, $00
vector_loop:
    .byte $E8, <($0200 + table_sub - spc_program)
    .byte $D5, $C0, $FF, $3D
    .byte $E8, >($0200 + table_sub - spc_program)
    .byte $D5, $C0, $FF, $3D
    .byte $C8, $20
    SPC_BRANCH $D0, vector_loop

    ; PCALL's high-page routine is executable RAM: INC $63 / RET.
    .byte $E8, $AB, $C5, $00, $FF
    .byte $E8, $63, $C5, $01, $FF
    .byte $E8, $6F, $C5, $02, $FF
    .byte $3F
    SPC_ADDRESS main_sub
    .repeat 16, I
        .byte $01 + I*$10
    .endrepeat

    ; TCALL0 and BRK share $FFDE. Replace that vector only after all TCALLs.
    .byte $E8, <($0200 + interrupt_sub - spc_program), $C5, $DE, $FF
    .byte $E8, >($0200 + interrupt_sub - spc_program), $C5, $DF, $FF
    SPC_PSW $25             ; P/I/C set, B clear, N/Z clear.
    .byte $0F               ; BRK pushes old PSW; entry sets B and clears I.
    .byte $0D, $AE, $C5, $0F, $10 ; Save PSW restored by RETI: $25.
    .byte $20               ; CLRP

    SPC_FLAG_BRANCH $10, $00, $80 ; BPL
    SPC_FLAG_BRANCH $30, $80, $00 ; BMI
    SPC_FLAG_BRANCH $50, $00, $40 ; BVC
    SPC_FLAG_BRANCH $70, $40, $00 ; BVS
    SPC_FLAG_BRANCH $90, $00, $01 ; BCC
    SPC_FLAG_BRANCH $B0, $01, $00 ; BCS
    SPC_FLAG_BRANCH $D0, $00, $02 ; BNE
    SPC_FLAG_BRANCH $F0, $02, $00 ; BEQ
    .repeat 8, Bit
        SPC_BIT_BRANCH Bit
    .endrepeat

    .byte $E8, $03
cbne_loop:
    .byte $AB, $68
    .byte $2E, $68           ; CBNE $68,cbne_loop; A is preserved.
    .byte (cbne_loop - (* + 1)) & $FF
    .byte $CD, $01, $8F, $00, $00, $E8, $05
    .byte $DE, $FF           ; CBNE $FF+X,indexed_ok wraps to $0000.
    .byte (indexed_ok - (* + 1)) & $FF
    SPC_JUMP spc_fail
indexed_ok:
    .byte $AB, $69
    .byte $8F, $03, $6A, $CD, $00
dbnz_memory:
    .byte $3D, $6E, $6A
    .byte (dbnz_memory - (* + 1)) & $FF
    .byte $C9, $02, $10      ; Three loop iterations.
    .byte $8D, $04, $CD, $00
dbnz_y:
    .byte $3D
    SPC_BRANCH $FE, dbnz_y
    .byte $C9, $03, $10      ; Four loop iterations.

    ; Logical and shift chain: 3C|03 -> 3F; &0F -> 0F; ^05 -> 0A;
    ; XCN -> A0; LSR/ASL -> A0; ROL/ROR with carry round trip -> A0.
    .byte $E8, $3C, $08, $03, $28, $0F, $48, $05, $9F
    .byte $5C, $1C, $60, $3C, $7C, $C5, $08, $10
    .byte $E8, $55, $C5, $00, $11
    .byte $EA, $00, $11      ; NOT1 $1100.0: $55 -> $54.
    .byte $60, $0A, $00, $51 ; CLRC / OR1 C,$1100.2 -> 1.
    .byte $8A, $00, $91      ; EOR1 C,$1100.4 -> 0.
    .byte $2A, $00, $11      ; OR1 C,/$1100.0 -> 1.
    .byte $4A, $00, $51      ; AND1 C,$1100.2 -> 1.
    .byte $6A, $00, $11      ; AND1 C,/$1100.0 -> 1.
    .byte $CA, $00, $F1      ; MOV1 $1100.7,C: $54 -> $D4.
    .byte $E8, $0F, $0E, $00, $11 ; TSET1 $1100 -> $DF.
    .byte $E8, $03, $4E, $00, $11 ; TCLR1 $1100 -> $DC.
    .byte $E5, $00, $11, $C5, $09, $10
    .byte $ED, $E0, $A0, $C0 ; NOTC, CLRV, EI, DI.

    SPC_COPY $60, $00
    SPC_COPY $67, $01
    SPC_COPY $61, $04
    SPC_COPY $62, $05
    SPC_COPY $63, $06
    SPC_COPY $64, $07
    SPC_COPY $65, $0A
    SPC_COPY $68, $0B
    SPC_COPY $69, $0C
    .byte $9D, $C9, $0D, $10 ; Record restored SP=$EF.
    .byte $8F, <($0200 + spc_response - spc_program), $70
    .byte $8F, >($0200 + spc_response - spc_program), $71
    .byte $CD, $00, $1F, $70, $00 ; JMP [$0070+X]

main_sub:
    .byte $AB, $61, $3F
    SPC_ADDRESS nested_sub
    .byte $6F
nested_sub:
    .byte $AB, $65, $4F, $00, $6F
table_sub:
    .byte $AB, $62, $6F
interrupt_sub:
    .byte $0D, $AE, $C5, $0E, $10 ; Entry PSW=$31 (P/B/C).
    .byte $20, $AC, $64, $00, $7F ; CLRP / INC !$0064 / RETI
spc_fail:
    .byte $8F, $EE, $F7, $8F, $A5, $F5, $FF

    .res $03C0 - (* - spc_program), $00
spc_response:
    .byte $60, $E5, $00, $10
    .repeat 7, I
        .byte $85
        .word $1001 + I
    .endrepeat
    .repeat 3, I
        .byte $85
        .word $100A + I
    .endrepeat
    .byte $48, $A5           ; Computed sum $37 XOR $A5 = $92.
    .byte $C4, $F7, $8F, $A5, $F5
spc_halt:
    .byte $EF               ; SLEEP; tests also run a STOP replacement.
    .res $0400 - (* - spc_program), $00
spc_program_end:

.segment "HEADER"
    .byte "PUPSNES APU FLOW ROM", $00
    PUPSNES_LOROM_HEADER_BYTES

PUPSNES_RESET_VECTORS start
