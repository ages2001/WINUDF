; udfdos.asm - WINUDF.EXE: start, INT 2Fh / INT 21h hooks and interrupt helpers of the DOS
; UDF redirector (small model .EXE; the C code is in udfdos.c, udflfn.c and udfinit.c)
;
; DOS calls the network redirector through INT 2Fh AH=11h with its swappable data area
; set up. The hook saves the registers in _rr, switches to its own stack and calls
; _redir; a nonzero return means the request was for a UDF drive and _rr holds the
; results (CF in rr_fl), zero passes the call on to the next handler (MSCDEX for CD
; drives without a UDF disc, other redirectors).
;
; The long file name functions of INT 21h (AH=71h) never reach a redirector: the INT 21h
; hook gives the ones for UDF drives (and the 64-bit seek AX=7142h for files on them) to
; _lfn, with the registers in _lr, on a stack of its own (_lfn may call DOS, which calls
; the redirector).
;
; Both handlers start with an IBM Interrupt Sharing Protocol header, so WINUDF /U can
; unhook them when other programs hooked the interrupts later (if they have one too).
;
; Memory: the resident code (_TEXT) comes first, then the code that only runs at the start
; (INIT_TEXT), then the data (DGROUP: the resident data up to RESEND, then the start-up
; data and stack). When the program stays resident, the resident data is moved down
; behind the resident code and the rest is given back to DOS.
;
; Build: nasm -f obj -o udfdos.obj udfdos.asm

        bits 16

segment _TEXT public class=CODE align=16
segment RESCEND public class=CODE align=16
segment INIT_TEXT public class=INITCODE align=16
segment CONST public class=DATA align=2
segment CONST2 public class=DATA align=2
segment _DATA public class=DATA align=2
segment _BSS public class=BSS align=16
segment RESEND public class=BSS align=16
segment INIT_DATA public class=INITDATA align=2
segment INITEND stack class=STACK align=16
group CGROUP _TEXT RESCEND INIT_TEXT
group DGROUP CONST CONST2 _DATA _BSS RESEND INIT_DATA INITEND

extern _redir
extern _lfn
extern _dos_init
extern _api

extern _rr:wrt DGROUP
extern _lr:wrt DGROUP
global _intx
global _old2f
global _old21
global _int2f
global _int21
global _hook2f
global _unhook2f
global _farcall
global _unhook_links
global _psp
global _lfn_on
global _ressize

MPLEX   equ 0D6h                ; WINUDF API: AX=D600h install check, D601h request

; IBM Interrupt Sharing Protocol header: the next handler in %1
%macro IISP 2
        jmp short %%skip
%1      dd 0
        dw 424Bh
        db 0
        jmp short %2
        times 7 db 0
%%skip:
%endmacro

segment _TEXT
..start:
        jmp init

dgseg   dw 0                    ; segment of the data (DGROUP)
busy    db 0
apiflag db 0
busy21  db 0
saved_ss dw 0
saved_sp dw 0
l_ss    dw 0
l_sp    dw 0
xr      dw 0
fcaddr  dd 0

reset2f:
        retf

; ---------------------------------------------------------------------------
; INT 2Fh
_int2f:
        IISP _old2f, reset2f
        cmp ah, 11h
        je .redir
        cmp ah, MPLEX
        je .api
.chain: jmp far [cs:_old2f]

.api:   cmp al, 0
        jne .apicall
        mov al, 0FFh
        mov bx, 5544h                   ; 'UD'
        push ds
        mov ds, [cs:dgseg]
        mov es, [_psp]                  ; ES = PSP of the resident program
        pop ds
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
        push ds
        mov ds, [cs:dgseg]
        mov [_rr + 0], ax
        mov [_rr + 2], bx
        mov [_rr + 4], cx
        mov [_rr + 6], dx
        mov [_rr + 8], si
        mov [_rr + 10], di
        mov [_rr + 12], bp
        pop word [_rr + 14]
        mov [_rr + 16], es
        mov [_rr + 20], ss
        mov [_rr + 22], sp
        cli
        mov [cs:saved_ss], ss
        mov [cs:saved_sp], sp
        mov ax, ds
        mov ss, ax
        mov sp, stack_top wrt DGROUP
        sti
        mov es, ax
        cld
        cmp byte [cs:apiflag], 0
        jne .callapi
        call _redir
        jmp .back
