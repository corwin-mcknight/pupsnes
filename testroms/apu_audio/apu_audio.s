.include "pupsnes.inc"

; An original looping BRR tone uploaded through the real IPL handshake.
; Pitch $0400 advances one decoded sample every four native DSP samples,
; so this 16-sample waveform repeats at 500 Hz.
.macro SPC_RAM Address, Value
    .byte $E8, Value
    .byte $C5
    .word Address
.endmacro
.macro SPC_DSP Address, Value
    .byte $8F, Address, $F2
    .byte $8F, Value, $F3
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
    .byte $8F, $00, $F1      ; IPL off, timers disabled.
    SPC_RAM $0800, $00      ; Directory source 0 starts and loops at $0900.
    SPC_RAM $0801, $09
    SPC_RAM $0802, $00
    SPC_RAM $0803, $09
    SPC_RAM $0900, $C3      ; Range 12, filter 0, loop and end flags.
    SPC_RAM $0901, $03
    SPC_RAM $0902, $57
    SPC_RAM $0903, $75
    SPC_RAM $0904, $30
    SPC_RAM $0905, $0D
    SPC_RAM $0906, $B9
    SPC_RAM $0907, $9B
    SPC_RAM $0908, $D0
    SPC_DSP $6C, $20        ; Unmute, echo writes disabled.
    SPC_DSP $0C, $7F
    SPC_DSP $1C, $7F
    SPC_DSP $5D, $08        ; Directory page.
    SPC_DSP $00, $50        ; Different voice volumes exercise stereo.
    SPC_DSP $01, $30
    SPC_DSP $02, $00
    SPC_DSP $03, $04        ; Pitch $0400, quarter sample per DSP frame.
    SPC_DSP $04, $00
    SPC_DSP $05, $00        ; Direct GAIN, no ADSR.
    SPC_DSP $07, $7F
    SPC_DSP $5C, $00
    SPC_DSP $7C, $00
    .res $00F0 - (* - spc_program), $00
spc_key_on:
    SPC_DSP $4C, $01        ; Negative test replaces this KON value with zero.
    .byte $8F, $38, $F7
    .byte $8F, $A5, $F5
    .byte $FF              ; The DSP keeps playing after SPC700 STOP.
    .res $0200 - (* - spc_program), $A6
spc_program_end:

.segment "HEADER"
    .byte "PUPSNES APU AUDIO", $00, $00, $00, $00
    PUPSNES_LOROM_HEADER_BYTES

PUPSNES_RESET_VECTORS start
