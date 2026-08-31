;--- TEST06: drives the two guest formats that BYPASS vsbpcm's passthrough
;--- tap, so the engine's RENDER TAIL runs instead.
;---
;--- Derived from Baron-von-Riedesel's TEST01 in this directory by way of
;--- TEST05 (same structure, DMA/SB setup, macros and argument handling); the
;--- ADPCM and direct-DAC payloads are the additions. No separate copyright
;--- is claimed over it -- it is a bench tool for this repo's own driver.
;---
;--- WHY: sndisr.c gates the tap on
;---     pt_block = pt_mode && VSB_GetBits() >= 8
;--- so anything below 8 bits per sample -- ADPCM -- fails it, and direct-DAC
;--- (DSP cmd 10h) never sets vsb.Started at all, so VSB_Running() is false
;--- and the block loop never runs. Both therefore leave pt_took at 0, the
;--- ISR does NOT take its goto isrexit shortcut, and the whole render tail
;--- executes: DecodeADPCM, cv_rate, the silence memset, the volume pass and
;--- AU_writedata. That tail is the least-tested code in the driver, and no
;--- existing test reaches it -- TEST01..TEST05 are all 8-bit or wider, so
;--- they all take the tap.
;---
;--- It doubles as a WEDGE REPRODUCER. On a passthrough backend `samples` is
;--- forced to PT_MODE_SAMPLES (1024) regardless of what the guest supplied,
;--- and sc_es1688/sc_vew211/sc_scp55 register no depth hook, so sndisr's
;--- nesting limiter is inactive on all three. Watch 0x4F0 (max nesting
;--- depth, healthy = 1) and 0x4F1 (render-guard skips, healthy = 0) while
;--- this runs. On a RATEDIAG build 0x4FA carries the direct-DAC inferred
;--- rate >> 8 and should track the `rate` argument in mode 1.
;---
;--- TEST06 [mode] [rate] [seconds] [blocksize] [irq]
;---   mode       0 = 4-bit ADPCM, single-cycle (DSP cmd 75h) -- DMA + SB IRQ,
;---                  next block armed inside the ISR. Exercises DecodeADPCM
;---                  AND cv_rate, i.e. both users of the ISR scratch buffer.
;---              1 = direct-DAC (DSP cmd 10h) -- no DMA, no IRQ, no TC. Two
;---                  trapped port writes per sample, paced to `rate` in
;---                  bursts of one BIOS tick. Exercises cv_rate via
;---                  VSB_ReadDirectSamples.
;---   rate       sample rate in Hz              (default 11025)
;---   seconds    run length                     (default 5)
;---   blocksize  ADPCM bytes per block, mode 0  (default 256). vsb.c sets
;---                  useRef from opcode bit 0 on EVERY block, so the first
;---                  byte of each block is eaten as the reference sample and
;---                  a block yields (blocksize-1)*2 samples, not blocksize*2.
;---                  The report is in ADPCM bytes/sec, so that does not skew
;---                  it -- but it is why the pitch is not exactly rate/2.
;---   irq        emulated SB IRQ, 2..7, mode 0  (default 7 -- vsbpcm's
;---                  launchers set BLASTER=...I7, and hooking the wrong
;---                  vector just hangs waiting for a block that never
;---                  completes, so check BLASTER before assuming)
;---
;--- Reports the achieved rate and a stretch factor x100, same as TEST05:
;--- 100 = the engine keeps up, 50 = everything plays at half speed.
;---
;--- ESC aborts. Both payloads are deliberately buzzy rather than musical --
;--- a healthy run is a steady tone and a starved one audibly stutters.

;--- CPU directive order matters: .286 here keeps .MODEL tiny's segments
;--- 16-bit (USE16). The .386 that enables the 32-bit arithmetic below goes
;--- INSIDE .CODE, exactly as test01.asm does it -- putting it up here makes
;--- the segments USE32 and every offset the wrong width.
	.286
	.MODEL tiny
	.dosseg
	.STACK 400h
	option casemap:none