.callapi:
        call _api
.back:  cli
        mov ss, [cs:saved_ss]
        mov sp, [cs:saved_sp]
        sti
        test ax, ax
        jnz .done
        ; not ours: every register as it came, to the next handler
        mov ax, [_rr + 0]
        mov bx, [_rr + 2]
        mov cx, [_rr + 4]
        mov dx, [_rr + 6]
        mov si, [_rr + 8]
        mov di, [_rr + 10]
        mov bp, [_rr + 12]
        mov es, [_rr + 16]
        mov ds, [_rr + 14]
        mov byte [cs:busy], 0
        jmp far [cs:_old2f]
.done:  mov ax, [_rr + 0]
        mov bx, [_rr + 2]
        mov cx, [_rr + 4]
        mov dx, [_rr + 6]
        mov si, [_rr + 8]
        mov di, [_rr + 10]
        mov bp, [_rr + 12]
        mov es, [_rr + 16]
        push bp
        mov bp, sp
        and word [bp + 6], 0FFFEh       ; CF of the caller's flags
        test byte [_rr + 18], 1
        jz .nc
        or word [bp + 6], 1
.nc:    pop bp
        mov ds, [_rr + 14]
        mov byte [cs:busy], 0
        iret

; ---------------------------------------------------------------------------
; INT 21h: long file name functions (AH=71h) of UDF drives
reset21:
        retf
_int21:
        IISP _old21, reset21
        cmp ah, 71h
        je .lfn
        cmp ah, 42h                     ; seek: the high dword of the position goes
        je .lfn
        cmp ax, 7303h                   ; free space (FAT32 function of MS-DOS 7.1)
        je .lfn
.chain: jmp far [cs:_old21]
.lfn:   cmp byte [cs:busy21], 0
        jne .chain
        mov byte [cs:busy21], 1
        push ds
        mov ds, [cs:dgseg]
        cmp byte [_lfn_on], 0
        jne .on
        pop ds
        mov byte [cs:busy21], 0
        jmp .chain
.on:    mov [_lr + 0], ax
        mov [_lr + 2], bx
        mov [_lr + 4], cx
        mov [_lr + 6], dx
        mov [_lr + 8], si
        mov [_lr + 10], di
        mov [_lr + 12], bp
        pop word [_lr + 14]
        mov [_lr + 16], es
        push bp
        mov bp, sp
        mov ax, [bp + 6]                ; the caller's flags
        pop bp
        mov [_lr + 18], ax
        mov [_lr + 20], ss
        mov [_lr + 22], sp
        cli
        mov [cs:l_ss], ss
        mov [cs:l_sp], sp
        mov ax, ds
        mov ss, ax
        mov sp, lstack_top wrt DGROUP
        sti
        mov es, ax
        cld
        call _lfn
        cli
        mov ss, [cs:l_ss]
        mov sp, [cs:l_sp]
        sti
        mov ds, [cs:dgseg]
        test ax, ax
        jnz .done
        mov ax, [_lr + 0]
        mov bx, [_lr + 2]
        mov cx, [_lr + 4]
        mov dx, [_lr + 6]
        mov si, [_lr + 8]
        mov di, [_lr + 10]
        mov bp, [_lr + 12]
        mov es, [_lr + 16]
        mov ds, [_lr + 14]
        mov byte [cs:busy21], 0
        jmp far [cs:_old21]
.done:  mov ax, [_lr + 0]
        mov bx, [_lr + 2]
        mov cx, [_lr + 4]
        mov dx, [_lr + 6]
        mov si, [_lr + 8]
        mov di, [_lr + 10]
        mov bp, [_lr + 12]
        mov es, [_lr + 16]
        push bp
        mov bp, sp
        and word [bp + 6], 0FFFEh
        test byte [_lr + 18], 1
        jz .nc
        or word [bp + 6], 1
.nc:    pop bp
        mov ds, [_lr + 14]
        mov byte [cs:busy21], 0
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
        mov ds, [cs:dgseg]
        mov bx, [cs:xr]
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
        mov es, [bx + 16]
        mov bx, [bx + 2]
        call far [cs:fcaddr]
        pop es
        pop ds
        pop di
        pop si
        pop bp
        ret

