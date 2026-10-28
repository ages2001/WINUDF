; udfvxd.asm - WINUDF.VXD: DDB, control dispatch and the VMM, IFS Manager and I/O Supervisor
; services used by the C part (udfvxd.c), as cdecl functions that keep EBX, ESI, EDI, EBP.
; Static VxD of Windows 95/98/ME, loaded by SYSTEM.INI [386Enh] device=WINUDF.VXD.
        bits 32

%macro VMMCALL 1
        int 20h
        dd %1
%endmacro

; VMM services
%define SVC_Get_VMM_Version     00010000h
%define SVC_Get_Cur_VM_Handle   00010001h
%define SVC_Begin_Nest_V86_Exec 00010082h
%define SVC_Exec_Int            00010084h
%define SVC_End_Nest_Exec       00010086h
%define SVC_Save_Client_State   0001008Dh
%define SVC_Restore_Client_State 0001008Eh
%define SVC_Alloc_Global_V86    000100A8h
%define SVC_Map_Flat            0001001Ch
%define SVC_Create_Semaphore    00010025h
%define SVC_Wait_Semaphore      00010027h
%define SVC_Signal_Semaphore    00010028h
%define SVC_Get_System_Time     0001003Fh
%define SVC_HeapAllocate        0001004Fh
%define SVC_HeapFree            00010051h
%define SVC_BlockOnID           000100FAh
%define SVC_Get_Cur_Thread      00010108h
%define SVC_SignalID            00010127h
; IFS Manager
%define SVC_IFS_Get_Version     00400000h
%define SVC_IFS_RegisterMount   00400001h
%define SVC_IFS_Get_DOSTime     00400007h
%define SVC_IFS_MetaMatch       0040003Dh
%define SVC_IFS_UniToBCS        00400040h
%define SVC_IFS_BCSToUni        00400042h
%define SVC_IFS_TimeZoneBias    00400060h
%define SVC_IFS_RegisterFSD     00400076h       ; Windows 98 and ME: mount routine with priority
; I/O Supervisor
%define SVC_IOS_SendCommand     00100004h
%define SVC_IOS_Register        00100007h

segment _LTEXT public class=CODE use32 align=16
segment _LDATA public class=CODE use32 align=16

extern _udf_vxd_init

segment _LDATA
global WINUDF_DDB
WINUDF_DDB:
        dd 0                    ; next
        dw 0400h                ; SDK version (Windows 95)
        dw 0                    ; Undefined_Device_ID: no services
        db 1, 1                 ; version 1.1
        dw 0                    ; flags
        db 'WINUDF  '           ; name
        dd 0A0010100h           ; FSD_Init_Order: after the IFS Manager
        dd Control
        dd 0, 0                 ; V86 / PM API
        dd 0, 0                 ; V86 / PM API CS:IP
        dd 0                    ; reference data
        dd 0, 0                 ; services
        dd 0                    ; Win32 services
        dd 0x50726576           ; 'Prev'
        dd 80                   ; DDB size
        dd 0x52737631, 0x52737632, 0x52737633   ; 'Rsv1' 'Rsv2' 'Rsv3'

segment CONST public class=DATA use32 align=4
segment CONST2 public class=DATA use32 align=4
segment _DATA public class=DATA use32 align=4
segment _BSS public class=BSS use32 align=16
global _bss_begin
_bss_begin:
segment _DATA
global _vx_v86buf
_vx_v86buf dd 0
segment ZZBSS public class=BSS use32 align=16
global _bss_end
_bss_end:
group DGROUP CONST CONST2 _DATA _BSS ZZBSS

segment _LTEXT

Control:
        cmp eax, 0              ; Sys_Critical_Init: uninitialized data cleared
        jne .ninit
        push edi
        push ecx
        mov edi, _bss_begin
        mov ecx, _bss_end
        sub ecx, edi
        xor eax, eax
        cld
        rep stosb
        ; a buffer in V86 memory for the BIOS disk calls (only possible now)
        push dword 0204h                ; GVDAParaAlign | GVDAZeroInit
        push dword 2200h                ; 16 sectors of 512 bytes and the packet
        VMMCALL SVC_Alloc_Global_V86
        add esp, 8
        mov [_vx_v86buf], eax
        pop ecx
        pop edi
        clc
        ret