;--- SoundBlaster constants

BASEADDR           EQU 0220h       ;SoundBlaster base address
SBIRQ              EQU 7           ;default SB IRQ (override: 5th argument)
DMAchannel         EQU 1           ;SoundBlaster DMA channel

	include DMA.INC
	include SB.INC

;--- Not in SB.INC (it carries only the commands TEST01..TEST05 needed).
;--- 75h = 4-bit ADPCM, single-cycle, WITH reference byte. vsb.c derives
;--- bits from the opcode ( cmd & 2 ? 3 : 4 ) and useRef from bit 0, so the
;--- first byte of the first block is consumed as the reference sample and
;--- every byte after that is two packed nibbles.
DSP_ADPCM4REF_SNGL equ 075h
DSP_DIRECTDAC      equ 010h

;--- DMA CONTROLLER REGISTERS
WRITEMASK          EQU 00ah         ; Mask register
WRITEMODE          EQU 00bh         ; Mode register
CLEARFLIPFLOP      EQU 00ch
PAGE_CHN           EQU DMAPageReg(DMAchannel) ;Page register for DMAchannel
BASE_CHN           EQU DMABaseReg(DMAchannel) ;Base address register
COUNT_CHN          EQU DMACntReg(DMAchannel)  ;Count register

;--- DMA MODE: single mode, NO auto-init, read -- same as TEST05. Auto-init
;--- is deliberately absent: it is what makes a block "single-cycle".
WANTEDMODE         EQU DMA_MODE_SINGLE or DMA_MODE_SINGLECYCLE or DMA_MODE_READ

CStr macro text:vararg
local sym
	.const
sym db text,0
	.code
	exitm <offset sym>
endm

	.const

;--- ADPCM payload. Creative 4-bit nibbles are sign-magnitude: bit 3 is the
;--- sign, bits 2-0 the step magnitude, high nibble first. 33h is two +3
;--- steps, 0BBh two -3. Sixteen bytes of each = 64 samples per cycle, so at
;--- 11025 Hz this is a ~172 Hz buzz. The decoder's step size adapts, so the
;--- waveform is not a clean triangle and is not meant to be -- what matters
;--- is that it is continuous, so starvation is audible.
CreateADPCM macro
	repeat 32
	  repeat 16
	  db 033h
	  endm
	  repeat 16
	  db 0BBh
	  endm
	endm
endm

;--- 4 x 1024 bytes. Only WALKLEN of it is ever played; the spare copies
;--- exist so a run of WALKLEN bytes that does not straddle a 64K DMA page
;--- boundary can always be found (see the fixup in main).
SampleBuffer LABEL BYTE
	CreateADPCM
	CreateADPCM
	CreateADPCM
	CreateADPCM
SAMPLEBUFFERLENGTH equ $ - offset SampleBuffer
;--- bytes actually walked. Must be <= SAMPLEBUFFERLENGTH/2 for the
;--- page-straddle fixup below to be guaranteed to find room.
WALKLEN            equ 2048

;--- Direct-DAC payload: plain unsigned 8-bit, the TEST01 zigzag. No DMA is
;--- involved, so no page-straddle concern and no need for spare copies.
DacWave LABEL BYTE
SMPVAL = 80h
	repeat 64	  ;80-FF
	db SMPVAL
SMPVAL = SMPVAL + 2
	endm
SMPVAL = 0FEh
	repeat 128    ;FF-00
	db SMPVAL
SMPVAL = SMPVAL - 2
	endm
SMPVAL = 0
	repeat 64     ;00-7F
	db SMPVAL
SMPVAL = SMPVAL + 2
	endm
DACWAVELEN equ $ - offset DacWave