; void __cdecl hook2f(void): INT 2Fh, and INT 21h for the long file names
_hook2f:
        push es
        mov ax, 352Fh
        int 21h
        mov [cs:_old2f], bx
        mov [cs:_old2f + 2], es
        mov ax, 3521h
        int 21h
        mov [cs:_old21], bx
        mov [cs:_old21 + 2], es
        pop es
        push ds
        push cs
        pop ds
        mov dx, _int2f
        mov ax, 252Fh
        int 21h
        mov dx, _int21
        mov ax, 2521h
        int 21h
        pop ds
        ret

; void __cdecl unhook2f(void): both vectors back (only when ours are the first handlers)
_unhook2f:
        push ds
        lds dx, [cs:_old2f]
        mov ax, 252Fh
        int 21h
        lds dx, [cs:_old21]
        mov ax, 2521h
        int 21h
        pop ds
        ret

; void __cdecl unhook_links(void __far * __far *l2f, void __far * __far *l21): the links
; (vector or the next pointer of an IISP header) that lead to our handlers get our next
_unhook_links:
        push bp
        mov bp, sp
        push es
        push bx
        pushf
        cli
        les bx, [bp + 4]
        mov ax, [cs:_old2f]
        mov [es:bx], ax
        mov ax, [cs:_old2f + 2]
        mov [es:bx + 2], ax
        les bx, [bp + 8]
        mov ax, [cs:_old21]
        mov [es:bx], ax
        mov ax, [cs:_old21 + 2]
        mov [es:bx + 2], ax
        popf
        pop bx
        pop es
        pop bp
        ret

; the end of the start: the resident data moves down behind the resident code (over the
; start-up code, which is why this runs from here), then the program stays resident
stay:   cli
        mov ax, cs
        mov bx, rescode_end wrt CGROUP
        shr bx, 4
        add ax, bx
        mov es, ax                      ; new data segment
        xor si, si
        xor di, di
        mov cx, _resend wrt DGROUP
        mov dx, [_psp]
        cld
        rep movsb
        mov [cs:dgseg], es
        mov ax, es
        mov ds, ax
        mov ss, ax
        mov sp, stack_top wrt DGROUP
        sti
        ; paragraphs to keep: PSP up to the end of the moved data
        mov bx, _resend wrt DGROUP
        add bx, 15
        shr bx, 4
        add ax, bx
        sub ax, dx
        mov dx, ax
        mov ax, 3100h
        int 21h

segment RESCEND
rescode_end:

; ---------------------------------------------------------------------------
segment _DATA
_psp    dw 0                    ; PSP of the program
_lfn_on db 1                    ; the INT 21h AH=71h functions are served
_ressize dw 0                   ; paragraphs that stay resident (PSP, code, data)

segment _BSS
        resb 2560
stack_top:
        resb 2048
lstack_top:

segment RESEND
_resend:                        ; end of the resident data

; ---------------------------------------------------------------------------
segment INIT_TEXT
init:   mov ax, DGROUP
        mov ds, ax
        cli
        mov ss, ax
        mov sp, init_stack wrt DGROUP
        sti
        mov [cs:dgseg], ax
        mov [_psp], es
        ; the program block: PSP up to the end of the stack
        mov bx, init_stack wrt DGROUP
        add bx, 15
        shr bx, 4
        add bx, ax
        mov cx, es
        sub bx, cx
        mov ah, 4Ah
        int 21h
        ; resident paragraphs: PSP, code up to rescode_end, data up to _resend
        mov ax, rescode_end wrt CGROUP
        shr ax, 4
        add ax, 10h
        mov bx, _resend wrt DGROUP
        add bx, 15
        shr bx, 4
        add ax, bx
        mov [_ressize], ax
        push ds
        pop es
        cld
        call _dos_init          ; returns AX != 0: stay resident
        test ax, ax
        jnz .tsr
        mov ah, 4Ch
        mov al, [_exitcode]
        int 21h
.tsr:   jmp stay

segment INIT_DATA
global _exitcode
_exitcode db 0

segment INITEND
        resb 2048
init_stack:
