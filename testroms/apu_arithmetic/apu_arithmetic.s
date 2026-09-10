.include "pupsnes.inc"

; ca65 assembles the 65C816 uploader; SPC700 opcodes are explicit below.
; Primary opcode/flag references:
; https://github.com/ares-emulator/ares/blob/master/ares/component/processor/spc700/instruction.cpp
; https://github.com/ares-emulator/ares/blob/master/ares/component/processor/spc700/algorithms.cpp
; https://github.com/ares-emulator/ares/blob/master/ares/component/processor/spc700/instructions.cpp
.macro SPC_PSW Value
    .byte $E8, Value        ; MOV A,#Value
    .byte $2D, $8E          ; PUSH A / POP PSW: establish flags through execution
.endmacro
.macro SPC_RESULT Index
    .byte $C5              ; MOV !($1000+3*Index),A
    .word $1000 + 3*Index
    .byte $CC              ; MOV !($1001+3*Index),Y
    .word $1001 + 3*Index
    .byte $0D, $AE          ; PUSH PSW / POP A: preserve the arithmetic flags
    .byte $C5
    .word $1002 + 3*Index
.endmacro
.macro SPC_MEMORY_RESULT Index, Address
    .byte $0D              ; Capture flags before the result load changes N/Z.
    .byte $E5
    .word Address
    .byte $C5
    .word $1000 + 3*Index
    .byte $CC
    .word $1001 + 3*Index
    .byte $AE
    .byte $C5
    .word $1002 + 3*Index
.endmacro
.macro SPC_WORD_MEMORY_RESULT Index, Address
    .byte $0D              ; Preserve RMW flags before MOVW changes N/Z.
    .byte $BA, Address
    .byte $C5
    .word $1000 + 3*Index
    .byte $CC
    .word $1001 + 3*Index
    .byte $AE
    .byte $C5
    .word $1002 + 3*Index
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

    ; Three pages uploaded: last counter $FF, so launch with $01.
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
    cmp #$62
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

    ; Stable offset for exact verification of all uploaded instructions.
    .res $0100 - (* - start), $EA