information     db 'TEST06 [mode] [rate] [seconds] [blocksize] [irq]',13,10
                db 'drives the two formats that bypass the passthrough tap',13,10
                db 'mode 0 = 4-bit ADPCM single-cycle (DSP 75h), DMA + IRQ',13,10
                db 'mode 1 = direct-DAC (DSP 10h), no DMA, no IRQ',13,10
                db 'defaults: mode 0, 11025 Hz, 5 s, 256-byte blocks, irq 7',13,10,'$'
running         db 'running... (ESC aborts)',13,10,'$'
sberror         db 'No SoundBlaster at base address 220h.',13,10,'$'
rateerror       db 'Invalid rate entered.',13,10,'$'
blkerror        db 'Invalid block size (1..2048).',13,10,'$'
irqerror        db 'Invalid IRQ (2..7).',13,10,'$'
modeerror       db 'Invalid mode (0 or 1).',13,10,'$'

	.data

oldInterrupt        dd 0
blocks              dd 0        ;completed ADPCM blocks (mode 0)
dacsamples          dd 0        ;direct-DAC samples emitted (mode 1)
bufpage             db 0        ;DMA page of SampleBuffer
bufoff              dw 0        ;DMA offset of SampleBuffer
playoff             dw 0        ;walking offset within the ADPCM buffer
dacoff              dw 0        ;walking offset within DacWave
blocksize           dw 256
rate                dw 11025
mode                dw 0
seconds             dw 5
pertick             dw 0        ;mode 1: samples emitted per BIOS tick
runticks            dw 0
elapsed             dw 0
timeconst           db 0
oldpic              db 0
stopflag            db 0        ;1 = time is up, ISR must not re-arm
irq                 dw SBIRQ    ;emulated SB IRQ actually hooked
vecoff              dw 0        ;(irq+8)*4 -- IVT offset of that IRQ
picand              db 0        ;AND mask that unmasks it at port 21h

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

;--- in: bx->string
;--- out: number in AX
;--- out: bx->behind number
;--- digits in CH

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

;--- Program the DMA controller and the DSP for ONE ADPCM block at playoff.
;--- Called from the main loop AND from the SB ISR, so it touches nothing but
;--- CS-relative data and leaves DS alone -- callers set DS=CS.
;--- Destroys AX, BX, CX, DX, SI.

ArmBlock proc near

		mov si, playoff
		mov cx, blocksize

;--- advance the play offset for the next block (wrap at the buffer end)
		mov ax, si
		add ax, cx
		cmp ax, WALKLEN
		jb  @F
		xor ax, ax
@@:
		mov playoff, ax

;--- 20-bit address of SampleBuffer+si
		mov bx, bufoff
		add bx, si
		mov al, bufpage
		adc al, 0
		mov ah, al				;AH = page

;--- MASK DMA CHANNEL
		mov al,DMAchannel
		add al,4
		out WRITEMASK,al
;--- CLEAR FLIPFLOP
		out CLEARFLIPFLOP,al
;--- WRITE TRANSFER MODE
		mov al,WANTEDMODE
		or al,DMAchannel
		out WRITEMODE,al
;--- WRITE PAGE NUMBER
		mov al,ah
		out PAGE_CHN,al
;--- WRITE BASEADDRESS
		mov ax,bx
		out BASE_CHN,al
		mov al,ah
		out BASE_CHN,al
;--- WRITE COUNT-1
		mov ax,cx
		dec ax
		push ax
		out COUNT_CHN,al
		mov al,ah
		out COUNT_CHN,al
;--- DEMASK CHANNEL
		mov al,DMAchannel
		out WRITEMASK,al
		pop cx					;CX = count-1

;--- DSP: 4-bit ADPCM with reference, single-cycle, length-1
		mov dx,BASEADDR+SB_DSPWRITE
		WAITWRITE
		mov al,DSP_ADPCM4REF_SNGL
		out dx,al
		WAITWRITE
		mov al,cl
		out dx,al
		WAITWRITE
		mov al,ch
		out dx,al
		ret
ArmBlock endp

