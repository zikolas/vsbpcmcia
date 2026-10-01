
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

;--- LPTSTUB: the /LPT variant of this stub (rmcode3.bin, installed only with
;--- /LPT, in its own DOS block -- it does not fit the PSP). It drives an
;--- OPL3LPT from V86 itself; see islpt. Never built for CARD_TP755, whose
;--- 388h fast path belongs to dbopl and whose C side refuses /LPT.
ifdef LPTSTUB
if HANDLE_IN_388H_DIRECTLY
	.err <LPTSTUB and CARD_TP755 exclude each other>
endif
endif

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
wFmSB dw ?	; emulated SB base whose FM aliases (+0..3, +8/9) are forwarded
		; in-stub to the real OPL3 at 388h; 0FFFFh = not armed
wDspWS dw ?	; emulated DSP write-status port (base+0Ch), answered in-stub
		; from bDspWS; 0FFFFh = not armed
bDspWS db ?	; DSP_Read0C's 2-bit busy counter (vsb.c owns every write)
bDspPad db ?	; pad: keeps codev86 at an even offset
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
ifdef LPTSTUB
wLpt  dw ?	; /LPT: OPL3LPT data port; 0 = not armed (ptrap.c: struct rmlpt)
bLptDly db ?	; control-port reads after each strobe (SBELPTDLY, def 6)
bLptCli db ?	; 1 = interrupts off around each write (SBELPTCLI=0 clears)
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
vars rmcode1 <0,0,0,20h,0,0FFFFh,0FFFFh,0,0,0,0,0,0,0>
elseifdef LPTSTUB
vars rmcode1 <0,0,0,20h,0,0FFFFh,0FFFFh,0,0,0,6,1>
else
vars rmcode1 <0,0,0,20h,0,0FFFFh,0FFFFh,0,0>
endif


;--- Multi-byte gate + the 0A0h short-circuit: BOTH builds need these, so
;--- they sit outside HANDLE_IN_388H_DIRECTLY. 0A0h is trapped purely so a
;--- word/dword access STARTING there reaches ptrap.c's decomposer (Jemm and
;--- hdpmi report only the start port, so the second byte -- the slave IMR --
;--- would otherwise hit real hardware behind VPIC's back and mask the engine
;--- IRQ). A plain BYTE access to 0A0h is pure passthrough and must never pay
;--- an RMCB round-trip: without this short-circuit every guest slave-PIC EOI
;--- on an IRQ>=8 box (the whole RTC-pump fleet) would switch worlds.
	test cl, 38h		; word/dword/string access? in-stub paths are
	jnz normal			; byte-only: ptrap.c decomposes those per-port
	cmp dx, 0A0h		; slave PIC cmd port, trapped ONLY for that
	je is20				; decomposer: byte access = passthrough (QPI sim)

ife RMPICTRAPDYN
	cmp dx, [vars.wPICp]
	je is20
endif

;--- SB-base FM aliases (base+0..3, base+8/9). On a card with real FM behind
;--- an UNTRAPPED 388h, ptrap.c arms wFmSB and the forward happens HERE, as a
;--- plain IN/OUT in V86 -- no RMCB round trip. Never armed under FMSHIM or
;--- FMVOL (388h stays trapped there; a direct OUT would recurse into this
;--- stub) nor with the software OPL -- except in the LPTSTUB variant with
;--- wLpt armed, where isfm hands the alias to islpt and never touches 388h.
;--- Era drivers pad every register write
;--- with 6-35 status reads through the same alias, so for a real-mode guest
;--- driving music via the Sound Blaster base this is the whole FM trap tax.
	cmp word ptr [vars.wFmSB], 0FFFFh
	je nofm
	push ax
	mov ax, dx
	sub ax, [vars.wFmSB]
	cmp ax, 3			; base+0..3
	jbe isfm
	sub ax, 8
	cmp ax, 1			; base+8/9
	jbe isfm
	pop ax
nofm:
ifdef LPTSTUB
;--- /LPT armed: 388h-38Bh are the OPL3LPT's, served in islpt
	cmp word ptr [vars.wLpt], 0
	je nolpt
	push ax
	mov ax, dx
	sub ax, 388h
	cmp ax, 3
	pop ax				; POP leaves the flags alone
	jbe islpt
