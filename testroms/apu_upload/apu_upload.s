.include "pupsnes.inc"

.segment "CODE"

; Boot and upload through the real IPL protocol documented in Anomie's
; SPC700 reference: https://github.com/gilligan/snesdev/blob/master/docs/spc700.txt
; No APU memory or CPU registers are initialized by the host test harness.
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

    ; Request a second block. The last counter was $0F; $11 is the
    ; required counter + 2 end-of-transfer token, skipping expected $10.
    lda #$00
    sta $2142
    lda #$04
    sta $2143
    lda #$01
    sta $2141
    lda #$11
    sta $2140
wait_second_block:
    cmp $2140
    bne wait_second_block

    ldx #$0000
upload_second_byte:
    lda second_block,x
    sta $2141
    txa
    sta $2140
wait_second_byte_ack:
    cmp $2140
    bne wait_second_byte_ack
    inx
    cpx #(second_block_end - second_block)
    bne upload_second_byte

    ; Mode zero starts the responder at $0200 after the second block.
    ; Its last counter was $03, so the launch token is $05.
    lda #$00
    sta $2141
    sta $2142
    lda #$02
    sta $2143
    lda #$05
    sta $2140
wait_launch_ack:
    cmp $2140
    bne wait_launch_ack

wait_program_ready:
    lda $2141
    cmp #$5A
    bne wait_program_ready
    lda #$41
    sta $2142
    lda #$77
    sta $2140

wait_reply:
    lda $2141
    cmp #$A5
    bne wait_reply
    lda $2143
    cmp #$42
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

    ; Keep the upload at ROM offset $0100 so the integration test can
    ; compare every uploaded byte against the assembled ROM image.
    .res $0100 - (* - start), $EA
spc_program:
    .byte $8F, $5A, $F5      ; MOV $F5,#$5A: announce program entry
    .byte $E4, $F4           ; MOV A,$F4
    .byte $68, $77           ; CMP A,#$77: wait for command-ready token
    .byte $D0, $FA           ; BNE back to MOV A,$F4
    .byte $E4, $F6           ; MOV A,$F6: receive command $41
    .byte $BC                ; INC A: compute reply $42
    .byte $C4, $10           ; MOV $10,A: leave computation evidence in ARAM
    .byte $C4, $F7           ; MOV $F7,A: send computed reply on another port
    .byte $8F, $A5, $F5      ; MOV $F5,#$A5: reply is ready
    .byte $FF                ; STOP
    ; Exercise the IPL's Y wrap and destination-page increment at $02FF.
    .res $010F - (* - spc_program), $A6
    .byte $3C
spc_program_end:
second_block:
    .byte $12, $34, $56, $78
second_block_end:

.segment "HEADER"
    .byte "PUPSNES APU UPLOAD", $00, $00, $00
    PUPSNES_LOROM_HEADER_BYTES

PUPSNES_RESET_VECTORS start
