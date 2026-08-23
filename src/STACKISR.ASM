
;--- handle sound hardware interrupt

	.386
	.MODEL ?MODEL, c
	option casemap:none
	option proc:private

	include config.inc
	include debug.inc

	public dwDS

externdef c __djgpp_stack_top:dword
externdef bIntSnd:byte

;--- ES1688 port: STACKCHECK on -- trips fatal_error(3) if SNDISR nesting ever
;--- carves more than 64KB (16 levels) off the private ISR stack, instead of
;--- silently marching into .data and corrupting fn ptrs (the hardware-test-3
;--- #GP). With the PT-mode speedups in sndisr.c depth should stay 1.
STACKCHECK equ 1

if STACKCHECK
externdef c DSBase:dword	; top-level on purpose: declared inside .code it
				; associates with CS and the ref gets a CS
				; prefix -- the diag path #GP'd exactly there
externdef c STACKLOW:dword	; OW startup's _STACKLOW (model c adds the _)
endif

ifdef NOTFLAT

STACKCORR equ 2048

	.code

dfOldSndVec PFAR ?	; original vector sound ISR
currSndvec  PFAR ?	; current vector sound ISR
dwDS    dd ?

	.data

else

STACKCORR equ 4096

	assume ds:_DATA

	.data

dfOldSndVec PFAR ?	; original vector sound ISR
	align 4
currSndvec  PFAR ?	; current vector sound ISR
	align 4
dwDS    dd ?

endif

dwISR	dd ?
if STACKCHECK
dwStackBottom dd ?
endif

	.code

;--- ISR for sound hw interrupt occuring in protected-mode

SwitchStackISR proc

	pushad
	push ds
	push es
	cld
	mov ebx, cs:[dwDS]
	mov ds, ebx
	mov es, ebx
	mov edx, ss
	mov ecx, esp

	sub [__djgpp_stack_top], STACKCORR
if STACKCHECK
	mov eax, [__djgpp_stack_top]
	cmp eax, [dwStackBottom]
	jc failure
endif
ifdef _DEBUG
	mov eax, [__djgpp_stack_top]	; test if the new stack is writable before ss is changed
	mov [eax-4], eax
endif
	mov ss, ebx
	mov esp, [__djgpp_stack_top]
	push edx
	push ecx
	call [dwISR]
	lss esp, [esp]
	add [__djgpp_stack_top], STACKCORR
if COMPAT4
	test al,2       ; PIT masked?
	jz @F
	mov al,ah
	out 21h,al
	mov al,1
@@:
endif
	and eax, eax	; interrupt handled?
	pop es
	pop ds
	popad
	jz prevhdlr
if not SETIF
;--- v1.6: if SETIF is 1, then interrupts were enabled inside the sound ISR;
;---       else they must be reenabled now.
	push eax
	insPUSHF
	mov ax,901h
	call cs:[oldint31]
	pop eax
endif
	insIRET
prevhdlr:
	jmp cs:[dfOldSndVec]
if STACKCHECK
failure:
ifdef STKDIAG
;--- DIAG build (bench autopsy, 2026-08-23 "fatal error 3" on the 235):
;--- SURVIVE the trip instead of dying, so COMRADE stays up and the 0x4F0
;--- telemetry (SNDISR maxdepth at 0x4F0, PMISR depth/max at 0x4FC/0x4FD)
;--- can be read after the event. Mask the RTC so an IRQ8 storm stops at
;--- the source, EOI both PICs so nothing stays in-service, mark 0x4FE=F3,
;--- unwind this entry and iret. Registers: pushad/ds/es are on the entry
;--- stack and SS has not been switched yet; DS is ours.
	in  al, 0A1h
	or  al, 1
	out 0A1h, al		; mask IRQ8: if the storm is RTC re-fire, it ends here
	mov al, 20h
	out 0A0h, al		; EOI slave + master: leave no in-service bits behind
	out 20h, al
	mov eax, 4FEh
	sub eax, [DSBase]
	mov byte ptr [eax], 0F3h	; marker: the stack check tripped
	add [__djgpp_stack_top], STACKCORR	; unwind our carve
	pop es
	pop ds
	popad
	insIRET
else
externdef c fatal_error:near
	push 3
	call fatal_error
endif
endif
SwitchStackISR endp

if TRAPINT21

;--- int 21h, ax=25xx,35xx called

_Snd_Notify21 proc public
	cmp ah, 25h
	jz is_25
ifdef NOTFLAT
	mov bx, word ptr cs:[currSndvec+0]
	mov es, word ptr cs:[currSndvec+SEGOFS]
else
	mov ebx, dword ptr cs:[currSndvec+0]
	mov es,   word ptr cs:[currSndvec+SEGOFS]
endif
	clc  ; return NC - call won't be routed to DPMI host
	ret
is_25:
	push es
ifdef NOTFLAT
	mov es, cs:[_csalias]
	assume es:_TEXT
else
	mov es, cs:[dwDS]
endif
	mov dword ptr es:[currSndvec+0], edx
	mov word ptr es:[currSndvec+SEGOFS], ds
	pop es
ifdef NOTFLAT
	assume es:DGROUP
endif
	clc  ; return NC - call won't be routed to DPMI host
	ret
_Snd_Notify21 endp

endif

NOTFR struct
	dd ?	;return
	IRETS <>
NOTFR ends

;--- int 31h, ax=204/205 called with BL=snd interrupt

_Snd_Notify31 proc public
	and byte ptr [esp].NOTFR._fl, not 1
	cmp al, 5
	jz is_05
ifdef NOTFLAT
	mov dx, word ptr cs:[currSndvec+0]
else
	mov edx, dword ptr cs:[currSndvec+0]
endif
	mov cx, word ptr cs:[currSndvec+SEGOFS]
	clc  ; return NC - call won't be routed to DPMI host
	ret

is_05:
	push ds
ifdef NOTFLAT
	mov ds, cs:[_csalias]
	assume ds:_TEXT
else
	mov ds, cs:[dwDS]
endif
	mov dword ptr [currSndvec+0], edx
	mov word ptr [currSndvec+SEGOFS], cx
	pop ds
ifdef NOTFLAT
	assume ds:DGROUP
endif
	clc  ; return NC - call won't be routed to DPMI host
	ret
_Snd_Notify31 endp

;--- install the sound ISR
;--- the idea is to hide this ISR from the ring3 DOS extender;

_SND_InstallISR proc public uses ebx interrupt:byte, isr:dword

	@dprintf CStr("SND_InstallISR enter",10)

	mov bl, interrupt
	mov ax, 204h
	int 31h
	jc error

	mov eax, isr
	mov dwISR, eax

ifdef NOTFLAT
	push ds
	mov ds, cs:[_csalias]
	assume ds:_TEXT
endif
	mov dword ptr [dfOldSndVec + 0], edx
	mov word ptr [dfOldSndVec + SEGOFS], cx
	mov dword ptr [currSndvec + 0], edx
	mov word ptr [currSndvec + SEGOFS], cx

	mov dwDS, ss
	mov [bIntSnd], bl	; activates notification by int 31h handler

ifdef NOTFLAT
	pop ds
	assume ds:DGROUP
endif

if STACKCHECK
ifdef NOTFLAT
;--- the stack here is the OW startup's 16K STACK segment inside a ~41K
;--- DGROUP: top - 64K UNDERFLOWS, the bottom bound wraps huge, and the
;--- check trips on the FIRST SwitchStackISR entry -- the 2026-08-23
;--- "fatal error 3" on the 235 was exactly this false alarm (telemetry:
;--- SNDISR had never run once). The honest bound is the segment's real
;--- floor, which the startup publishes as _STACKLOW.
	mov eax, [STACKLOW]
else
	mov eax, [__djgpp_stack_top]
	sub eax, 10000h
endif
	mov [dwStackBottom], eax
endif

	mov ecx, cs
	mov edx, offset SwitchStackISR
	mov ax, 205h
	int 31h
error:
	sbb eax,eax
	inc eax
	ret
_SND_InstallISR endp

_SND_UninstallISR proc public uses ebx interrupt:byte

	@dprintf CStr("SND_UninstallISR enter",10)
	mov edx, dword ptr dfOldSndVec
	mov cx, word ptr dfOldSndVec+SEGOFS
	mov bl, interrupt
	mov ax, 205h
	int 31h
	sbb eax,eax
	inc eax
	ret

_SND_UninstallISR endp

ifdef NOTFLAT
;--- v2.0: dummy DoINTR_
;--- + avoids OW's DoINTR_, which adds an array of INT XX instructions.
;--- + ensures that code size remains below 64k.
DoINTR_ proc public
	@dprintf CStr("WARNING! DoINTR_ called",10)
	int 3
	ret
DoINTR_ endp
endif

	END