spc_program:
    .byte $8F, $00, $F1      ; MOV $F1,#0: IPL off, timers remain disabled

    ; Carry/borrow chain. Snapshot stores do not change flags; reload only
    ; the previous result because capturing PSW uses A as a temporary.
    SPC_PSW $00
    .byte $8D, $00           ; MOV Y,#0
    .byte $E8, $7F           ; MOV A,#$7F
    .byte $88, $01           ; ADC A,#1 -> $80, N/V/H
    SPC_RESULT 0
    .byte $E5, $00, $10      ; MOV A,!$1000
    .byte $88, $80           ; ADC A,#$80 -> $00, Z/V/C
    SPC_RESULT 1
    .byte $E5, $03, $10      ; MOV A,!$1003
    .byte $A8, $01           ; SBC A,#1 -> $FF, borrow
    SPC_RESULT 2
    .byte $8F, $01, $40      ; MOV $40,#1
    .byte $E5, $06, $10      ; MOV A,!$1006
    .byte $84, $40           ; ADC A,$40 -> $00, Z/H/C
    SPC_RESULT 3

    ; Arithmetic writes memory, then byte INC/DEC cross zero. The flag
    ; snapshots precede any load of the modified memory value.
    SPC_PSW $00
    .byte $8F, $FF, $41      ; MOV $41,#$FF
    .byte $89, $40, $41      ; ADC $41,$40 -> $00
    SPC_MEMORY_RESULT 4, $0041
    .byte $B8, $01, $41      ; SBC $41,#1 -> $FF, borrow
    SPC_MEMORY_RESULT 5, $0041
    .byte $AB, $41           ; INC $41 -> $00
    SPC_MEMORY_RESULT 6, $0041
    .byte $8C, $41, $00      ; DEC !$0041 -> $FF
    SPC_MEMORY_RESULT 7, $0041

    ; Word read/modify/write wraps the operand's high byte within page 1.
    ; Capture INCW/DECW flags before MOVW reloads the resulting word.
    SPC_PSW $20
    .byte $8F, $FF, $FF      ; MOV $FF,#$FF -> $01FF
    .byte $8F, $7F, $00      ; MOV $00,#$7F -> $0100
    .byte $3A, $FF           ; INCW $FF: $7FFF -> $8000
    SPC_WORD_MEMORY_RESULT 8, $FF
    .byte $1A, $FF           ; DECW $FF: $8000 -> $7FFF
    SPC_WORD_MEMORY_RESULT 9, $FF

    ; ADDW/SUBW use their own carry input and report half carry at bit 12.
    SPC_PSW $01             ; ADDW must ignore this set carry
    .byte $8F, $01, $50      ; MOV $50,#1
    .byte $8F, $00, $51      ; MOV $51,#0
    .byte $8D, $7F           ; MOV Y,#$7F
    .byte $E8, $FF           ; MOV A,#$FF
    .byte $7A, $50           ; ADDW YA,$50 -> $8000, N/V/H
    SPC_RESULT 10
    .byte $E5, $1E, $10      ; MOV A,!$101E
    .byte $EC, $1F, $10      ; MOV Y,!$101F
    .byte $9A, $50           ; SUBW YA,$50 -> $7FFF, V/C
    SPC_RESULT 11
    .byte $E5, $21, $10      ; MOV A,!$1021
    .byte $EC, $22, $10      ; MOV Y,!$1022
    .byte $5A, $50           ; CMPW YA,$50: greater, YA remains $7FFF
    SPC_RESULT 12
    SPC_PSW $48             ; V/H are preserved by comparisons
    .byte $8D, $00           ; MOV Y,#0
    .byte $E8, $01           ; MOV A,#1
    .byte $5A, $50           ; CMPW YA,$50: equal, Z/C set
    SPC_RESULT 13

    ; Byte register and memory comparisons preserve their operands and V/H.
    SPC_PSW $48
    .byte $CD, $00           ; MOV X,#0
    .byte $8D, $00           ; MOV Y,#0
    .byte $E8, $5A           ; MOV A,#$5A
    .byte $C8, $01           ; CMP X,#1: negative with borrow
    SPC_RESULT 14
    .byte $8D, $3B           ; MOV Y,#$3B
    .byte $E8, $5A           ; MOV A,#$5A
    .byte $5E, $40, $00      ; CMP Y,!$0040: greater
    SPC_RESULT 15
    .byte $69, $40, $41      ; CMP $41,$40: $FF-$01, no memory write
    SPC_MEMORY_RESULT 16, $0041

    ; Decimal adjustments consume the H/C produced by binary ADC/SBC.
    SPC_PSW $00
    .byte $8D, $00           ; MOV Y,#0
    .byte $E8, $45           ; MOV A,#$45
    .byte $88, $38           ; ADC A,#$38
    .byte $DF                ; DAA: decimal 45+38 = 83
    SPC_RESULT 17
    SPC_PSW $00
    .byte $E8, $99           ; MOV A,#$99
    .byte $88, $01           ; ADC A,#1
    .byte $DF                ; DAA: decimal 99+1 = 00 with carry
    SPC_RESULT 18
    SPC_PSW $00
    .byte $E8, $09           ; MOV A,#9
    .byte $88, $09           ; ADC A,#9 -> $12 with H=1
    .byte $DF                ; DAA must use H even though low digit is only 2
    SPC_RESULT 25            ; Decimal 9+9 = 18, H preserved
    SPC_PSW $01
    .byte $E8, $00           ; MOV A,#0
    .byte $A8, $01           ; SBC A,#1
    .byte $BE                ; DAS: decimal 00-1 = 99 with borrow
    SPC_RESULT 19
    SPC_PSW $01
    .byte $E8, $10           ; MOV A,#$10
    .byte $A8, $01           ; SBC A,#1
    .byte $BE                ; DAS: decimal 10-1 = 09 without borrow
    SPC_RESULT 20

    ; A nonzero, negative low product still produces Z=1/N=0 when Y=0.
    SPC_PSW $49
    .byte $8D, $0F           ; MOV Y,#$0F
    .byte $E8, $10           ; MOV A,#$10
    .byte $CF                ; MUL YA -> $00F0
    SPC_RESULT 26

    ; Product/remainder chain: $12*$10 = $0120, then $0120/3 = $60 r0.
    ; MUL preserves V/H/C and derives N/Z from the product's high byte.
    SPC_PSW $49
    .byte $8D, $10           ; MOV Y,#$10
    .byte $E8, $12           ; MOV A,#$12
    .byte $CF                ; MUL YA
    SPC_RESULT 21
    .byte $E5, $3F, $10      ; MOV A,!$103F
    .byte $EC, $40, $10      ; MOV Y,!$1040
    .byte $CD, $03           ; MOV X,#3
    .byte $9E                ; DIV YA,X
    SPC_RESULT 22

    ; DIV also has a nine-bit quotient case and a distinct overflow path.
    SPC_PSW $01
    .byte $8D, $12           ; MOV Y,#$12
    .byte $E8, $34           ; MOV A,#$34
    .byte $CD, $10           ; MOV X,#$10
    .byte $9E                ; $1234/$10 -> A=$23, Y=$04, V/H/C
    SPC_RESULT 23
    SPC_PSW $01
    .byte $8D, $30           ; MOV Y,#$30
    .byte $E8, $00           ; MOV A,#0
    .byte $CD, $10           ; MOV X,#$10
    .byte $9E                ; alternate path -> A=$EE, Y=$20, N/V/H/C
    SPC_RESULT 24

    ; Fixed arithmetic position for the negative test: changing ADC to SBC
    ; must alter the CPU-observed answer, while all earlier results survive.
    .res $02E0 - (* - spc_program), $00
spc_response:
    SPC_PSW $00
    .byte $E5, $42, $10      ; MOV A,!$1042: quotient $60 from case 22
    .byte $88, $02           ; ADC A,#2 -> $62 (SBC replacement gives $5D)
    .byte $C4, $F7           ; MOV $F7,A
    .byte $8F, $A5, $F5      ; MOV $F5,#$A5: response ready
    .byte $FF                ; STOP, PC=$04EF
    .res $0300 - (* - spc_program), $A6
spc_program_end:

.segment "HEADER"
    .byte "PUPSNES APU MATH ROM", $00
    PUPSNES_LOROM_HEADER_BYTES

PUPSNES_RESET_VECTORS start