.ninit: cmp eax, 1              ; Device_Init
        jne .ok
        pushad
        cld
        call _udf_vxd_init
        popad
.ok:    clc
        ret

; ---------------------------------------------------------------------------
; C-convention services: the arguments copied, EBX ESI EDI EBP kept
%macro CSVC 3
global %1
%1:
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
%assign i %3
%rep %3
        push dword [ebp + 4 + 4*i]
%assign i i-1
%endrep
        VMMCALL %2
        add esp, 4*%3
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret
%endmacro

CSVC _vx_heap_alloc, SVC_HeapAllocate, 2         ; void *(u32 size, u32 flags)
CSVC _vx_heap_free, SVC_HeapFree, 2              ; void (void *p, u32 flags)
CSVC _vx_block, SVC_BlockOnID, 2                 ; void (u32 id, u32 flags)
CSVC _vx_signal, SVC_SignalID, 1                 ; void (u32 id)
CSVC _vx_register_mount, SVC_IFS_RegisterMount, 3    ; int (fn, version, default)
CSVC _vx_register_fsd, SVC_IFS_RegisterFSD, 6        ; int (type, fn, order, 0, class, version)
CSVC _vx_dostime, SVC_IFS_Get_DOSTime, 0         ; u32 (void): date << 16 | time
CSVC _vx_tzbias, SVC_IFS_TimeZoneBias, 0         ; s32 (void): minutes, UTC = local + bias
CSVC _vx_metamatch, SVC_IFS_MetaMatch, 3         ; int (u16 *pattern, u16 *name, flags)
CSVC _vx_unitobcs, SVC_IFS_UniToBCS, 5           ; u32 (u8 *out, u16 *in, bytes, max, set)
CSVC _vx_bcstouni, SVC_IFS_BCSToUni, 4           ; u32 (u16 *out, u8 *in, bytes, set)
CSVC _vx_ios_register, SVC_IOS_Register, 1       ; void (struct drp *)

; int vx_intr(u32 n, struct v86r *r): software interrupt n in V86 mode (nested execution in
; the current VM); r: EAX EBX ECX EDX ESI EDI, DS ES (words), flags out
global _vx_intr
_vx_intr:
        push ebp
        push ebx
        push esi
        push edi
        sub esp, 80h
        VMMCALL SVC_Get_Cur_VM_Handle
        mov ebp, [ebx + 8]              ; CB_Client_Pointer
        mov edi, esp
        VMMCALL SVC_Save_Client_State
        VMMCALL SVC_Begin_Nest_V86_Exec
        mov esi, [esp + 98h]            ; r
        mov eax, [esi]
        mov [ebp + 1Ch], eax
        mov eax, [esi + 4]
        mov [ebp + 10h], eax
        mov eax, [esi + 8]
        mov [ebp + 18h], eax
        mov eax, [esi + 12]
        mov [ebp + 14h], eax
        mov eax, [esi + 16]
        mov [ebp + 4], eax
        mov eax, [esi + 20]
        mov [ebp + 0], eax
        movzx eax, word [esi + 24]
        mov [ebp + 3Ch], eax
        movzx eax, word [esi + 26]
        mov [ebp + 38h], eax
        and dword [ebp + 2Ch], ~1
        mov eax, [esp + 94h]            ; n
        VMMCALL SVC_Exec_Int
        mov esi, [esp + 98h]
        mov eax, [ebp + 1Ch]
        mov [esi], eax
        mov eax, [ebp + 10h]
        mov [esi + 4], eax
        mov eax, [ebp + 18h]
        mov [esi + 8], eax
        mov eax, [ebp + 14h]
        mov [esi + 12], eax
        mov eax, [ebp + 4]
        mov [esi + 16], eax
        mov eax, [ebp + 0]
        mov [esi + 20], eax
        mov ax, [ebp + 3Ch]
        mov [esi + 24], ax
        mov ax, [ebp + 38h]
        mov [esi + 26], ax
        mov eax, [ebp + 2Ch]
        mov [esi + 28], eax
        VMMCALL SVC_End_Nest_Exec
        mov esi, esp
        VMMCALL SVC_Restore_Client_State
        add esp, 80h
        cld
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret

