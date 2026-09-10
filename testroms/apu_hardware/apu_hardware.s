.include "pupsnes.inc"

; ca65 assembles the 65C816 uploader; SPC700 bytes below use the hardware
; register protocol documented by Anomie and implemented by ares:
; https://raw.githubusercontent.com/gilligan/snesdev/master/docs/spc700.txt
; https://github.com/ares-emulator/ares/blob/master/ares/sfc/smp/io.cpp
.macro SPC_SAVE Index
    .byte $C5
    .word $1000 + Index
.endmacro
.macro SPC_BRANCH Opcode, Target
    .byte Opcode, <(Target - (* + 1))
.endmacro
.macro SPC_JUMP Target
    .byte $5F
    .word $0200 + (Target - spc_program)
.endmacro
.macro SPC_TIMER Address, DisableMask, Index
    .local poll, ready, cleared
    .byte $8D, $FF          ; MOV Y,#$FF: bounded even if a timer never fires
poll:
    .byte $E4, Address      ; MOV A,timer output: clear its four-bit latch
    SPC_BRANCH $D0, ready
    SPC_BRANCH $FE, poll    ; DBNZ Y,poll
    SPC_JUMP spc_fail
ready:
    SPC_SAVE Index
    .byte $68, $01          ; First count must be exactly one.
    SPC_BRANCH $D0, spc_fail
    .byte $8F, DisableMask, $F1
    .byte $E4, Address      ; Disabled timer retains its cleared output.
    SPC_SAVE Index + 1
    .byte $68, $00
    SPC_BRANCH $F0, cleared
    SPC_JUMP spc_fail
cleared:
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
    lda #$AA
    jsr wait_port0
    lda #$BB
    jsr wait_port1
    lda #$00
    sta $2142
    lda #$02
    sta $2143
    lda #$01
    sta $2141
    lda #$CC
    sta $2140
    jsr wait_port0
    ldx #$0000
upload_byte:
    lda spc_program,x
    sta $2141
    txa
    sta $2140
    jsr wait_port0
    inx
    cpx #(spc_program_end - spc_program)
    bne upload_byte

    ; Two full pages: counter wrapped through $FF, then launch with $01.
    lda #$00
    sta $2141
    sta $2142
    lda #$02
    sta $2143
    lda #$01
    sta $2140
    jsr wait_port0
    lda #$A5
    jsr wait_port1
    lda $2143
    cmp #$38
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

    ; Every hardware wait has a CPU-side timeout, including IPL upload.
wait_port0:
    ldy #$1000
poll_port0:
    cmp $2140
    beq port0_ready
    dey
    bne poll_port0
    jmp fail
port0_ready:
    rts
wait_port1:
    ldy #$1000
poll_port1:
    cmp $2141
    beq port1_ready
    dey
    bne poll_port1
    jmp fail
port1_ready:
    rts

    .res $0100 - (* - start), $EA
spc_program:
    .byte $8F, $00, $F1      ; IPL off; timers disabled.
    .byte $8F, $6C, $F2
    .byte $8F, $60, $F3      ; FLG: mute and inhibit echo writes.
    .byte $8F, $0C, $F2
    .byte $8F, $35, $F3      ; MVOLL = $35
    .byte $E4, $F3
    SPC_SAVE 0
    .byte $8F, $8C, $F2      ; High address bit mirrors DSP reads.
    .byte $E4, $F3
    SPC_SAVE 1
    .byte $8F, $A9, $F3      ; High address bit suppresses DSP writes.
    .byte $E4, $F3
    SPC_SAVE 2
    .byte $E4, $F2
    SPC_SAVE 3              ; The address latch itself retains bit 7.
    .byte $8F, $00, $F2
    .byte $8F, $5A, $F3      ; Voice 0 VOLL is independent of MVOLL.
    .byte $E4, $F3
    SPC_SAVE 4
    .byte $8F, $7C, $F2
    .byte $8F, $FF, $F3      ; Any ENDX write clears all its bits.
    .byte $E4, $F3
    SPC_SAVE 5
    .byte $8F, $6C, $F2
    .byte $E4, $F3
    SPC_SAVE 6

    ; T0/T1 tick every 128 SPC clocks; T2 ticks every 16. Targets 2/3/16
    ; yield first outputs in 129..256 / 257..384 / 241..256 clocks from
    ; enable, regardless of the shared divider's existing phase. Polling
    ; costs at most 11 clocks per attempt; each completed timer is disabled
    ; before its next output. T2 is still read before its second output.
    .byte $8F, $02, $FA
    .byte $8F, $03, $FB
    .byte $8F, $10, $FC
    .byte $E4, $FA          ; Targets are write-only.
    SPC_SAVE 13
    .byte $E4, $FB
    SPC_SAVE 14
    .byte $E4, $FC
    SPC_SAVE 15

    ; Fixed byte offset for the negative test: disabling timer 0 must
    ; exercise the timeout and fail instead of fabricating a ready reply.
    .res $0100 - (* - spc_program), $00
spc_enable:
    .byte $8F, $07, $F1
    SPC_TIMER $FD, $06, 7
    SPC_TIMER $FE, $04, 9
    SPC_TIMER $FF, $00, 11
    .byte $8F, $0F, $FD      ; Writes do not set the timer output latch.
    .byte $E4, $FD
    SPC_SAVE 16
    .byte $E8, $A5
    SPC_SAVE 17
    SPC_JUMP spc_response
spc_fail:
    .byte $8F, $00, $F1
    .byte $E8, $E0
    SPC_SAVE 17
    .byte $8F, $00, $F7
    .byte $8F, $A5, $F5
    .byte $FF

    .res $01E0 - (* - spc_program), $00
spc_response:
    .byte $E5, $00, $10      ; $35 DSP readback plus three observed ticks.
    .byte $60                ; CLRC
    .byte $85, $07, $10
    .byte $85, $09, $10
    .byte $85, $0B, $10      ; Computed reply $38.
    .byte $C4, $F7
    .byte $8F, $A5, $F5
    .byte $FF                ; STOP, PC=$03F3
    .res $0200 - (* - spc_program), $A6
spc_program_end:

.segment "HEADER"
    .byte "PUPSNES APU HW ROM", $00, $00, $00
    PUPSNES_LOROM_HEADER_BYTES

PUPSNES_RESET_VECTORS start
