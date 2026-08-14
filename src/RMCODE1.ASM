
;--- 16-bit helper code for PTRAP.C, real-mode part.
;--- if IRQs > 8 are to be supported, this code needs adjustments.

	.286
	option proc:private
	option casemap:none

ifdef CARD_TP755
HANDLE_IN_388H_DIRECTLY equ 1 ; TP755: answer OPL delay reads in the v86 stub
else
HANDLE_IN_388H_DIRECTLY equ 0 ; std=0, must match #define in ptrap.c
endif
RMPICTRAPDYN            equ 0 ; std=0, must match #define in ptrap.c

OPLRING_ENTRIES         equ 1024 ; must match OPLRING_ENTRIES in ptrap.c
OPLRING_MASK            equ OPLRING_ENTRIES-1

;--- if RMPICTRAPDYN is 0, the PIC port 0020h is permanently trapped.
;--- if RMPICTRAPDYN is 1, the PIC port 0020h is trapped only while SB IRQ 2/5/7
;--- is emulated. This is quite some overhead.

;--- rmcode1 struct - if it's changed, PTRAP.C must be adjusted!

rmcode1 struct
rmcb  dd ?	; rmcb to vsbhda port trap handler
_0004 db ?	; current OPL index (stub shadow; routes timer-family writes)
_0005 db ?	; 0x388 status cache, maintained by ptrap.c (TP755)
wPICp dw ?	; PIC port to trap ( 0020h or 0ffffh )
dwQPI dd ?	; QPI entry Jemm/Qemm, for "untrapped io"
if HANDLE_IN_388H_DIRECTLY
;--- OPL write ring (SPSC, v86 stub produces / SNDISR drains): each entry is
;--- one complete register write, lo=value hi=index. The ring itself lives in
;--- sc_tp755's DOS block (patched in at init) -- the PSP has no room.
rseg  dw ?	; real-mode segment of the ring
rhead dw ?	; producer slot (stub)
rtail dw ?	; consumer slot (ptrap.c drain)
rfull db ?	; ring-full events seen by the stub (wraps at 256; ptrap.c
			; mirrors it to IAC 4F7 -- "did we fall back to sync at all")
dum3  db ?	; pad: keeps codev86 at an even offset
endif
rmcode1 ends

;--- ring: OPLRING_ENTRIES x 2 bytes. Was 256 with byte-wide slot indices
;--- (wrapping mod 256 for free), which MEASURED FULL on the bench: MI1
;--- crescendos pinned the occupancy high-water at 255/255 (IAC 4FE), i.e.
;--- the ring saturated and those writes fell back to the synchronous trap
;--- path -- the exact cost the ring exists to avoid, arriving precisely when
;--- the load peaks. The codec IRQ (the only drain) runs at ~43 Hz = ~23 ms
;--- per period, and iMUSE peaks near 14k OPL writes/s = ~330 per period, so
;--- 256 was ~30% short. 1024 buys 3x headroom for one AND per write.

;--- this code runs in real-mode.

_TEXT segment word public 'CODE'

	assume ds:nothing, ss:nothing

if HANDLE_IN_388H_DIRECTLY
vars rmcode1 <0,0,0,20h,0,0,0,0,0,0>
else
vars rmcode1 <0,0,0,20h,0>
endif

if HANDLE_IN_388H_DIRECTLY
	test cl, 38h		; word/dword/string access? in-stub paths are
	jnz normal			; byte-only: ptrap.c decomposes those per-port
						; (Jemm reports only the START port of a multi-
						; byte access -- see the 0xA0/IMR clobber notes)
	cmp dx, 0A0h		; slave PIC cmd port, trapped ONLY for that
	je is20				; decomposer: byte access = passthrough (QPI sim)
endif

ife RMPICTRAPDYN
	cmp dx, [vars.wPICp]
	je is20
endif

if HANDLE_IN_388H_DIRECTLY
	cmp dx, 388h
	je is388
	cmp dx, 389h
	je is389
normal:
endif

	jmp [vars.rmcb]

if HANDLE_IN_388H_DIRECTLY

;--- the OPL fast path. Reads never leave the stub (status cache / 0FFh).
;--- Writes: the DX4/75 bench proved the per-write RMCB round-trip is the FM
;--- killer (a PIT-driven music handler whose OPL writes cost more than its
;--- own period starves IRQ10 outright -- saturation meter 4FD hit 65-77
;--- polls/tick). So non-timer register writes are BUFFERED: the index write
;--- costs one stub visit and no mode switch at all; the data write appends
;--- one (index,value) entry to the SPSC ring drained by SNDISR each tick.
;--- Timer-family traffic (index 2/3/4) stays fully synchronous -- ptrap.c
;--- drains the ring before applying it, so order and detect semantics are
;--- exactly vopl3.cpp's.

is388:
	test cl, 4			; OUT instr?
	jz is388in
	mov [vars._0004], al	; ALWAYS track the index -- the following data
	cmp al, 2			; write routes on it (a timer index that skipped
	jb @F				; this store sent timer DATA into the ring as a
	cmp al, 4			; reg-0 write: killed MI1's AdLib detect)
	jbe normal			; timer-family index: synchronous path
@@:
	retf				; non-timer: tracked, zero round-trips
is388in:
	mov al, [vars._0005]; status cache -- ptrap.c recomputes it from
	retf				; vopl3.cpp after every OPL trap it handles

is389:
	test cl, 4			; OUT instr?
	jz is389in
	cmp [vars._0004], 2
	jb is389buf
	cmp [vars._0004], 4	; timer data write: synchronous (drain-first)
	jbe normal
is389buf:
	cmp [vars.rseg], 0	; ring not armed yet (or template just re-copied):
	je normal			; take the synchronous path -- NEVER touch seg 0
	push bx
	push es
	mov es, [vars.rseg]
	mov bx, [vars.rhead]
	shl bx, 1
	push ax
	mov ah, [vars._0004]
	mov es:[bx], ax		; entry: lo=value, hi=index
	pop ax
	shr bx, 1
	inc bx
	and bx, OPLRING_MASK	; slot index no longer wraps for free (>256)
	cmp bx, [vars.rtail]
	je @F				; full: entry uncommitted -- take the sync path
	mov [vars.rhead], bx	; commit
	pop es
	pop bx
	retf
@@:
	inc byte ptr [vars.rfull]	; count it: a full ring is the one condition
	pop es						; that puts writes back on the slow path
	pop bx
	jmp normal			; C drains the committed entries, then applies this
is389in:
	mov al, 0FFh		; IN 389h: the OPL reg file is write-only; era
	retf				; drivers read it purely as delay padding after
						; writes (chocolate-doom OPL notes) -- answer
						; like an empty bus, zero round-trips
endif

ife RMPICTRAPDYN

;--- This code runs when NO SB IRQ (2/5/7) is emulated.
;--- Since port 0020h is permanently trapped, QPI must be called
;--- to execute the IN/OUT instruction.
;--- 02/2024: fixed: register AH/AX preserved for IN/OUT

is20:
	push bx
	xchg ax, bx
	test cl, 4			; OUT instr?
	.386
	setnz al			; ax=1a00h is "sim IN", 1a01h = "sim OUT"
	.286
	mov ah, 1ah
	call [vars.dwQPI]	; call QPIEmu/Qemm
	xchg ax, bx
	pop bx
	retf
endif

_TEXT ends

	end