;--- Emit one BIOS tick's worth of direct-DAC samples (mode 1). Each sample
;--- is DSP cmd 10h followed by the byte, i.e. TWO trapped port writes -- the
;--- guest side of the cost this mode exists to measure. vsb.c's CMD10NOWAIT
;--- clears the busy flag for cmd 10h, so WAITWRITE does not spin here.
;--- Destroys AX, CX, DX, SI.

EmitBurst proc near
		mov cx, pertick
		or cx, cx
		jz eb_done
eb_next:
		push cx
		mov si, dacoff
		mov al, DacWave[si]
		inc si
		cmp si, DACWAVELEN
		jb @F
		xor si, si
@@:
		mov dacoff, si
		mov ah, al				;WAITWRITE destroys AL, so park the sample
		mov dx, BASEADDR+SB_DSPWRITE
		WAITWRITE
		mov al, DSP_DIRECTDAC
		out dx, al
		WAITWRITE
		mov al, ah
		out dx, al
		inc dacsamples
		pop cx
		loop eb_next
eb_done:
		ret
EmitBurst endp

main proc c argc:word, argv:ptr

		RESET_DSP

;--- start msg
		mov dx,offset information
		mov ah,9
		int 21h

		cmp argc, 2
		jb options_done
		mov si, argv
		mov bx, [si+2]
		call getdec
		jc mode_invalid
		cmp ax, 2
		jnc mode_invalid
		mov mode, ax

		cmp argc, 3
		jb options_done
		mov bx, [si+4]
		call getdec
		jc rate_invalid
		mov rate, ax

		cmp argc, 4
		jb options_done
		mov bx, [si+6]
		call getdec
		jc options_done
		or ax, ax
		jz options_done
		mov seconds, ax

		cmp argc, 5
		jb options_done
		mov bx, [si+8]
		call getdec
		jc blk_invalid
		or ax, ax
		jz blk_invalid
		cmp ax, WALKLEN
		ja blk_invalid
		mov blocksize, ax

		cmp argc, 6
		jb options_done
		mov bx, [si+10]
		call getdec
		jc irq_invalid
		cmp ax, 2
		jc irq_invalid
		cmp ax, 8
		jnc irq_invalid
		mov irq, ax

options_done:

;--- derive the IVT offset and the PIC unmask from the chosen IRQ
		mov ax, irq
		add ax, 8
		shl ax, 2
		mov vecoff, ax
		mov cx, irq
		mov ax, 1
		shl ax, cl
		not al
		mov picand, al

;--- run length in BIOS ticks: seconds * 182 / 10
		mov ax, seconds
		mov dx, 182
		mul dx
		mov bx, 10
		div bx
		mov runticks, ax

;--- mode 1 pacing: samples per BIOS tick = rate * 10 / 182. rate * 10
;--- overflows 16 bits above 6553 Hz, so the mul result is taken as dx:ax.
		mov ax, rate
		mov bx, 10
		mul bx
		mov bx, 182
		div bx
		mov pertick, ax

;--- ENABLE SB SPEAKERS (for all SBs < SB16)
		mov dx,BASEADDR+SB_DSPWRITE
		WAITWRITE
		mov al,DSP_ENABLESPEAKER
		out dx,al

;--- SET TIMECONSTANT. Direct-DAC does not use it -- the driver infers that
;--- rate from how many samples arrive per tick -- but a real guest sets it
;--- anyway, and on a RATEDIAG build it populates 0x4F2/0x4F3 so the
;--- time-constant reading can be compared against 0x4FA's inference.
		mov dx,BASEADDR+SB_DSPWRITE
		WAITWRITE
		mov al,DSP_SETTIMECONST
		out dx,al
		WAITWRITE

		mov cx,dx
		mov ax,lowword 1000000
		mov dx,highword 1000000
		mov bx,rate
		cmp bx,100
		jb rate_invalid
		div bx
		cmp ah,00h
		jnz rate_invalid
		neg al
		mov timeconst,al
		mov dx,cx
		out dx,al