; u32 vx_vmm_version(void): major << 8 | minor
global _vx_vmm_version
_vx_vmm_version:
        push ebx
        push ecx
        push edx
        push esi
        push edi
        VMMCALL SVC_Get_VMM_Version
        movzx eax, ax
        pop edi
        pop esi
        pop edx
        pop ecx
        pop ebx
        ret

; u32 vx_ifs_version(void): 0 when the IFS Manager is not there
global _vx_ifs_version
_vx_ifs_version:
        push ebx
        push ecx
        push edx
        push esi
        push edi
        VMMCALL SVC_IFS_Get_Version
        jnc .ok
        xor eax, eax
.ok:    pop edi
        pop esi
        pop edx
        pop ecx
        pop ebx
        ret

; u32 vx_sem_create(void): semaphore with one token, 0 on failure
global _vx_sem_create
_vx_sem_create:
        push ebx
        push esi
        push edi
        mov ecx, 1
        VMMCALL SVC_Create_Semaphore
        jnc .ok
        xor eax, eax
.ok:    pop edi
        pop esi
        pop ebx
        ret

; void vx_sem_wait(u32 sem)
global _vx_sem_wait
_vx_sem_wait:
        push ebx
        push esi
        push edi
        mov eax, [esp + 16]
        mov ecx, 1                      ; BLOCK_SVC_INTS
        VMMCALL SVC_Wait_Semaphore
        pop edi
        pop esi
        pop ebx
        ret

; void vx_sem_signal(u32 sem)
global _vx_sem_signal
_vx_sem_signal:
        push ebx
        push esi
        push edi
        mov eax, [esp + 16]
        VMMCALL SVC_Signal_Semaphore
        pop edi
        pop esi
        pop ebx
        ret

; u32 vx_thread(void): current thread handle
global _vx_thread
_vx_thread:
        push edi
        VMMCALL SVC_Get_Cur_Thread
        mov eax, edi
        pop edi
        ret

; u32 vx_time(void): milliseconds since Windows started
global _vx_time
_vx_time:
        VMMCALL SVC_Get_System_Time
        ret

; u32 vx_map_flat(void *client_regs, u32 segofs): linear address of a client segment:offset
; (AH: offset of the segment register in the client registers, AL: of the offset register)
global _vx_map_flat
_vx_map_flat:
        push ebp
        push ebx
        push esi
        push edi
        mov ebp, [esp + 20]
        mov eax, [esp + 24]
        push eax
        VMMCALL SVC_Get_Cur_VM_Handle
        pop eax
        VMMCALL SVC_Map_Flat
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret

; void vx_ios_send(void *ior, u32 dcb)
global _vx_ios_send
_vx_ios_send:
        push ebp
        push ebx
        push esi
        push edi
        mov esi, [esp + 20]
        mov edi, [esp + 24]
        VMMCALL SVC_IOS_SendCommand
        cld
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret

; u32 vx_call1(void *fn, void *arg): C-style call of an I/O Supervisor routine
global _vx_call1
_vx_call1:
        push ebp
        mov ebp, esp
        push ebx
        push esi
        push edi
        push dword [ebp + 12]
        call [ebp + 8]
        add esp, 4
        cld
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret

; IOR completion (ESI = IOR): clears the pending word after the IOR and wakes the
; thread that waits for it
global _vx_iorcb
_vx_iorcb:
        push eax
        push ecx
        push edx
        mov dword [esi + 58h], 0       ; after the IOR, in the IOP
        push esi
        VMMCALL SVC_SignalID
        add esp, 4
        pop edx
        pop ecx
        pop eax
        ret
