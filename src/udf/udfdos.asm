; udfdos.asm - WINUDF.EXE: start, INT 2Fh hook and interrupt helpers of the DOS UDF
; redirector (tiny model .COM image; the C code is in udfdos.c and udfinit.c)
;
; DOS calls the network redirector through INT 2Fh AH=11h with its swappable data area
; set up. The hook saves the registers in _rr, switches to its own stack and calls
; _redir; a nonzero return means the request was for a UDF drive and _rr holds the
; results (CF in rr_fl), zero passes the call on to the next handler (MSCDEX for CD
; drives without a UDF disc, other redirectors).
;
; Build: nasm -f obj -o udfdos.obj udfdos.asm

        bits 16

segment _TEXT public class=CODE align=16
segment CONST public class=DATA align=2
segment CONST2 public class=DATA align=2
segment _DATA public class=DATA align=2
segment _BSS public class=BSS align=16
segment RESEND public class=BSS align=16
segment INIT_TEXT public class=INITCODE align=16
segment INIT_DATA public class=INITDATA align=2
segment INITEND public class=INITDATA align=16
group DGROUP _TEXT CONST CONST2 _DATA _BSS RESEND INIT_TEXT INIT_DATA INITEND

extern _redir
extern _dos_init
extern _api

extern _rr
global _intx
global _old2f
global _hook2f
global _unhook2f
global _resend
global _imgend
global _farcall

MPLEX   equ 0D6h                ; WINUDF API: AX=D600h install check, D601h request

segment _TEXT
        resb 100h
..start:
start:  jmp init

; ---------------------------------------------------------------------------
; INT 2Fh
int2f:
        cmp ah, 11h
        je .redir
        cmp ah, MPLEX
        je .api
.chain: jmp far [cs:_old2f]

.api:   cmp al, 0
        jne .apicall
        mov al, 0FFh
        mov bx, 'UD'
        push cs
        pop es                          ; ES = resident segment
        iret
.apicall:
        cmp byte [cs:busy], 0
        jne .chain
        mov byte [cs:apiflag], 1
        jmp .go

.redir: cmp byte [cs:busy], 0
        jne .chain
        mov byte [cs:apiflag], 0
.go:    mov byte [cs:busy], 1
        mov [cs:_rr + 0], ax
        mov [cs:_rr + 2], bx
        mov [cs:_rr + 4], cx
        mov [cs:_rr + 6], dx
        mov [cs:_rr + 8], si
        mov [cs:_rr + 10], di
        mov [cs:_rr + 12], bp
        mov [cs:_rr + 14], ds
        mov [cs:_rr + 16], es
        mov [cs:_rr + 20], ss
        mov [cs:_rr + 22], sp
        push cs
        pop ds
        cli
        mov [saved_ss], ss
        mov [saved_sp], sp
        push cs
        pop ss
        mov sp, stack_top
        sti
        push cs
        pop es
        cld
        cmp byte [apiflag], 0
        jne .callapi
        call _redir
        jmp .back
.callapi:
        call _api
.back:  cli
        mov ss, [saved_ss]
        mov sp, [saved_sp]
        sti
        test ax, ax
        jnz .done
        ; not ours: every register as it came, to the next handler
        mov ax, [cs:_rr + 0]
        mov bx, [cs:_rr + 2]
        mov cx, [cs:_rr + 4]
        mov dx, [cs:_rr + 6]
        mov si, [cs:_rr + 8]
        mov di, [cs:_rr + 10]
        mov bp, [cs:_rr + 12]
        mov es, [cs:_rr + 16]
        mov ds, [cs:_rr + 14]
        mov byte [cs:busy], 0
        jmp far [cs:_old2f]
.done:  mov ax, [cs:_rr + 0]
        mov bx, [cs:_rr + 2]
        mov cx, [cs:_rr + 4]
        mov dx, [cs:_rr + 6]
        mov si, [cs:_rr + 8]
        mov di, [cs:_rr + 10]
        mov bp, [cs:_rr + 12]
        mov es, [cs:_rr + 16]
        mov ds, [cs:_rr + 14]
        push bp
        mov bp, sp
        and word [bp + 6], 0FFFEh       ; CF of the caller's flags
        test byte [cs:_rr + 18], 1
        jz .nc
        or word [bp + 6], 1
.nc:    pop bp
        mov byte [cs:busy], 0
        iret

; ---------------------------------------------------------------------------
; int __cdecl intx(int n, struct rregs *r): interrupt n with the registers of *r
_intx:
        push bp
        mov bp, sp
        push si
        push di
        push ds
        push es
        mov al, [bp + 4]
        mov [cs:.ins + 1], al
        jmp short .flush
.flush: mov bx, [bp + 6]
        mov [cs:xr], bx
        mov ax, [bx + 0]
        mov cx, [bx + 4]
        mov dx, [bx + 6]
        mov si, [bx + 8]
        mov di, [bx + 10]
        mov es, [bx + 16]
        push word [bx + 14]
        push word [bx + 2]
        mov bp, [bx + 12]
        pop bx
        pop ds
.ins:   int 0
        push ds
        push bx
        push cs
        pop ds
        mov bx, [xr]
        mov [bx + 0], ax
        pop word [bx + 2]
        mov [bx + 4], cx
        mov [bx + 6], dx
        mov [bx + 8], si
        mov [bx + 10], di
        mov [bx + 12], bp
        pop word [bx + 14]
        mov [bx + 16], es
        pushf
        pop word [bx + 18]
        pop es
        pop ds
        pop di
        pop si
        pop bp
        ret

; void __cdecl farcall(void __far *fn, struct rregs *r): far call with registers (device
; driver strategy / interrupt routines: ES:BX request header)
_farcall:
        push bp
        mov bp, sp
        push si
        push di
        push ds
        push es
        mov ax, [bp + 4]
        mov [cs:fcaddr], ax
        mov ax, [bp + 6]
        mov [cs:fcaddr + 2], ax
        mov bx, [bp + 8]
        mov [cs:xr], bx
        mov es, [bx + 16]
        mov bx, [bx + 2]
        call far [cs:fcaddr]
        push cs
        pop ds
        pop es
        pop ds
        pop di
        pop si
        pop bp
        ret

; void __cdecl hook2f(void), unhook2f(void)
_hook2f:
        push es
        mov ax, 352Fh
        int 21h
        mov [_old2f], bx
        mov [_old2f + 2], es
        pop es
        mov dx, int2f
        mov ax, 252Fh
        int 21h
        ret

_unhook2f:
        push ds
        lds dx, [_old2f]
        mov ax, 252Fh
        int 21h
        pop ds
        ret

; ---------------------------------------------------------------------------
segment _DATA
_old2f  dd 0
busy    db 0
apiflag db 0

segment _BSS
xr      resw 1
fcaddr  resd 1
saved_ss resw 1
saved_sp resw 1
        resb 2560
stack_top:

segment RESEND
_resend:                        ; end of the resident part

; ---------------------------------------------------------------------------
segment INIT_TEXT
init:   mov sp, init_stack
        call _dos_init          ; returns AX = paragraphs to keep, 0 = exit
        test ax, ax
        jz .exit
        mov dx, ax
        mov ax, 3100h
        int 21h
.exit:  mov ax, 4C00h | 0
        mov al, [_exitcode]
        int 21h

segment INIT_DATA
global _exitcode
_exitcode db 0

segment INITEND
        resb 1536
init_stack:
_imgend:
