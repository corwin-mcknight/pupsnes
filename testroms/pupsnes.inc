.macro PUPSNES_LOROM_HEADER_BYTES
    .byte $20
    .byte $00
    .byte $08
    .byte $00
    .byte $00
    .byte $00
    .byte $00
    .word $0000
    .word $0000
.endmacro

.macro PUPSNES_RESET_VECTORS ResetLabel
    .segment "VECTORS"
        .res 28, $00
        .addr ResetLabel
        .addr $0000
.endmacro
