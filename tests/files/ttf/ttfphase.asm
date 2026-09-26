; This file is part of the dosbox-automation Project.
; License: GPL-2.0-or-later. Contact: dosbox-automation-project@trinity2k.net
;
; ttfphase.com - switch between 25 and 50 text lines at a chosen point in the frame.
;
; Usage:  ttfphase       switch late in the frame, around display line 300
;         ttfphase e     switch at the top of the frame
;
; Test fixture for tests/integration/test_ttf_font_check.py. A line-count
; change schedules the VGA resize half a frame later. A switch late in the
; frame lets a frame end pass while the resize is pending, which used to
; make TTF's screen font check compare the new font at the old character
; height and report a false alteration (ada-rsn9). Eight round trips of
; INT 10h AX=1112h (8x8 ROM font, 50 lines) and AX=0003h (mode 3).
;
; Build (reproducible, no build-system rule, like the Y: tools):
;   nasm -f bin ttfphase.asm -o ttfphase.com

cpu 8086
org 0x100

ROUND_TRIPS     equ 8
LATE_LINE       equ 300
SETTLE_FRAMES   equ 5
INPUT_STATUS    equ 0x3DA

start:
        mov     byte [early], 0
        mov     si, 0x81        ; PSP command tail
.scan:
        lodsb
        cmp     al, ' '
        je      .scan
        or      al, 0x20        ; fold to lower case
        cmp     al, 'e'
        jne     .go
        mov     byte [early], 1
.go:
        mov     dx, INPUT_STATUS
        mov     di, ROUND_TRIPS
.trip:
        call    sync
        mov     ax, 0x1112      ; load 8x8 ROM font into block 0: 50 lines
        xor     bl, bl
        int     0x10
        call    settle
        call    sync
        mov     ax, 0x0003      ; mode 3: 25 lines, 8x16 font
        int     0x10
        call    settle
        dec     di
        jnz     .trip
        mov     ah, 0x09        ; the test waits for this line
        mov     dx, done_msg
        int     0x21
        int     0x20

; Wait for the frame top, then for LATE_LINE displayed lines unless early.
sync:
        mov     dx, INPUT_STATUS
        call    frame_top
        cmp     byte [early], 0
        jne     .done
        mov     cx, LATE_LINE
.line:
        in      al, dx          ; bit 0 clear: inside a displayed line
        test    al, 1
        jnz     .line
.blank:
        in      al, dx          ; bit 0 set: that line ended
        test    al, 1
        jz      .blank
        loop    .line
.done:
        ret

; Returns as vertical retrace ends (bit 3 falls).
frame_top:
.in_display:
        in      al, dx
        test    al, 8
        jz      .in_display
.in_retrace:
        in      al, dx
        test    al, 8
        jnz     .in_retrace
        ret

settle:
        mov     dx, INPUT_STATUS
        mov     bx, SETTLE_FRAMES
.frame:
        call    frame_top
        dec     bx
        jnz     .frame
        ret

early:  db      0
done_msg: db    'ttfphase: 8 round trips', 13, 10, '$'