;--- Direct-DAC needs no DMA channel, no SB IRQ and no buffer address: skip
;--- the whole setup and go straight to the emit loop.
		cmp mode, 1
		jz dac_run

;--- SETUP IRQ
		xor ax,ax
		mov es,ax
		mov si,vecoff
		mov ax,es:[si+0]
		mov word ptr [oldInterrupt+0],ax
		mov ax,es:[si+2]
		mov word ptr [oldInterrupt+2],ax
		cli
		mov ax,OFFSET SB_IRQ
		mov es:[si+0],ax
		mov es:[si+2],cs
		sti

;--- unmask SB IRQ
		in al, 21h
		mov oldpic, al
		and al, picand
		out 21h, al

;------------------------------------------------
; 20-bit linear address of SampleBuffer -> bufpage:bufoff
;------------------------------------------------
		mov si,offset SampleBuffer
		mov ax,ds
		rol ax,4
		mov bl,al
		and bl,00fh
		and al,0f0h
		add si,ax
		adc bl,0
		mov bufpage, bl
		mov bufoff, si

;--- 64K DMA PAGE STRADDLE. The 8237 wraps within a page instead of carrying
;--- into the next one, so a block crossing the boundary plays the wrong
;--- bytes. Keep the whole WALKLEN run on one side of it: if this page has
;--- less than WALKLEN left, restart at the boundary -- the buffer is
;--- 2*WALKLEN long, so whatever does not fit before it does fit after.
		mov ax, si
		neg ax					;AX = 10000h - si = bytes left in this page
		jz  straddle_fix		;si was 0: a full page is left, nothing to do
		cmp ax, WALKLEN
		jnc straddle_ok
straddle_fix:
		or  ax, ax
		jz  straddle_ok
		mov bufoff, 0
		inc bufpage
straddle_ok:
		mov playoff, 0

		mov dx,offset running
		mov ah,9
		int 21h

;--- arm the first block and start the clock
		call ArmBlock

		mov ah,0
		int 1ah					;CX:DX = BIOS tick count
		mov bx,dx				;BX = start tick (low word is enough)

waitloop:
;--- time up?
		push bx
		mov ah,0
		int 1ah
		pop bx
		mov ax,dx
		sub ax,bx				;elapsed ticks (wrap-safe over a run this short)
		mov elapsed,ax
		cmp ax,runticks
		jnc timeup

;--- ESC also stops it
		mov ah,01
		int 16h
		jz waitloop
		mov ah,00
		int 16h
		cmp ah,1
		jnz waitloop

timeup:
		mov stopflag,1			;stop the ISR re-arming before we tear down
		RESET_DSP

;--- restore PIC mask
		mov al,oldpic
		out 21h,al

;--- restore IRQ
		xor ax,ax
		mov es,ax
		mov si,vecoff
		cli
		mov ax,word ptr [oldInterrupt+0]
		mov es:[si+0],ax
		mov ax,word ptr [oldInterrupt+2]
		mov es:[si+2],ax
		sti

;--- report (mode 0)
		invoke printf, CStr("mode=0 ADPCM blocksize=%u rate=%u irq=%u timeconst=%u",10), blocksize, rate, irq, timeconst
		invoke printf, CStr("blocks=%lu elapsed=%u ticks",10), blocks, elapsed

;--- bytes/sec = blocks * blocksize * 182 / (elapsed * 10). Note this is the
;--- ADPCM byte rate, i.e. half the SAMPLE rate at 4 bits per sample.
		mov eax, blocks
		movzx edx, blocksize
		mul edx
		mov edx, 182
		mul edx
		movzx ecx, elapsed
		imul ecx, ecx, 10
		or ecx, ecx
		jz nodiv
		xor edx, edx
		div ecx
		push eax
		invoke printf, CStr("achieved %lu ADPCM bytes/sec (2 samples each)",10), eax
		pop eax

