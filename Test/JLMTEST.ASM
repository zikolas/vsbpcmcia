;--- JLMTEST: a DOS-prompt check of VSBPCMJ.DLL, VSBPCM's ring-0 companion
;--- (jlm/VSBPCMJ.ASM), and an A/B of what it buys. Run it with VSBPCM
;--- resident, from V86 -- the world the JLM serves.
;---
;--- JLMTEST [count]   JLMTEST S (step 1 only: state and counters)
;--- JLMTEST P [hz]     what a fast PIT costs (see "P mode" below)
;--- JLMTEST T          IBMAUD's play position as VSBPCM reads it (below)
;--- JLMTEST W [s] [us]  a direct-DAC tone to listen to (see "W mode")
;---   1. finds VSBPCMJ (INT 2Fh AX=1684h BX=5650h) and prints its state and
;---      counters. Without it, or unarmed, the rest still runs -- through
;---      QPI, which is the other side of the A/B.
;---   2. the AdLib timer detection, as games run it, at 388h and at the SB
;---      base alias (base+8): status must read 00 after the reset and C0
;---      once timer 1 is started.
;---   3. count FM register writes (default 2000) at 388h, each padded with
;---      6 and 35 status reads -- the AdLib delay idiom the JLM patches out
;---      -- and the BIOS ticks they took. Registers A0h-A8h (frequency low
;---      bytes, no key-on), so nothing sounds; all voices are keyed off
;---      after.
;---   4. count direct-DAC samples (10h and one byte to base+0Ch, the write
;---      status polled before each) and the ticks they took. The byte is
;---      80h, silence.
;---   5. the counters again.
;--- SB base 220h (VSBPCM's /A220, the launchers' BLASTER=A220).
;---
;--- P mode: a direct-DAC game (Another World) runs the PIT at its sample
;--- rate and writes one sample per tick, so every resident that hooks IRQ0
;--- or traps the DSP is paid that often. P counts a busy loop for 2 s three
;--- times: PIT at 18.2 Hz; PIT at [hz] (default 10000) with a bare ISR; and
;--- PIT at [hz] with the ISR also writing one sample the way Another World
;--- does (poll, 10h, poll, 80h). The loss against the first count, spread
;--- over the ticks, is the cost of one tick. The ISR chains to the old
;--- INT 8 about 18.2 times a second, so the BIOS clock keeps time. A
;--- fourth run adds three PUSHF/CLI/POPF to the bare ISR: under Jemm each
;--- costs a fault into the monitor unless the CPU's VME is in use.
;---
;--- T mode: the IBM PCMCIA Audio Adapter's play position (346h, a 14-bit
;--- word count) read with IN AX,DX back to back for 4 s, while the P-mode
;--- DAC ISR at 10 kHz keeps VSBPCM's card playing. Steps of 0-8 words are
;--- the count advancing, 9-127 an interrupt between two reads; anything
;--- else (a jump of about 255 and the step back) is a read that caught
;--- the low byte wrapping between its two halves. Counts the low-byte
;--- wraps seen too, and prints the first four jumps as before > after.
;---
;--- W mode: a 440 Hz sine by direct DAC for [s] seconds (default 20), from
;--- the P-mode ISR at 10 kHz -- Another World's code shape, so VSBPCMJ
;--- patches it as it does the game. A pure tone shows what the path adds:
;--- a clean tone is a clean path. [us] (default 0) blocks interrupts for
;--- that many port-80h reads (about a microsecond each) every other BIOS
;--- tick, the way a busy guest loses timer ticks and delivers in bursts.
;---
;--- Same structure and includes as the other tests here (printf.inc, the
;--- SB.INC macros, PTBYPASS's argument parsing). Build: jwasm -mz JLMTEST.ASM

	.286
	.MODEL tiny
	.dosseg
	.STACK 400h
	option casemap:none

SBBASE  equ 0220h
DEVID   equ 5650h               ; VSBPCMJ's device id

	include SB.INC

CStr macro text:vararg
local sym
	.const
sym db text,0
	.code
	exitm <offset sym>
endm

	.data

jlmentry dd 0                   ; VSBPCMJ's V86 API, 0 = not loaded
blk      dd 0                   ; linear address of its shared block
count    dw 2000
statonly db 0
t0       dd 0
pmode    db 0
dacmode  db 0                   ; P: the ISR writes a direct-DAC sample
hz       dw 10000
isrcnt   dw 0                   ; P: ticks since the last BIOS chain
chainat  dw 0                   ; P: ticks per BIOS tick
oldint8  dd 0
TPORT    equ 0346h              ; T: IBMAUD's play position (/BASE 340h)
tend     dd 0                   ; T: the BIOS tick it stops at
treads   dd 0
tcarry   dd 0                   ; T: low-byte wraps seen in small steps
tslow    dd 0                   ; T: steps of 9-127 words
tjump    dd 0                   ; T: any other step
tlog     dw 8 dup (0)           ; T: the first four jumps, before and after
wsec     dw 20                  ; W: seconds
wcli     dw 0                   ; W: port-80h reads with interrupts off
tnext    dd 0                   ; W: the BIOS tick of the next block
phase    dw 0                   ; W: sine phase, 1/65536 of a turn
TSTEP    equ 2884               ; W: 440 Hz at 10 kHz
sintab   label byte             ; W: 128 + 48 sin, a turn in 256 steps
	db 80h,81h,82h,84h,85h,86h,87h,88h,89h,8Bh,8Ch,8Dh,8Eh,8Fh,90h,91h
	db 92h,93h,95h,96h,97h,98h,99h,9Ah,9Bh,9Ch,9Dh,9Eh,9Eh,9Fh,0A0h,0A1h
	db 0A2h,0A3h,0A4h,0A4h,0A5h,0A6h,0A7h,0A7h,0A8h,0A9h,0A9h,0AAh,0AAh,0ABh,0ABh,0ACh
	db 0ACh,0ADh,0ADh,0AEh,0AEh,0AEh,0AFh,0AFh,0AFh,0AFh,0AFh,0B0h,0B0h,0B0h,0B0h,0B0h
	db 0B0h,0B0h,0B0h,0B0h,0B0h,0B0h,0AFh,0AFh,0AFh,0AFh,0AFh,0AEh,0AEh,0AEh,0ADh,0ADh
	db 0ACh,0ACh,0ABh,0ABh,0AAh,0AAh,0A9h,0A9h,0A8h,0A7h,0A7h,0A6h,0A5h,0A4h,0A4h,0A3h
	db 0A2h,0A1h,0A0h,9Fh,9Eh,9Eh,9Dh,9Ch,9Bh,9Ah,99h,98h,97h,96h,95h,93h
	db 92h,91h,90h,8Fh,8Eh,8Dh,8Ch,8Bh,89h,88h,87h,86h,85h,84h,82h,81h
	db 80h,7Fh,7Eh,7Ch,7Bh,7Ah,79h,78h,77h,75h,74h,73h,72h,71h,70h,6Fh
	db 6Eh,6Dh,6Bh,6Ah,69h,68h,67h,66h,65h,64h,63h,62h,62h,61h,60h,5Fh
	db 5Eh,5Dh,5Ch,5Ch,5Bh,5Ah,59h,59h,58h,57h,57h,56h,56h,55h,55h,54h
	db 54h,53h,53h,52h,52h,52h,51h,51h,51h,51h,51h,50h,50h,50h,50h,50h
	db 50h,50h,50h,50h,50h,50h,51h,51h,51h,51h,51h,52h,52h,52h,53h,53h
	db 54h,54h,55h,55h,56h,56h,57h,57h,58h,59h,59h,5Ah,5Bh,5Ch,5Ch,5Dh
	db 5Eh,5Fh,60h,61h,62h,62h,63h,64h,65h,66h,67h,68h,69h,6Ah,6Bh,6Dh
	db 6Eh,6Fh,70h,71h,72h,73h,74h,75h,77h,78h,79h,7Ah,7Bh,7Ch,7Eh,7Fh

	.CODE
	.386
	include PRINTF.INC

dectest proc near
	cmp al,'0'
	jc dectst1
	cmp al,'9' + 1
	jnc dectst1
	sub al,'0'
	and al,al
	ret
dectst1:
	stc
	ret
dectest endp

;--- in: bx->string; out: AX = number, CF on error (from PTBYPASS)
getdec proc uses di
	mov ch,0
	mov di,0
nextdigit:
	mov al,[bx]
	call dectest
	jc done
	inc ch
	mov ah,0
	push ax
	mov ax,di
	mov di,10
	mul di
	mov di,ax
	pop ax
	add di,ax
	jc exit
	cmp dx,0
	stc
	jnz exit
	inc bx
	jmp nextdigit
done:
	cmp ch,1
	mov ax,di
exit:
	ret
getdec endp

;--- EAX = BIOS tick count
ticks proc
	push es
	push 40h
	pop es
	mov eax, es:[6Ch]
	pop es
	ret
ticks endp

;--- wait for a tick edge, then t0 = that tick
starttick proc
	call ticks
	mov t0, eax
@@:
	call ticks
	cmp eax, t0
	jz @B
	mov t0, eax
	ret
starttick endp

;--- report count items in the ticks since t0
report proc c uses bx pName:ptr byte
	call ticks
	sub eax, t0
	mov ebx, eax
	movzx eax, count
	invoke printf, CStr("%s: %u in %lu ticks"), pName, count, ebx
	or ebx, ebx
	jz rep_fast
	movzx eax, count
	mov edx, 182
	mul edx
	mov ecx, ebx
	imul ecx, ecx, 10
	div ecx
	invoke printf, CStr(" = %lu/s",10), eax
	ret
rep_fast:
	invoke printf, CStr(" (under one tick)",10)
	ret
report endp

;--- one OPL register write at DX (index port): AL = register, AH = value,
;--- with the AdLib delays, 6 status reads after the index, 35 after the
;--- value. Destroys AL, CX.
fmwrite proc
	out dx, al
	mov cx, 6
@@:
	in al, dx
	loop @B
	inc dx
	mov al, ah
	out dx, al
	dec dx
	mov cx, 35
@@:
	in al, dx
	loop @B
	ret
fmwrite endp

;--- AdLib timer detection at DX: AL = status after the reset, AH = status
;--- once timer 1 has had time to expire
detect proc uses bx cx
	mov ax, 6004h               ; reg 4 <- 60h: mask both timers
	call fmwrite
	mov ax, 8004h               ; reg 4 <- 80h: reset the IRQ flags
	call fmwrite
	in al, dx
	mov bl, al
	mov ax, 0FF02h              ; reg 2 <- FFh: timer 1 expires in 80 us
	call fmwrite
	mov ax, 2104h               ; reg 4 <- 21h: start timer 1
	call fmwrite
	mov cx, 400
@@:
	in al, 61h                  ; ~1 us each, untrapped
	loop @B
	in al, dx
	mov bh, al
	mov ax, 6004h
	call fmwrite
	mov ax, 8004h
	call fmwrite
	mov ax, bx
	ret
detect endp

;--- print VSBPCMJ's counters from its shared block (src/JLMSHARE.H)
showblk proc uses es bx si
	mov eax, blk
	or eax, eax
	jnz @F
	invoke printf, CStr("VSBPCMJ: not armed",10)
	ret
@@:
	mov ebx, eax
	shr eax, 4
	mov es, ax
	and bx, 0Fh
	mov eax, es:[bx+28h]
	mov ecx, es:[bx+2Ch]
	mov edx, es:[bx+30h]
	invoke printf, CStr("  FM: %lu writes, %lu status reads, %lu patched",10), eax, ecx, edx
	mov eax, es:[bx+20h]
	mov ecx, es:[bx+24h]
	movzx edx, word ptr es:[bx+1Eh]
	invoke printf, CStr("  DAC: %lu samples, %lu lost, fed at %lu Hz",10), eax, ecx, edx
	mov eax, es:[bx+34h]
	mov ecx, es:[bx+38h]
	invoke printf, CStr("  DSP: %lu writes forwarded, %lu status reads",10), eax, ecx
	mov eax, es:[bx+58h]
	mov ecx, es:[bx+5Ch]
	mov edx, es:[bx+60h]
	invoke printf, CStr("  drains %lu, fed %lu, flushed %lu",10), eax, ecx, edx
	mov eax, es:[bx+64h]
	mov ecx, es:[bx+68h]
	mov edx, es:[bx+6Ch]
	invoke printf, CStr("  longest gap %lu/32768 s, room-cut %lu, held %lu",10), eax, ecx, edx
	movzx eax, word ptr es:[bx+70h]
	movzx ecx, word ptr es:[bx+72h]
	movzx edx, word ptr es:[bx+74h]
	movzx esi, word ptr es:[bx+76h]
	invoke printf, CStr("  ring peak %lu, streams %lu, rate changes %lu, gaps %lu",10), eax, ecx, edx, esi
	mov eax, es:[bx+78h]
	movzx ecx, byte ptr es:[bx+7Ch]
	mov edx, es:[bx+88h]
	invoke printf, CStr("  sound IRQs %lu, pump guard %lu, DSP polls patched %lu",10), eax, ecx, edx
	movzx eax, word ptr es:[bx+80h]
	movzx ecx, word ptr es:[bx+82h]
	movzx edx, word ptr es:[bx+84h]
	invoke printf, CStr("  RTC put back: PIE %lu, IRQ8 unmask %lu, stuck flag %lu",10), eax, ecx, edx
	mov eax, es:[bx+90h]
	mov ecx, es:[bx+94h]
	invoke printf, CStr("  EOIs at ring 0 %lu, 10h+sample pairs %lu",10), eax, ecx
	mov eax, es:[bx+9Ch]
	mov ecx, es:[bx+98h]
	invoke printf, CStr("  V86 producer: %lu sites, %lu samples",10), eax, ecx
	movzx eax, word ptr es:[bx+900h]
	movzx ecx, word ptr es:[bx+902h]
	movzx edx, word ptr es:[bx+904h]
	invoke printf, CStr("  1-s rate %lu..%lu Hz, card short by up to %lu samples",10), eax, ecx, edx
	movsx eax, word ptr es:[bx+906h]
	movsx ecx, word ptr es:[bx+90Ah]
	movsx edx, word ptr es:[bx+90Ch]
	movzx esi, word ptr es:[bx+908h]
	invoke printf, CStr("  fill trim now %ld Hz, range %ld..%ld, gaps held %lu",10), eax, ecx, edx, esi
	cmp word ptr es:[bx+4], 6       ; the FM ring came with layout 6
	jb sb_noring
	movzx eax, word ptr es:[bx+912h]
	mov ecx, es:[bx+918h]
	mov edx, es:[bx+914h]
	invoke printf, CStr("  FM ring: %lu entries, %lu waiting at most, %lu lost",10), eax, ecx, edx
sb_noring:
	lea si, [bx+40h]
	call showsite
	lea si, [bx+4Ch]
	call showsite
	ret
showblk endp

;--- one recorded guest site at ES:SI: its address and 8 code bytes
showsite proc
	mov eax, es:[si]
	invoke printf, CStr("  site %05lX:"), eax
	push cx
	mov cx, 8
	add si, 4
@@:
	movzx eax, byte ptr es:[si]
	push cx
	invoke printf, CStr(" %02lX"), eax
	pop cx
	inc si
	loop @B
	pop cx
	invoke printf, CStr(10)
	ret
showsite endp

;--- P mode ------------------------------------------------------------

;--- IRQ0: optionally one direct-DAC sample in Another World's code shape
;--- (IN/OR/JS polls, MOV AL,10h and MOV AL,AH before the two OUTs), so that
;--- VSBPCMJ patches it as it would there
isr8 proc far
	push ax
	push ds
	mov ax, cs
	mov ds, ax
	cmp dacmode, 3
	jnz isr_np
	pushf                       ; P: what three PUSHF/CLI/POPF cost in V86
	cli                         ; (a fault each into Jemm without VME)
	popf
	pushf
	cli
	popf
	pushf
	cli
	popf
	jmp isr_nodac
isr_np:
	cmp dacmode, 0
	jz isr_nodac
	push dx
	mov ah, 80h                 ; the sample, from AH as Another World does
	cmp dacmode, 2
	jb isr_sample
	push bx                     ; W: the next sine sample instead
	mov bx, phase
	add bx, TSTEP
	mov phase, bx
	mov bl, bh
	xor bh, bh
	mov ah, sintab[bx]
	pop bx
isr_sample:
	mov dx, SBBASE+SB_DSPWRITE
isr_p1:
	in al, dx
	or al, al
	js isr_p1
	mov al, 10h
	out dx, al
isr_p2:
	in al, dx
	or al, al
	js isr_p2
	mov al, ah
	out dx, al
	pop dx
isr_nodac:
	inc isrcnt
	mov ax, isrcnt
	cmp ax, chainat
	jb isr_eoi
	mov isrcnt, 0
	pop ds
	pop ax
	jmp dword ptr cs:[oldint8]  ; the BIOS tick: it sends the EOI
isr_eoi:
	mov al, 20h
	out 20h, al
	pop ds
	pop ax
	iret
isr8 endp

;--- PIT channel 0, square wave, divisor AX (0 = 65536)
setpit proc
	push ax
	mov al, 36h
	out 43h, al
	pop ax
	out 40h, al
	mov al, ah
	out 40h, al
	ret
setpit endp

;--- PIT channel 0 at [hz]
pithz proc
	mov dx, 0012h               ; divisor = 1193182 / hz
	mov ax, 34DEh
	div hz
	call setpit
	ret
pithz endp

;--- isr8 on INT 8, chaining to the BIOS every hz/18.2 ticks
hook8 proc
	xor dx, dx
	mov ax, hz
	mov cx, 10
	mul cx
	mov cx, 182
	div cx
	or ax, ax
	jnz @F
	inc ax
@@:
	mov chainat, ax
	mov ax, 3508h
	int 21h
	mov word ptr oldint8+0, bx
	mov word ptr oldint8+2, es
	push ds
	mov ax, cs
	mov ds, ax
	mov dx, offset isr8
	mov ax, 2508h
	int 21h
	pop ds
	ret
hook8 endp

unhook8 proc
	push ds
	lds dx, oldint8
	mov ax, 2508h
	int 21h
	pop ds
	ret
unhook8 endp

;--- EAX = passes of a busy loop in 36 BIOS ticks (2 s)
measure proc uses ebx ecx
	call starttick
	mov ebx, t0
	add ebx, 36
	xor ecx, ecx
ms_loop:
	inc ecx
	test cx, 3FFh
	jnz ms_loop
	call ticks
	cmp eax, ebx
	jb ms_loop
	mov eax, ecx
	ret
measure endp

;--- one fast-PIT count, reported against the 18.2 Hz one in EBX
fastrun proc c uses ebx esi pName:ptr byte
	cli
	mov isrcnt, 0
	call pithz
	sti
	call measure
	mov esi, eax
	cli
	xor ax, ax
	call setpit                 ; back to 18.2 Hz
	sti
	mov eax, ebx                ; lost fraction, 1/10000
	sub eax, esi
	jns @F
	xor eax, eax
@@:
	mov ecx, 10000
	mul ecx
	div ebx
	mov ecx, 1000               ; tenths of a microsecond per tick:
	mul ecx                     ; lost/10000 * 1e7 / hz
	movzx ecx, hz
	div ecx
	mov ecx, eax
	xor edx, edx
	mov eax, ecx
	mov ecx, 10
	div ecx
	invoke printf, CStr("PIT %u Hz, %s: %lu loops, %lu.%lu us a tick",10), hz, pName, esi, eax, edx
	ret
fastrun endp

;--- CF clear if a DSP answers at SBBASE (reset, then AAh)
dspcheck proc
	mov dx, SBBASE+SB_DSPRESET
	mov al, 1
	out dx, al
	mov cx, 10
@@:
	in al, 80h
	loop @B
	xor al, al
	out dx, al
	mov dx, SBBASE+SB_DSPSTATUS
	mov cx, 2000
dsc_wait:
	in al, dx
	or al, al
	js dsc_data
	loop dsc_wait
	stc
	ret
dsc_data:
	mov dx, SBBASE+SB_DSPREAD
	in al, dx
	cmp al, 0AAh
	jnz @F
	clc
	ret
@@:
	stc
	ret
dspcheck endp

pmain proc
	movzx eax, hz
	invoke printf, CStr("P mode: PIT at %lu Hz against 18.2 Hz, 2 s each",10), eax
	call measure
	push eax
	invoke printf, CStr("PIT 18.2 Hz: %lu loops",10), eax
	call hook8                  ; our IRQ0 handler in
	pop ebx                     ; the 18.2 Hz reference, for fastrun
	mov dacmode, 0
	invoke fastrun, CStr("bare ISR")
	call dspcheck
	jc pm_nodsp
	mov dacmode, 1
	invoke fastrun, CStr("DAC ISR")
	mov dacmode, 3
	invoke fastrun, CStr("ISR + 3 PUSHF/CLI/POPF")
	mov dacmode, 0
	jmp pm_done
pm_nodsp:
	invoke printf, CStr("no DSP answers at %Xh: DAC run skipped",10), SBBASE
pm_done:
	call unhook8
	ret
pmain endp

;--- T mode ------------------------------------------------------------

tmain proc
	movzx eax, hz
	invoke printf, CStr("T mode: %Xh read back to back for 4 s, DAC ISR at %lu Hz",10), TPORT, eax
	call dspcheck
	jnc @F
	invoke printf, CStr("no DSP answers at %Xh: nothing plays",10), SBBASE
	ret
@@:
	call hook8
	mov dacmode, 1
	cli
	mov isrcnt, 0
	call pithz
	sti
	call starttick              ; a second for VSBPCM to arm the card
	mov eax, t0
	add eax, 18
	mov tend, eax
@@:
	call ticks
	cmp eax, tend
	jb @B
	call starttick
	mov eax, t0
	add eax, 72
	mov tend, eax
	mov dx, TPORT
	in ax, dx
	and ax, 3FFFh
	mov bx, ax                  ; the read before
tm_loop:
	in ax, dx
	and ax, 3FFFh
	inc treads
	mov cx, ax
	sub cx, bx
	and cx, 3FFFh
	cmp cx, 8
	ja tm_step
	cmp al, bl                  ; a small step that wrapped the low byte
	jae tm_next
	inc tcarry
	jmp tm_next
tm_step:
	cmp cx, 127
	ja tm_jump
	inc tslow
	jmp tm_next
tm_jump:
	inc tjump
	cmp tjump, 4
	ja tm_next
	mov si, word ptr tjump
	dec si
	shl si, 2
	mov tlog[si], bx
	mov tlog[si+2], ax
tm_next:
	mov bx, ax
	test word ptr treads, 3FFh
	jnz tm_loop
	call ticks
	cmp eax, tend
	jb tm_loop
	cli
	xor ax, ax
	call setpit                 ; back to 18.2 Hz
	sti
	mov dacmode, 0
	call unhook8
	invoke printf, CStr("%lu reads, %lu wraps, %lu slow steps, %lu jumps",10), treads, tcarry, tslow, tjump
	xor si, si
tm_log:
	mov ax, si
	shr ax, 2
	cmp ax, word ptr tjump
	jae tm_end
	cmp si, 16
	jae tm_end
	invoke printf, CStr("  %04X > %04X",10), tlog[si], tlog[si+2]
	add si, 4
	jmp tm_log
tm_end:
	ret
tmain endp

;--- W mode ------------------------------------------------------------

wmain proc
	invoke printf, CStr("W mode: 440 Hz by direct DAC for %u s, %u us blocks",10), wsec, wcli
	call dspcheck
	jnc @F
	invoke printf, CStr("no DSP answers at %Xh: nothing plays",10), SBBASE
	ret
@@:
	call hook8
	mov dacmode, 2
	cli
	mov isrcnt, 0
	call pithz
	sti
	call starttick
	movzx eax, wsec             ; BIOS ticks: seconds * 18.2
	imul eax, eax, 182
	xor edx, edx
	mov ecx, 10
	div ecx
	add eax, t0
	mov tend, eax
	mov eax, t0
	add eax, 2
	mov tnext, eax
wm_loop:
	call ticks
	cmp eax, tend
	jae wm_done
	cmp wcli, 0
	jz wm_loop
	cmp eax, tnext
	jb wm_loop
	add eax, 2
	mov tnext, eax
	cli
	mov cx, wcli
@@:
	in al, 80h
	loop @B
	sti
	jmp wm_loop
wm_done:
	cli
	xor ax, ax
	call setpit                 ; back to 18.2 Hz
	sti
	mov dacmode, 0
	call unhook8
	invoke printf, CStr("W mode: done",10)
	ret
wmain endp

main proc c argc:word, argv:ptr

	cmp argc, 2
	jb args_done
	mov si, argv
	mov bx, [si+2]
	mov al, [bx]
	or al, 20h
	cmp al, 's'                 ; JLMTEST S: the counters only
	jnz @F
	mov statonly, 1
	jmp args_done
@@:
	cmp al, 'p'                 ; JLMTEST P [hz]: the fast-PIT costs
	jnz @F
	cmp argc, 3
	jb pm_go
	mov bx, [si+4]
	call getdec
	jc pm_go
	cmp ax, 100
	jb pm_go
	mov hz, ax
pm_go:
	call pmain
	ret
@@:
	cmp al, 't'                 ; JLMTEST T: IBMAUD's position reads
	jnz @F
	call tmain
	ret
@@:
	cmp al, 'w'                 ; JLMTEST W [s] [us]: a tone to listen to
	jnz wm_args_done
	cmp argc, 3
	jb wm_go
	mov bx, [si+4]
	call getdec
	jc wm_go
	or ax, ax
	jz @F
	mov wsec, ax
@@:
	cmp argc, 4
	jb wm_go
	mov bx, [si+6]
	call getdec
	jc wm_go
	mov wcli, ax
wm_go:
	call wmain
	ret
wm_args_done:
	call getdec
	jc args_done
	or ax, ax
	jz args_done
	mov count, ax
args_done:

;--- 1. find the JLM
	mov ax, 1684h
	mov bx, DEVID
	xor di, di
	mov es, di
	int 2Fh
	or al, al
	jnz nojlm
	mov ax, es
	or ax, di
	jz nojlm
	mov word ptr jlmentry+0, di
	mov word ptr jlmentry+2, es
	xor ax, ax
	xor edx, edx
	call dword ptr [jlmentry]
	mov blk, edx
	movzx ecx, ah
	movzx eax, al
	movzx ebx, bx
	invoke printf, CStr("VSBPCMJ %lu.%lu: flags %lX, block at %lX",10), ecx, eax, ebx, blk
	call showblk
	cmp statonly, 0
	jz detect1
	ret
nojlm:
	invoke printf, CStr("VSBPCMJ not loaded: everything below goes through QPI",10)

;--- 2. AdLib detection at 388h and at the SB alias
detect1:
	mov dx, 388h
	call detect
	movzx ecx, ah
	movzx eax, al
	invoke printf, CStr("detect 388h: %02lX then %02lX (want 00 then C0)",10), eax, ecx
	mov dx, SBBASE+8
	call detect
	movzx ecx, ah
	movzx eax, al
	invoke printf, CStr("detect %Xh: %02lX then %02lX (want 00 then C0)",10), SBBASE+8, eax, ecx

;--- 3. FM register writes with the delay reads
	call starttick
	mov si, count
	xor bl, bl
fm_next:
	mov ax, si                  ; value: the countdown's low byte
	mov ah, al
	mov al, 0A0h
	add al, bl
	mov dx, 388h
	call fmwrite
	inc bl
	cmp bl, 9
	jb @F
	xor bl, bl
@@:
	dec si
	jnz fm_next
	invoke report, CStr("FM writes")
	mov bl, 0B0h                ; key every voice off
@@:
	mov al, bl
	xor ah, ah
	mov dx, 388h
	call fmwrite
	inc bl
	cmp bl, 0B9h
	jb @B

;--- 4. direct DAC
	call starttick
	mov dx, SBBASE+SB_DSPWRITE
	mov si, count
dac_next:
	WAITWRITE
	mov al, 10h
	out dx, al
	WAITWRITE
	mov al, 80h
	out dx, al
	dec si
	jnz dac_next
	invoke report, CStr("DAC samples")

;--- 5. the counters again
	cmp blk, 0
	jz @F
	invoke printf, CStr("after:",10)
	call showblk
@@:
	ret
main endp

	include SETARGV.INC

start:
		mov ax,cs
		mov ds,ax
		mov bx,ss
		sub bx,ax
		shl bx,004h
		mov ss,ax
		add sp,bx
		call _setargv
		invoke main, [_argc], [_argv]
		mov ax,4c00h
		int 21h

	END start
