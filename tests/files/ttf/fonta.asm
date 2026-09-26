; This file is part of the dosbox-automation Project.
; License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
;
; fonta.com - replace glyph 'A' of font block 0 with a solid block.
;
; Test fixture for tests/integration/test_ttf_font_check.py: a real custom
; font that TTF output must detect and give way to. INT 10h AX=1100h loads
; user glyphs without a mode set, so no VGA resize is involved.
;
; Build (reproducible, no build-system rule, like the Y: tools):
;   nasm -f bin fonta.asm -o fonta.com

cpu 8086
org 0x100

        mov     ax, 0x1100      ; load user font
        mov     bp, glyph       ; ES:BP, ES = CS for a .COM
        mov     cx, 1           ; one glyph
        mov     dx, 'A'         ; starting at 'A'
        mov     bx, 0x1000      ; 16 bytes per glyph, block 0
        int     0x10
        int     0x20

glyph:  times 16 db 0xFF