;--- stretch factor x100 = wanted / achieved, wanted = rate/2 ADPCM bytes/s
		mov ecx, eax
		or ecx, ecx
		jz nodiv
		movzx eax, rate
		shr eax, 1
		imul eax, eax, 100
		xor edx, edx
		div ecx
		invoke printf, CStr("stretch factor x100 = %lu  (100 = keeps up)",10), eax
		jmp nodiv

;--------------------------------------------------------------------
; mode 1: direct-DAC. No DMA, no IRQ -- just cmd 10h per sample, paced
; to one BIOS tick's worth per burst so the average tracks `rate`.
;--------------------------------------------------------------------
dac_run:
		mov dx,offset running
		mov ah,9
		int 21h

		mov ah,0
		int 1ah
		mov bx,dx				;BX = start tick

dac_loop:
		push bx
		mov ah,0
		int 1ah
		pop bx
		mov di,dx				;DI = tick this burst started in

		call EmitBurst

;--- time up?
		push bx
		mov ah,0
		int 1ah
		pop bx
		mov ax,dx
		sub ax,bx
		mov elapsed,ax
		cmp ax,runticks
		jnc dac_timeup

;--- ESC also stops it
		mov ah,01
		int 16h
		jz dac_wait
		mov ah,00
		int 16h
		cmp ah,1
		jz dac_timeup

;--- wait for the tick to advance. If the burst already overran a tick this
;--- falls straight through, so a host that cannot keep up simply reports a
;--- low achieved rate instead of drifting.
dac_wait:
		push bx
		mov ah,0
		int 1ah
		pop bx
		cmp dx,di
		jz dac_wait
		jmp dac_loop

dac_timeup:
		RESET_DSP

		invoke printf, CStr("mode=1 direct-DAC rate=%u pertick=%u timeconst=%u",10), rate, pertick, timeconst
		invoke printf, CStr("samples=%lu elapsed=%u ticks",10), dacsamples, elapsed

;--- samples/sec = dacsamples * 182 / (elapsed * 10)
		mov eax, dacsamples
		mov edx, 182
		mul edx
		movzx ecx, elapsed
		imul ecx, ecx, 10
		or ecx, ecx
		jz nodiv
		xor edx, edx
		div ecx
		push eax
		invoke printf, CStr("achieved %lu samples/sec (wanted %u)",10), eax, rate
		pop eax

		mov ecx, eax
		or ecx, ecx
		jz nodiv
		movzx eax, rate
		imul eax, eax, 100
		xor edx, edx
		div ecx
		invoke printf, CStr("stretch factor x100 = %lu  (100 = keeps up)",10), eax

nodiv:
done:
		ret

;--- error 'no sb found'
RESET_ERROR:
		mov dx, offset sberror
		mov ah, 9
		int 21h
		jmp done
rate_invalid:
		mov dx, offset rateerror
		mov ah, 9
		int 21h
		jmp done
blk_invalid:
		mov dx, offset blkerror
		mov ah, 9
		int 21h
		jmp done
irq_invalid:
		mov dx, offset irqerror
		mov ah, 9
		int 21h
		jmp done
mode_invalid:
		mov dx, offset modeerror
		mov ah, 9
		int 21h
		jmp done
main endp

;--- SB IRQ (mode 0 only). Entered either from the real PIC or -- under
;--- vsbpcm -- as a synchronous "int 8+irq" from inside the driver's own RTC
;--- ISR, so DS is whatever the driver was running with. Set it ourselves.

SB_IRQ proc
		push ax
		push bx
		push cx
		push dx
		push si
		push ds
		mov ax,cs
		mov ds,ax

		mov dx,BASEADDR+SB_DSPSTATUS	;IRQ ACKNOWLEDGE
		in al,dx

		inc blocks

		cmp stopflag,0
		jnz irq_eoi
		call ArmBlock			;re-arm right here, in the ISR
irq_eoi:
		mov al,20h
		out 20h,al

		pop ds
		pop si
		pop dx
		pop cx
		pop bx
		pop ax
		IRET
SB_IRQ endp

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