nolpt:
endif
;--- DSP write-status reads (base+0Ch, "is the DSP busy?"): a driver polls
;--- this before EVERY command or data byte it writes, so answering it here
;--- roughly halves the round trips of a DSP command sequence.
	cmp dx, [vars.wDspWS]
	je isws

if HANDLE_IN_388H_DIRECTLY
	cmp dx, 388h
	je is388
	cmp dx, 389h
	je is389
endif

normal:
	jmp [vars.rmcb]

;--- FM alias: rewrite the port to 388h + (port and 3) and do the access.
;--- The base is 16-aligned, so base+8/9 land on 388h/389h.
isfm:
	pop ax
ifdef LPTSTUB
	cmp word ptr [vars.wLpt], 0
	jne islpt			; /LPT: the alias goes to the OPL3LPT
endif
	push dx
	and dx, 3
	add dx, 388h
	test cl, 4			; OUT instr?
	jz isfmin
	out dx, al
	pop dx
	clc				; handled
	retf
isfmin:
	in al, dx
	pop dx
	clc
	retf

ifdef LPTSTUB
;--- /LPT: the OPL3LPT driven from V86, no RMCB. Entered for 388h-38Bh and
;--- for the SB-base FM aliases alike, with the guest's own port in DX: only
;--- bits 0-1 matter (1 = data port, 2 = second register array), and the
;--- aliases carry the same bits because the SB base is 16-aligned.
;--- Reads answer from the status cache (_0005) that fmshim.c keeps current.
;--- Index 4 (timer control in the first array, 4-op select in the second)
;--- and the data write after it go to fmshim.c, so its timer model sees
;--- every timer command; it forwards those to the LPT itself. The index
;--- shadow (_0004) is shared with fmshim.c, which updates it too.
islpt:
	test cl, 4			; OUT instr?
	jz islptin
	test dl, 1
	jnz islptdat
	mov [vars._0004], al	; index write: shadow it first, on both paths
	cmp al, 4
	je normal
	push bx
	mov bx, 0D09h		; first array: control 13/9/13
	test dl, 2
	jz @F
	mov bx, 0501h		; second array: control 5/1/5
@@:
	call lptout
	pop bx
	clc
	retf
islptdat:
	cmp [vars._0004], 4
	je normal
	push bx
	mov bx, 0C08h		; value: control 12/8/12
	call lptout
	pop bx
	clc
	retf
islptin:
	mov al, [vars._0005]
	clc
	retf

;--- AL onto the LPT data lines, then BH, BL, BH to the control port, then
;--- bLptDly control-port reads (the OPL3 needs 3.3 us after each write; 6
;--- by default). The same sequence as fmshim.c's LptPulse, after FastDoom's
;--- OPL3LPT driver. With bLptCli set, interrupts are off throughout, as on
;--- the RMCB path: a guest ISR writing FM in the middle would change the
;--- data lines under the pulse. All registers preserved.
lptout:
	pushf
	cmp byte ptr [vars.bLptCli], 0
	je @F
	cli
@@:
	push ax
	push cx
	push dx
	mov dx, [vars.wLpt]
	out dx, al
	inc dx
	inc dx
	mov al, bh
	out dx, al
	mov al, bl
	out dx, al
	mov al, bh
	out dx, al
	mov cl, [vars.bLptDly]
	xor ch, ch
	jcxz lptdone
@@:
	in al, dx
	loop @B
lptdone:
	pop dx
	pop cx
	pop ax
	popf
	ret
endif

;--- DSP write-status: the same 2-bit counter vsb.c's DSP_Read0C keeps --
;--- ready (7Fh) three reads out of four, busy (0FFh) on the fourth -- and
;--- vsb.c resets it on every write to the port. Writes still trap; the
;--- counter cell is shared (PTRAP_DspStatusCell), so both sides agree.
isws:
	test cl, 4			; OUT instr?
	jnz normal			; the write side is vsb.c's
	inc byte ptr [vars.bDspWS]
	mov al, 7Fh
	test byte ptr [vars.bDspWS], 3
	jnz @F
	mov al, 0FFh
@@:
	clc
	retf


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
