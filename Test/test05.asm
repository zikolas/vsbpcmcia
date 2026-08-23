;--- TEST05: a TORRENT of small 8-bit single-cycle blocks ( DSP cmd 0x14 ).
;---
;--- Derived from Baron-von-Riedesel's TEST01 in this directory (same
;--- structure, DMA/SB setup and macros); the chaining, the re-arm modes and
;--- the throughput report are the additions. No separate copyright is
;--- claimed over it -- it is a bench tool for this repo's own driver.
;---
;--- WHY: Duke Nukem II's intro SFX reach the driver as a stream of ~12-byte
;--- single-cycle blocks, each one ended by a TC-IRQ that prompts the guest
;--- to program the next. Under vsbpcm's passthrough tap that plays back
;--- stretched, and every measurement so far has needed the game itself --
;--- which wants 560K, so the box has to run comrade-less with a human at
;--- the keyboard. This reproduces the same block pattern in a few KB, so the
;--- bench is scriptable over the link.
;---
;--- TEST05 [blocksize] [rate] [mode] [seconds] [irq]
;---   blocksize  bytes per single-cycle block   (default 12 = Duke's size)
;---   rate       sample rate in Hz              (default 21376 = Duke's)
;---   mode       0 = re-arm INSIDE the SB ISR   (default; what a driver that
;---                  keeps the DMA fed does, and what lets vsbpcm's tap loop
;---                  carry on to the next block within one RTC tick)
;---              1 = re-arm from the MAIN LOOP  (the ISR only sets a flag --
;---                  the tap loop cannot chase this one, so it is the
;---                  worst-case shape)
;---   seconds    run length                     (default 5)
;---   irq        emulated SB IRQ, 2..7          (default 7 -- vsbpcm's
;---                  launchers set BLASTER=...I7, and hooking the wrong
;---                  vector just hangs waiting for a block that never
;---                  completes, so check BLASTER before assuming)
;---
;--- Reports blocks completed and the achieved byte rate. Divide that by the
;--- requested rate and you have the stretch factor directly: 1.00 = the
;--- engine keeps up, 0.50 = everything plays at half speed.
;---
;--- The waveform is a zigzag walked continuously across blocks, so a healthy
;--- run is a steady tone and a starved one audibly stutters.

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

;--- PIC masks and the vector offset are derived from the runtime irq
;--- (see SetupIrqVars); SBIRQ above is only the default.

;--- DMA CONTROLLER REGISTERS
WRITEMASK          EQU 00ah         ; Mask register
WRITEMODE          EQU 00bh         ; Mode register
CLEARFLIPFLOP      EQU 00ch
PAGE_CHN           EQU DMAPageReg(DMAchannel) ;Page register for DMAchannel
BASE_CHN           EQU DMABaseReg(DMAchannel) ;Base address register
COUNT_CHN          EQU DMACntReg(DMAchannel)  ;Count register

;--- DMA MODE: single mode, NO auto-init, read. Auto-init is deliberately
;--- absent -- it is what makes a block "single-cycle", and the whole point
;--- of this test is the per-block guest round-trip that auto-init avoids.
WANTEDMODE         EQU DMA_MODE_SINGLE or DMA_MODE_SINGLECYCLE or DMA_MODE_READ

CStr macro text:vararg
local sym
	.const
sym db text,0
	.code
	exitm <offset sym>
endm

	.const

;--- 4 x 1024 bytes of zigzag. Only WALKLEN of it is ever played; the
;--- spare copies exist so a run of WALKLEN bytes that does not straddle a
;--- 64K DMA page boundary can always be found (see the fixup in main).
CreateData macro
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
endm

SampleBuffer LABEL BYTE
	CreateData
	CreateData
	CreateData
	CreateData
SAMPLEBUFFERLENGTH equ $ - offset SampleBuffer
;--- bytes actually walked. Must be <= SAMPLEBUFFERLENGTH/2 for the
;--- page-straddle fixup below to be guaranteed to find room.
WALKLEN            equ 2048

information     db 'TEST05 [blocksize] [rate] [mode] [seconds] [irq]',13,10
                db 'plays a torrent of small 8bit single-cycle blocks',13,10
                db 'defaults: 12 bytes, 21376 Hz, mode 0 (re-arm in ISR), 5 s, irq 7',13,10
                db 'mode 0 = re-arm inside the SB ISR, 1 = from the main loop',13,10,'$'
running         db 'running...',13,10,'$'
sberror         db 'No SoundBlaster at base address 220h.',13,10,'$'
rateerror       db 'Invalid rate entered.',13,10,'$'
blkerror        db 'Invalid block size (1..1024).',13,10,'$'
irqerror        db 'Invalid IRQ (2..7).',13,10,'$'

	.data

oldInterrupt        dd 0
blocks              dd 0        ;completed blocks
bufpage             db 0        ;DMA page of SampleBuffer
bufoff              dw 0        ;DMA offset of SampleBuffer
playoff             dw 0        ;walking offset within the buffer
blocksize           dw 12
rate                dw 21376
mode                dw 0
seconds             dw 5
runticks            dw 0
elapsed             dw 0
timeconst           db 0
oldpic              db 0
stopflag            db 0        ;1 = time is up, ISR must not re-arm
armflag             db 0        ;mode 1: main loop owes an arm
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

;--- Program the DMA controller and the DSP for ONE block at playoff.
;--- Called from the main loop AND (mode 0) from the SB ISR, so it touches
;--- nothing but CS-relative data and leaves DS alone -- callers set DS=CS.
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

;--- DSP: 8bit mono single-cycle, length-1
		mov dx,BASEADDR+00Ch
		WAITWRITE
		mov al,DSP_DMADAC8BIT
		out dx,al
		WAITWRITE
		mov al,cl
		out dx,al
		WAITWRITE
		mov al,ch
		out dx,al
		ret
ArmBlock endp

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
		jc blk_invalid
		or ax, ax
		jz blk_invalid
		cmp ax, WALKLEN
		ja blk_invalid
		mov blocksize, ax

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
		mov mode, ax

		cmp argc, 5
		jb options_done
		mov bx, [si+8]
		call getdec
		jc options_done
		or ax, ax
		jz options_done
		mov seconds, ax

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

;--- ENABLE SB SPEAKERS (for all SBs < SB16)
		mov dx,BASEADDR+00Ch
		WAITWRITE
		mov al,DSP_ENABLESPEAKER
		out dx,al

;--- SET TIMECONSTANT
		mov dx,BASEADDR+00Ch
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
;--- mode 1: the ISR only flags, the main loop does the arming
		cmp mode, 1
		jnz no_mainarm
		cmp armflag, 0
		jz no_mainarm
		mov armflag, 0
		call ArmBlock
no_mainarm:

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

;--- report
		invoke printf, CStr("blocksize=%u rate=%u mode=%u irq=%u timeconst=%u",10), blocksize, rate, mode, irq, timeconst
		invoke printf, CStr("blocks=%lu elapsed=%u ticks",10), blocks, elapsed

;--- bytes/sec = blocks * blocksize * 182 / (elapsed * 10)
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
		invoke printf, CStr("achieved %lu bytes/sec (wanted %u)",10), eax, rate
		pop eax

;--- stretch factor x100 = rate * 100 / achieved
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
main endp

;--- SB IRQ. Entered either from the real PIC or -- under vsbpcm -- as a
;--- synchronous "int 8+irq" from inside the driver's own RTC ISR, so DS is
;--- whatever the driver was running with. Set it ourselves.

SB_IRQ proc
		push ax
		push bx
		push cx
		push dx
		push si
		push ds
		mov ax,cs
		mov ds,ax

		mov dx,BASEADDR+00Eh	;IRQ ACKNOWLEDGE
		in al,dx

		inc blocks

		cmp stopflag,0
		jnz irq_eoi
		cmp mode,1
		jz irq_flag
		call ArmBlock			;mode 0: re-arm right here, in the ISR
		jmp irq_eoi
irq_flag:
		mov armflag,1			;mode 1: leave it to the main loop
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
