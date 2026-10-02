;==========================================================================
;  VSBPCMJ - ring-0 companion for VSBPCMCIA, as a JLM (Jemm Loadable Module).
;
;    JLOAD VSBPCMJ.DLL       after QPIEMU, before VSBPCM; VSBPCM arms it
;    JLOAD -u VSBPCMJ.DLL    unload (refused while VSBPCM has it armed)
;
;  WHY. VSBPCM traps V86 port accesses through QPI, i.e. Jemm's QPIEMU, and
;  QPIEMU runs VSBPCM's real-mode stub as a nested execution for every
;  single access: Begin_Nest_Exec, Simulate_Far_Call, Resume_Exec, with the
;  RMCB into protected mode behind most of them. Two kinds of guest traffic
;  are too dense for that:
;   - FM. A real-mode OPL driver writes thousands of registers a second and
;     pads each write with 6-35 status reads. Theme Hospital's Miles driver
;     runs in V86 (the MSS driver called through DPMI), so all of its FM
;     takes the QPI path.
;   - direct DAC. DSP command 10h plus one sample byte, two trapped writes
;     per sample, from a timer interrupt at several kHz (Another World).
;  The QPI stub also never sees the guest's CS:IP, so it cannot patch the
;  delay reads out of the guest's code. A JLM's I/O handler runs at ring 0
;  with the client registers in hand, and costs one fault per access.
;
;  WHAT IT OWNS, once VSBPCM arms it (VSBPCM then leaves these ports off
;  its QPI traps; protected-mode guests still reach VSBPCM through HDPMI):
;   - FM, when VSBPCM runs its FM shim (no FM chip, /FMSHIM or /LPT) or
;     its software OPL3 (the builds with dbopl): 388h-38Bh and the
;     SB-base aliases (base+0..3, base+8/9). Status reads come from the
;     timer model of src/FMSHIM.C; with /LPT every index and data write
;     also goes to the OPL3LPT, and for the software OPL3 every data
;     write goes into the FM ring in the shared block, which VSBPCM
;     replays into its OPL3 before it renders. A status read while the
;     last index written is 20h or above is delay padding: its IN AL,DX
;     (ECh) is rewritten to NOP (90h) in the guest's code.
;   - direct DAC: the DSP write port (base+0Ch). Write-status reads are
;     answered from the counter VSBPCM's stub and vsb.c share; command 10h
;     and its sample byte go into a ring that VSBPCM drains each tick;
;     every other write is handed to VSBPCM's stub exactly as QPIEMU
;     would hand it over.
;
;  V86 API (entry from INT 2Fh AX=1684h BX=5650h -> ES:DI, AL=0):
;    AX=0  status: AX = version (high byte major), BX = armed flags,
;          EDX = linear address of the shared block (0 = not armed)
;    AX=1  arm: EDX = linear address of the shared block (src/JLMSHARE.H).
;          CF + AX: 1 already armed, 2 bad block, 3 port DX already trapped
;    AX=2  disarm: untrap everything
;
;  Build (tools/buildjlm.sh): JWasm -coff, wlink, then tools/pe2px.py.
;  JLM.INC is Jemm's (github.com/Baron-von-Riedesel/Jemm) and is not in
;  this repository.
;
;  (C) 2026 zikolas. GNU General Public License v2 - see COPYING.
;  Original work. The module shape (DDB, V86 API, DllMain) follows Jemm's
;  public-domain QPIEMU and IOTRAP samples, and forward repeats QPIEMU's
;  nested-execution call. The OPL3LPT write sequence is FastDoom's
;  (ns_sbmus.c), as in src/FMSHIM.C; the delay-read rule is the one ADLiPT
;  (github.com/pdewacht/adlipt) applies to V86 guests.
;==========================================================================
        .386
        .model flat, stdcall

        .nolist
        include jlm.inc
        .list

DEVICE_ID   equ 5650h
VER_MAJOR   equ 0
VER_MINOR   equ 5

DLL_PROCESS_ATTACH  equ 1
DLL_PROCESS_DETACH  equ 0

;--- the shared block, in DOS memory; offsets match src/JLMSHARE.H
SH_SIG      equ 00h         ; 'VSBJ'
SH_VER      equ 04h         ; block layout version
SH_RSIZE    equ 06h         ; DAC ring bytes, a power of two
SH_LPT      equ 08h         ; OPL3LPT data port; 0 = timer status only
SH_LPTDLY   equ 0Ah         ; control-port reads after each strobe
SH_FLAGS    equ 0Bh         ; F_* below
SH_SBBASE   equ 0Ch         ; emulated SB base
SH_QPICB    equ 10h         ; seg:off of VSBPCM's QPI stub
SH_WSCELL   equ 14h         ; linear address of the DSP write-status counter
SH_DSPIDLE  equ 18h         ; vsb.c: 1 = the next base+0Ch write is a command
SH_PEND10   equ 19h         ; 1 = a 10h was taken here, its sample comes next
SH_DHEAD    equ 1Ah         ; DAC ring: producer index (here)
SH_DTAIL    equ 1Ch         ;           consumer index (VSBPCM)
                            ; 1Eh: VSBPCM's measured DAC rate (not used here)
SH_DCOUNT   equ 20h         ; DAC samples received (wraps)
SH_DLOST    equ 24h         ; ... of which dropped, ring full
SH_FMW      equ 28h         ; FM writes served
SH_FMR      equ 2Ch         ; FM status reads served
SH_FMPATCH  equ 30h         ; IN AL,DX rewritten to NOP
SH_DACFWD   equ 34h         ; DSP writes handed to VSBPCM's stub
SH_DACWS    equ 38h         ; DSP write-status reads answered
SH_ARMED    equ 3Ch         ; written at arm: this module's version
SH_WSSITE   equ 40h         ; last write-status read: linear address of its
                            ; IN, then the 8 code bytes from there
SH_WRSITE   equ 4Ch         ; last direct-DAC write: the same for its OUT
                            ; 58h-77h: VSBPCM's drain statistics
SH_TICK     equ 78h         ; VSBPCM: counts its sound interrupts (the pump)
SH_GUARD    equ 7Ch         ; VSBPCM: 1 = keep its RTC pump alive (pulse)
SH_RTCPIE   equ 80h         ; pulse: RTC periodic interrupt found off
SH_RTCMASK  equ 82h         ; pulse: IRQ8 found masked at the slave PIC
SH_RTCHEAL  equ 84h         ; pulse: stuck RTC flags cleared (register C)
SH_DSPPATCH equ 88h         ; DSP write-status polls patched out
SH_PICCELL  equ 8Ch         ; linear address of the stub's PIC word: 0FFFFh
                            ; while VSBPCM virtualizes the SB IRQ
SH_PICIO    equ 90h         ; port 20h accesses done here
SH_DACPAIR  equ 94h         ; 10h-and-sample pairs made one trap
SH_V86PUTS  equ 98h         ; samples ringput (V86) has queued
SH_V86PATCH equ 9Ch         ; sequences turned into calls to ringput
SH_RING     equ 0A0h
SH_V86CODE  equ SH_RING + 800h  ; ringput's copy, after a 2048-byte ring
V86CODEMAX  equ 96
                            ; 900h-90Dh: VSBPCM's drain statistics
SH_OHEAD    equ 90Eh        ; FM ring: producer index (here)
SH_OTAIL    equ 910h        ;          consumer index (VSBPCM)
SH_OSIZE    equ 912h        ;          entries, a power of two; 0 = none
SH_OLOST    equ 914h        ;          FM writes dropped, ring full
SH_ORING    equ 91Ch        ;          dwords: value | index << 8 |
                            ;          register array << 16

SIG_VSBJ    equ 4A425356h   ; 'VSBJ'
BLK_VER     equ 6

F_FM        equ 01h
F_DAC       equ 02h
F_PATCH     equ 04h         ; FM delay reads -> NOP
F_POLL      equ 08h         ; DSP write-status polls -> no read at all
F_PIC       equ 10h         ; port 20h: the master PIC's command port
F_PAIR      equ 20h         ; 10h-and-sample sequences -> one trap
F_V86       equ 40h         ; ... and then a call to ringput, no trap
F_OPL       equ 80h         ; FM data writes into the FM ring, not the LPT

NPAIR       equ 4           ; direct-DAC sample sites remembered

MAXPORTS    equ 12

        .data

ifdef ?PE
        public export ddb
        option dotname
.drectve segment info
        db "-subsystem:native -dll -fixed:no"
.drectve ends
.hdr$2 segment flat
        db "PX"
.hdr$2 ends
else
        public ddb
endif

ddb VxD_Desc_Block <0,0,DEVICE_ID,VER_MAJOR,VER_MINOR,0,"VSBPCMJ",0,0, v86_api >

share   dd 0                ; linear address of the shared block; 0 = disarmed
rmask   dd 0                ; DAC ring size - 1
lptdat  dd 0                ; OPL3LPT data port; 0 = no chip
lptdly  dd 0                ; control-port reads after each strobe
omask   dd 0                ; FM ring entries - 1
flags   dd 0                ; F_* armed
sbbase  dd 0
qpicb   dd 0
wscell  dd 0
nports  dd 0
ports   dw MAXPORTS dup (0) ; ports trapped, for disarm
fmidx   db 0, 0             ; index latch, per register array
fmtmr   db 0, 0             ; last timer-control write, per timer
lastidx db 0                ; last index written, either array
lastsec db 0                ; RTC seconds at the last check of a still pump
secchg  db 0                ; ... and how often they changed since
lasttick dd 0               ; VSBPCM's interrupt count at the last access
still   dd 0                ; accesses since it last moved
span    dd 0                ; accesses per pump interrupt, smoothed
piccell dd 0
pairsin dd 0                ; pair sites in use
pairs   dd NPAIR dup (0)    ; linear addresses of their sample OUTs

banner  db "VSBPCMJ 0.5: ring-0 FM and DSP ports for VSBPCM, armed when it loads"
        db 13, 10, 0

;--- ringput: 16-bit code, copied to SH_V86CODE in the shared block at arm
;--- and far-called from a guest's sequence that v86patch rewrote. AH = the
;--- sample; DS = CS = the block. Preserves all registers and flags.
;---      pushf / cli / push ds / push bx / push cs / pop ds
;---      mov bx,[1Ah] / mov [bx+0A0h],ah / inc bx / and bx,7FFh
;---      cmp bx,[1Ch] / je full / mov [1Ah],bx / jmp count
;---  full: add word [24h],1 / adc word [26h],0       ; lost
;---  count: add word [20h],1 / adc word [22h],0      ; received
;---      add word [98h],1 / adc word [9Ah],0         ; queued from V86
;---      pop bx / pop ds / popf / retf
v86tmpl db 09Ch,0FAh,01Eh,053h,00Eh,01Fh,08Bh,01Eh,01Ah,000h,088h,0A7h
        db 0A0h,000h,043h,081h,0E3h,0FFh,007h,03Bh,01Eh,01Ch,000h,074h
        db 006h,089h,01Eh,01Ah,000h,0EBh,00Ah,083h,006h,024h,000h,001h
        db 083h,016h,026h,000h,000h,083h,006h,020h,000h,001h,083h,016h
        db 022h,000h,000h,083h,006h,098h,000h,001h,083h,016h,09Ah,000h
        db 000h,05Bh,01Fh,09Dh,0CBh
V86TMPLLEN equ $ - v86tmpl

        .code

;--- V86 API ---------------------------------------------------------------
v86_api proc
        @VMMCall Simulate_Far_Ret
        and [ebp].Client_Reg_Struc.Client_EFlags, not 1
        movzx eax, word ptr [ebp].Client_Reg_Struc.Client_EAX
        cmp eax, 0
        jz api_status
        cmp eax, 1
        jz api_arm
        cmp eax, 2
        jz api_disarm
api_fail:
        or [ebp].Client_Reg_Struc.Client_EFlags, 1
        ret
api_status:
        mov word ptr [ebp].Client_Reg_Struc.Client_EAX, VER_MAJOR * 256 + VER_MINOR
        mov eax, share
        mov [ebp].Client_Reg_Struc.Client_EDX, eax
        or eax, eax
        jz @F
        mov eax, flags
@@:
        mov word ptr [ebp].Client_Reg_Struc.Client_EBX, ax
        ret
api_arm:
        call arm
        jnc @F
        mov word ptr [ebp].Client_Reg_Struc.Client_EAX, ax
        mov word ptr [ebp].Client_Reg_Struc.Client_EDX, dx
        jmp api_fail
@@:
        ret
api_disarm:
        call disarm
        ret
v86_api endp

;--- arm: EBP = client (EDX = block). CF + AX = error, DX = port ------------
arm proc uses ebx esi edi
        cmp share, 0
        jnz arm_busy
        mov ebx, [ebp].Client_Reg_Struc.Client_EDX
        ; the block must sit in DOS memory or a UMB, both worlds' shared map
        cmp ebx, 600h
        jb arm_bad
        cmp ebx, 0F0000h - SH_RING
        jae arm_bad
        cmp dword ptr [ebx+SH_SIG], SIG_VSBJ
        jnz arm_bad
        cmp word ptr [ebx+SH_VER], BLK_VER
        jnz arm_bad
        movzx eax, word ptr [ebx+SH_RSIZE]
        cmp eax, 16
        jb arm_bad
        cmp eax, 8000h
        ja arm_bad
        lea ecx, [eax-1]
        test eax, ecx
        jnz arm_bad
        mov rmask, ecx
        lea ecx, [ebx+SH_RING+eax+V86CODEMAX]
        cmp ecx, 0F0000h
        ja arm_bad
        ; LPT: one of the three standard bases, or none. Nothing here may
        ; ever reach COM1 (3F8h, the COMrade link) or the PCIC (3E0h).
        movzx eax, word ptr [ebx+SH_LPT]
        or eax, eax
        jz arm_lptok
        cmp eax, 378h
        jz arm_lptok
        cmp eax, 278h
        jz arm_lptok
        cmp eax, 3BCh
        jnz arm_bad
arm_lptok:
        mov lptdat, eax
        movzx eax, byte ptr [ebx+SH_LPTDLY]
        mov lptdly, eax
        movzx eax, word ptr [ebx+SH_SBBASE]
        cmp eax, 220h
        jz @F
        cmp eax, 240h
        jnz arm_bad
@@:
        mov sbbase, eax
        movzx eax, byte ptr [ebx+SH_FLAGS]
        and eax, F_FM or F_DAC or F_PATCH or F_POLL or F_PIC or F_PAIR or F_V86 or F_OPL
        mov flags, eax
        test eax, F_OPL                 ; the FM ring: FM ports, no LPT, and
        jz arm_noopl                    ; a ring that ends below 0F0000h
        test eax, F_FM
        jz arm_bad
        mov lptdat, 0
        movzx eax, word ptr [ebx+SH_OSIZE]
        cmp eax, 16
        jb arm_bad
        cmp eax, 4096
        ja arm_bad
        lea ecx, [eax-1]
        test eax, ecx
        jnz arm_bad
        mov omask, ecx
        lea ecx, [ebx+SH_ORING+eax*4]
        cmp ecx, 0F0000h
        ja arm_bad
arm_noopl:
        mov eax, [ebx+SH_QPICB]
        mov qpicb, eax
        mov eax, [ebx+SH_WSCELL]
        mov wscell, eax
        mov eax, [ebx+SH_PICCELL]
        mov piccell, eax
        test byte ptr flags, F_PIC
        jz @F
        cmp eax, 400h                   ; the stub's PIC word, in DOS memory
        jb arm_bad
        cmp eax, 0F0000h
        jae arm_bad
@@:
        test byte ptr flags, F_DAC
        jz arm_nodac
        cmp qpicb, 0
        jz arm_bad
        cmp wscell, 400h
        jb arm_bad
        cmp wscell, 0F0000h
        jae arm_bad
arm_nodac:
        mov word ptr fmidx, 0
        mov word ptr fmtmr, 0
        mov lastidx, 0
        mov secchg, 0
        mov still, 0
        mov span, 0
        mov pairsin, 0
        mov eax, [ebx+SH_TICK]
        mov lasttick, eax
        test byte ptr flags, F_V86      ; ringput assumes a 2048-byte ring
        jz arm_nov86
        cmp rmask, 7FFh
        jz @F
        and flags, not F_V86
        jmp arm_nov86
@@:
        mov esi, offset v86tmpl         ; DS only: ES and DF are not ours
        lea edi, [ebx+SH_V86CODE]       ; to assume inside a trap
        mov ecx, V86TMPLLEN
arm_copy:
        mov al, [esi]
        mov [edi], al
        inc esi
        inc edi
        loop arm_copy
arm_nov86:
        mov nports, 0
        test byte ptr flags, F_FM
        jz arm_dac
        mov esi, offset fm_io
        mov edx, 388h
arm_fm:
        call trap1
        jc arm_undo
        inc edx
        cmp edx, 38Ch
        jb arm_fm
        mov edx, sbbase
arm_alias:
        call trap1                      ; base+0..3
        jc arm_undo
        inc edx
        mov eax, edx
        sub eax, sbbase
        cmp eax, 4
        jb arm_alias
        add edx, 4
        call trap1                      ; base+8
        jc arm_undo
        inc edx
        call trap1                      ; base+9
        jc arm_undo
arm_dac:
        test byte ptr flags, F_DAC
        jz arm_pic
        mov esi, offset dsp_io
        mov edx, sbbase
        add edx, 0Ch
        call trap1
        jc arm_undo
arm_pic:
        test byte ptr flags, F_PIC
        jz arm_done
        mov esi, offset pic_io
        mov edx, 20h
        call trap1
        jc arm_undo
arm_done:
        mov dword ptr [ebx+SH_ARMED], VER_MAJOR * 256 + VER_MINOR
        mov share, ebx
        clc
        ret
arm_undo:
        push edx
        call untrap_all
        pop edx
        mov ax, 3
        stc
        ret
arm_busy:
        mov ax, 1
        stc
        ret
arm_bad:
        mov ax, 2
        stc
        ret
arm endp

;--- trap port EDX with handler ESI and record it. CF = refused. -----------
trap1 proc
        push edx
        @VMMCall Install_IO_Handler
        pop edx
        jc @F
        mov ecx, nports
        mov ports[ecx*2], dx
        inc nports
        clc
@@:
        ret
trap1 endp

untrap_all proc uses edi
        mov edi, nports
@@:
        or edi, edi
        jz @F
        dec edi
        movzx edx, ports[edi*2]
        @VMMCall Remove_IO_Handler
        jmp @B
@@:
        mov nports, 0
        ret
untrap_all endp

disarm proc
        call untrap_all
        mov share, 0
        ret
disarm endp

;--- Keep VSBPCM's pump alive. On the IBM card its sound interrupt is the
;--- RTC's periodic interrupt, and something in a game's stack can switch it
;--- off: the BIOS clears PIE when an INT 15h wait ends, and IRQ8 has been
;--- found masked at the slave PIC. VSBPCM's own watchdog rode a protected-
;--- mode IRQ0 hook, two mode switches per PIT tick -- thousands a second
;--- under a direct-DAC game. This one rides the guest's own accesses at
;--- ring 0: FM, DSP, and with F_PIC every V86 EOI, which the BIOS timer
;--- alone sends 18.2 times a second. While VSBPCM's interrupt count moves,
;--- an access costs one compare. When it stands still for twice the usual
;--- number of accesses per interrupt (span, measured as it runs), a check:
;--- PIE back on, IRQ8 unmasked, and once the RTC's seconds have changed
;--- twice with the count still, register C read to clear a stuck flag
;--- (ib_watchdog's rules, in sc_ibmaud.c; the seconds are read only while
;--- no update is in progress). Only while VSBPCM asks for it (SH_GUARD).
;--- Preserves all.
pulse proc
        push ebx
        mov ebx, share
        or ebx, ebx
        jz pulse_out
        cmp byte ptr [ebx+SH_GUARD], 0
        jz pulse_out
        push eax
        mov eax, [ebx+SH_TICK]
        cmp eax, lasttick
        jz pulse_still
        mov lasttick, eax               ; the pump runs: learn the span
        mov eax, still
        add eax, span
        shr eax, 1
        mov span, eax
        mov still, 0
        mov secchg, 0
        pop eax
        jmp pulse_out
pulse_still:
        inc still
        mov eax, span
        lea eax, [eax*2+2]
        cmp still, eax
        jb pulse_ret
        mov still, 0
        mov al, 8Bh                     ; register B (bit 7: NMI off meanwhile)
        out 70h, al
        in al, 71h
        test al, 40h                    ; PIE
        jnz pulse_pie
        or al, 40h
        mov ah, al
        mov al, 8Bh
        out 70h, al
        mov al, ah
        out 71h, al
        inc word ptr [ebx+SH_RTCPIE]
pulse_pie:
        in al, 0A1h                     ; slave PIC mask: IRQ8 is bit 0
        test al, 1
        jz pulse_mask
        and al, 0FEh
        out 0A1h, al
        inc word ptr [ebx+SH_RTCMASK]
pulse_mask:
        mov al, 8Ah                     ; register A: bit 7 = update in progress
        out 70h, al
        in al, 71h
        test al, 80h
        jnz pulse_done
        mov al, 80h                     ; seconds
        out 70h, al
        in al, 71h
        cmp al, lastsec
        jz pulse_done
        mov lastsec, al
        inc secchg
        cmp secchg, 2                   ; still through two second changes
        jb pulse_done
        mov secchg, 0
        mov al, 8Ch                     ; register C: reading it clears the
        out 70h, al                     ; flags of a lost interrupt
        in al, 71h
        inc word ptr [ebx+SH_RTCHEAL]
pulse_done:
        mov al, 0Dh                     ; index back on register D, NMI on
        out 70h, al
pulse_ret:
        pop eax
pulse_out:
        pop ebx
        ret
pulse endp

;--- LPT: AL onto the data lines, then BH, BL, BH to the control port, then
;--- lptdly control-port reads (the OPL3 needs 3.3 us after each write).
;--- Interrupts are off: a JLM runs with them disabled. Preserves all.
lptout proc
        push eax
        push ecx
        push edx
        mov edx, lptdat
        out dx, al
        add edx, 2
        mov al, bh
        out dx, al
        mov al, bl
        out dx, al
        mov al, bh
        out dx, al
        mov ecx, lptdly
        jecxz lpt_done
lpt_wait:
        in al, dx
        loop lpt_wait
lpt_done:
        pop edx
        pop ecx
        pop eax
        ret
lptout endp

;--- FM register write: DX = port (388h-38Bh or an SB alias), AL = value.
;--- Port bit 0 = data, bit 1 = second register array; the SB base is
;--- 16-aligned, so its aliases carry the same bits. Preserves all.
fm_wr proc
        push ebx
        push ecx
        mov ebx, share
        inc dword ptr [ebx+SH_FMW]
        mov ecx, edx
        shr ecx, 1
        and ecx, 1                      ; register array
        test dl, 1
        jnz fmw_data
        mov fmidx[ecx], al              ; index write
        mov lastidx, al
        cmp lptdat, 0
        jz fmw_done
        mov bx, 0D09h                   ; first array: control 13/9/13
        jecxz @F
        mov bx, 0501h                   ; second array: control 5/1/5
@@:
        call lptout
        jmp fmw_done
fmw_data:
        jecxz fmw_first
        jmp fmw_send
fmw_first:
        cmp fmidx[0], 4                 ; timer control: both halves are
        jnz fmw_send                    ; latched from the same byte, as in
        test al, 0C1h                   ; FMSHIM_Acc
        jz @F
        mov fmtmr[0], al
@@:
        test al, 0A2h
        jz fmw_send
        mov fmtmr[1], al
fmw_send:
        test byte ptr flags, F_OPL
        jnz fmw_ring
        cmp lptdat, 0
        jz fmw_done
        mov bx, 0C08h                   ; value: control 12/8/12
        call lptout
        jmp fmw_done
fmw_ring:                               ; the software OPL3's entry, in order
        push eax
        push edx
        movzx eax, al
        mov ah, fmidx[ecx]
        shl ecx, 16
        or eax, ecx                     ; value | index << 8 | array << 16
        movzx ecx, word ptr [ebx+SH_OHEAD]
        lea edx, [ecx+1]
        and edx, omask
        cmp dx, word ptr [ebx+SH_OTAIL]
        jz fmw_full
        mov [ebx+SH_ORING+ecx*4], eax
        mov word ptr [ebx+SH_OHEAD], dx ; then the index: the entry is whole
        jmp fmw_put
fmw_full:
        inc dword ptr [ebx+SH_OLOST]
fmw_put:
        pop edx
        pop eax
fmw_done:
        pop ecx
        pop ebx
        ret
fm_wr endp

;--- FM status read: AL = the timer model's status (timers read back as
;--- expired the moment they are started unmasked). Preserves all but AL.
fm_rd proc
        push ebx
        push ecx
        mov ebx, share
        inc dword ptr [ebx+SH_FMR]
        xor cl, cl
        mov ch, fmtmr[0]
        and ch, 0C1h
        cmp ch, 01h
        jnz @F
        or cl, 0C0h
@@:
        mov ch, fmtmr[1]
        and ch, 0A2h
        cmp ch, 02h
        jnz @F
        or cl, 0A0h
@@:
        mov al, cl
        pop ecx
        pop ebx
        ret
fm_rd endp

;--- is DX one of our FM ports? CF clear if so. Preserves all.
is_fm proc
        push eax
        movzx eax, dx
        sub eax, 388h
        cmp eax, 3
        jbe isfm_yes
        movzx eax, dx
        sub eax, sbbase
        cmp eax, 3
        jbe isfm_yes
        cmp eax, 8
        jz isfm_yes
        cmp eax, 9
        jz isfm_yes
        pop eax
        stc
        ret
isfm_yes:
        pop eax
        clc
        ret
is_fm endp

;--- one byte of a multi-byte or string access at port DX: ours goes
;--- through the FM model, anything else to the hardware. CL bit 2 = OUT,
;--- AL = value (OUT) or result (IN).
fm_byte proc
        call is_fm
        jc fmb_hw
        test cl, OUTPUT
        jnz fm_wr
        jmp fm_rd
fmb_hw:
        test cl, OUTPUT
        jz @F
        out dx, al
        ret
@@:
        in al, dx
        ret
fm_byte endp

;--- the I/O handler for the FM ports ---------------------------------------
;--- in:  ECX = access type, EDX = port, EBP = client, EAX = client EAX
;--- out: EAX, stored into the client EAX for a non-string IN
fm_io proc
        call pulse
        test cl, STRING_IO
        jnz fm_string
        test cl, WORD_IO or DWORD_IO
        jnz fm_wide
        test cl, OUTPUT
        jnz fm_wr
        call fm_rd
        ; SBEFMPATCH: a status read while the last index is 20h or above is
        ; delay padding (AdLib detection reads status on registers 1-4).
        ; EIP is already past the instruction, so its EC is at IP-1. Only
        ; conventional memory is patched: code lives there, and a write to
        ; a read-only page at ring 0 would fault.
        test byte ptr flags, F_PATCH
        jz fmio_ret
        cmp lastidx, 20h
        jb fmio_ret
        push ebx
        push ecx
        movzx ebx, word ptr [ebp].Client_Reg_Struc.Client_CS
        shl ebx, 4
        movzx ecx, word ptr [ebp].Client_Reg_Struc.Client_EIP
        dec cx
        add ebx, ecx
        cmp ebx, 0A0000h
        jae fmio_np
        cmp byte ptr [ebx], 0ECh        ; IN AL,DX
        jnz fmio_np
        mov byte ptr [ebx], 90h         ; NOP
        mov ebx, share
        inc dword ptr [ebx+SH_FMPATCH]
fmio_np:
        pop ecx
        pop ebx
fmio_ret:
        ret

;--- word / dword: byte by byte, low byte first (Jemm's own order)
fm_wide:
        push ebx
        push esi
        push edi
        mov esi, 2
        test cl, DWORD_IO
        jz @F
        mov esi, 4
@@:
        mov ebx, eax                    ; OUT: the value; IN: the result
        xor edi, edi
fmw_next:
        push edx
        push ecx
        add edx, edi
        mov ecx, edi
        shl ecx, 3
        mov eax, ebx
        shr eax, cl
        mov ecx, [esp]                  ; the access type again
        call fm_byte
        test cl, OUTPUT
        jnz fmw_step
        mov ecx, edi                    ; IN: put AL into byte EDI of EBX
        shl ecx, 3
        ror ebx, cl
        mov bl, al
        rol ebx, cl
fmw_step:
        pop ecx
        pop edx
        inc edi
        cmp edi, esi
        jb fmw_next
        mov eax, ebx
        pop edi
        pop esi
        pop ebx
        ret

;--- string I/O (INS/OUTS, rare on these ports): element by element through
;--- fm_byte, then SI or DI and, with REP, CX back to the guest. The high
;--- word of ECX holds the segment.
fm_string:
        push ebx
        push esi
        push edi
        push eax
        mov ebx, ecx
        shr ebx, 16
        shl ebx, 4                      ; segment base
        mov esi, 1
        test cl, WORD_IO
        jz @F
        mov esi, 2
@@:
        test cl, DWORD_IO
        jz @F
        mov esi, 4
@@:
        mov edi, 1                      ; elements
        test cl, REP_IO
        jz @F
        movzx edi, word ptr [ebp].Client_Reg_Struc.Client_ECX
@@:
fms_elem:
        or edi, edi
        jz fms_done
        push edi
        push edx
        xor edi, edi                    ; byte within the element
fms_byte:
        test cl, OUTPUT
        jz fms_in
        movzx eax, word ptr [ebp].Client_Reg_Struc.Client_ESI
        add ax, di
        mov al, [ebx+eax]
        call fm_byte
        jmp fms_next
fms_in:
        call fm_byte
        push edx
        movzx edx, word ptr [ebp].Client_Reg_Struc.Client_EDI
        add dx, di
        mov [ebx+edx], al
        pop edx
fms_next:
        inc edx
        inc edi
        cmp edi, esi
        jb fms_byte
        pop edx
        pop edi
        ; advance SI or DI by one element, backwards when DF is set
        mov eax, esi
        test byte ptr [ebp].Client_Reg_Struc.Client_EFlags+1, 4
        jz @F
        neg eax
@@:
        test cl, OUTPUT
        jz @F
        add word ptr [ebp].Client_Reg_Struc.Client_ESI, ax
        jmp fms_cnt
@@:
        add word ptr [ebp].Client_Reg_Struc.Client_EDI, ax
fms_cnt:
        dec edi
        test cl, REP_IO
        jz fms_elem
        mov word ptr [ebp].Client_Reg_Struc.Client_ECX, di
        jmp fms_elem
fms_done:
        pop eax
        pop edi
        pop esi
        pop ebx
        ret
fm_io endp

;--- hand an access to VSBPCM's real-mode stub, as QPIEMU's iocb does: DX =
;--- port, CL = type, CH bit 1 = the guest's IF, client AL = the value; the
;--- stub runs nested with IF and TF clear. Returns the client EAX.
forward proc
        push ecx
        push edx
        push [ebp].Client_Reg_Struc.Client_EFlags
        push [ebp].Client_Reg_Struc.Client_ECX
        push [ebp].Client_Reg_Struc.Client_EDX
        mov ch, byte ptr [ebp].Client_Reg_Struc.Client_EFlags+1
        and ch, 2
        and byte ptr [ebp].Client_Reg_Struc.Client_EFlags+1, not (1+2)
        mov word ptr [ebp].Client_Reg_Struc.Client_EDX, dx
        mov word ptr [ebp].Client_Reg_Struc.Client_ECX, cx
        @VMMCall Begin_Nest_Exec
        movzx edx, word ptr qpicb+0
        movzx ecx, word ptr qpicb+2
        @VMMCall Simulate_Far_Call
        @VMMCall Resume_Exec
        @VMMCall End_Nest_Exec
        pop [ebp].Client_Reg_Struc.Client_EDX
        pop [ebp].Client_Reg_Struc.Client_ECX
        pop [ebp].Client_Reg_Struc.Client_EFlags
        mov eax, [ebp].Client_Reg_Struc.Client_EAX
        pop edx
        pop ecx
        ret
forward endp

;--- record where the guest is: the linear address of the instruction that
;--- trapped (EIP is past it; IN AL,DX and OUT DX,AL are one byte), then the
;--- 8 code bytes from there, at EBX+EDI. A bench black box: it shows how a
;--- guest drives the DSP. Preserves all.
site proc
        push eax
        push esi
        movzx esi, word ptr [ebp].Client_Reg_Struc.Client_CS
        shl esi, 4
        movzx eax, word ptr [ebp].Client_Reg_Struc.Client_EIP
        dec ax
        add esi, eax
        mov [ebx+edi], esi
        mov eax, [esi]
        mov [ebx+edi+4], eax
        mov eax, [esi+4]
        mov [ebx+edi+8], eax
        pop esi
        pop eax
        ret
site endp

;--- A write-status poll that only waits for "ready" is patched out of the
;--- guest, like the FM delay reads: this DSP is never busy, and each poll is
;--- a trap -- two of the four per sample in a direct-DAC loop. Only these
;--- exact busy-wait loops, the IN being the instruction that trapped:
;---    IN AL,DX / OR|TEST AL,AL / JS back to the IN
;---    IN AL,DX / TEST AL,80h  / JNZ back to the IN
;---    the same tests, then JNS|JZ over a LOOP back to the IN
;--- The IN becomes NOP and the test XOR AL,AL, which says ready (SF and
;--- ZF as for 00h) without a read; the branch stays. EIP is already past
;--- the IN, so execution resumes on the XOR. Conventional memory only.
;--- EBX = the shared block (SH_WSSITE holds the IN's linear address).
pollpatch proc
        push eax
        push esi
        mov esi, [ebx+SH_WSSITE]
        cmp esi, 0A0000h - 8
        jae pp_no
        cmp byte ptr [esi], 0ECh        ; IN AL,DX
        jnz pp_no
        mov ax, [esi+1]
        cmp ax, 0C00Ah                  ; OR AL,AL
        jz pp_sign
        cmp ax, 0C008h                  ; OR AL,AL (other encoding)
        jz pp_sign
        cmp ax, 0C084h                  ; TEST AL,AL
        jz pp_sign
        cmp ax, 080A8h                  ; TEST AL,80h
        jz pp_zero
        jmp pp_no
pp_sign:
        cmp word ptr [esi+3], 0FB78h    ; JS $-3: back to the IN
        jz pp_do
        cmp dword ptr [esi+3], 0F9E20279h   ; JNS +2, LOOP back to the IN
        jz pp_do
        jmp pp_no
pp_zero:
        cmp word ptr [esi+3], 0FB75h    ; JNZ back to the IN
        jz pp_do
        cmp dword ptr [esi+3], 0F9E20274h   ; JZ +2, LOOP back to the IN
        jnz pp_no
pp_do:
        mov byte ptr [esi], 90h         ; NOP
        mov word ptr [esi+1], 0C032h    ; XOR AL,AL
        inc dword ptr [ebx+SH_DSPPATCH]
pp_no:
        pop esi
        pop eax
        ret
pollpatch endp

;--- A direct-DAC loop sends 10h and its sample with the same two OUTs every
;--- time. Another World's, once its polls are patched (pollpatch):
;---      MOV AL,10h / OUT DX,AL / NOP / XOR AL,AL / JS $-3 / MOV AL,r8 /
;---      OUT DX,AL
;--- On the trap of that 10h OUT, with exactly this code behind it, the OUT
;--- becomes NOP and the second OUT's address is remembered: from then on a
;--- write from there is a 10h-and-sample pair, one trap instead of two.
;--- At most NPAIR sites, in conventional memory. EBX = the shared block.
pairpatch proc
        push eax
        push esi
        cmp pairsin, NPAIR
        jae pair_no
        movzx esi, word ptr [ebp].Client_Reg_Struc.Client_CS
        shl esi, 4
        movzx eax, word ptr [ebp].Client_Reg_Struc.Client_EIP
        dec ax
        add esi, eax                    ; the 10h OUT
        cmp esi, 600h
        jb pair_no
        cmp esi, 0A0000h - 16
        jae pair_no
        cmp word ptr [esi-2], 10B0h     ; MOV AL,10h
        jnz pair_no
        cmp byte ptr [esi], 0EEh        ; OUT DX,AL
        jnz pair_no
        cmp dword ptr [esi+1], 78C03290h    ; NOP / XOR AL,AL / JS
        jnz pair_no
        cmp byte ptr [esi+5], 0FBh      ; ... back to the NOP
        jnz pair_no
        cmp byte ptr [esi+6], 8Ah       ; MOV AL,r8
        jnz pair_no
        mov al, [esi+7]
        and al, 0F8h
        cmp al, 0C0h
        jnz pair_no
        cmp byte ptr [esi+8], 0EEh      ; OUT DX,AL: the sample
        jnz pair_no
        mov byte ptr [esi], 90h         ; the 10h OUT goes
        mov eax, pairsin
        lea esi, [esi+8]
        mov pairs[eax*4], esi
        inc pairsin
        inc dword ptr [ebx+SH_DACPAIR]
pair_no:
        pop esi
        pop eax
        ret
pairpatch endp

;--- Once pollpatch and pairpatch are through, Another World runs this for
;--- every sample: MOV AL,10h / NOP / NOP / XOR AL,AL / JS $-3 / MOV AL,AH /
;--- OUT DX,AL. All of it but the OUT is dead, and the OUT is the last trap.
;--- These 11 bytes become a far call to ringput in the shared block and 6
;--- NOPs, so the sample goes into the ring from V86 without a trap. Only
;--- this exact sequence, ending at the OUT that trapped; EIP is past that
;--- OUT, so execution goes on after it as before. EBX = the shared block.
v86patch proc
        push eax
        push esi
        movzx esi, word ptr [ebp].Client_Reg_Struc.Client_CS
        shl esi, 4
        movzx eax, word ptr [ebp].Client_Reg_Struc.Client_EIP
        dec ax
        add esi, eax                    ; the sample OUT
        sub esi, 10                     ; the MOV AL,10h before it
        cmp esi, 600h
        jb v86_no
        cmp esi, 0A0000h - 16
        jae v86_no
        cmp dword ptr [esi], 909010B0h      ; MOV AL,10h / NOP / NOP
        jnz v86_no
        cmp dword ptr [esi+4], 0FB78C032h   ; XOR AL,AL / JS $-3
        jnz v86_no
        cmp word ptr [esi+8], 0C48Ah        ; MOV AL,AH
        jnz v86_no
        cmp byte ptr [esi+10], 0EEh         ; OUT DX,AL
        jnz v86_no
        mov byte ptr [esi], 9Ah             ; CALL FAR block:SH_V86CODE
        mov word ptr [esi+1], SH_V86CODE
        mov eax, ebx
        shr eax, 4
        mov word ptr [esi+3], ax
        mov dword ptr [esi+5], 90909090h    ; NOP x 6
        mov word ptr [esi+9], 9090h
        inc dword ptr [ebx+SH_V86PATCH]
v86_no:
        pop esi
        pop eax
        ret
v86patch endp

;--- CF clear if the OUT that trapped is a remembered sample site. Preserves
;--- all but flags.
ispair proc
        push eax
        push ecx
        push esi
        movzx esi, word ptr [ebp].Client_Reg_Struc.Client_CS
        shl esi, 4
        movzx eax, word ptr [ebp].Client_Reg_Struc.Client_EIP
        dec ax
        add esi, eax
        mov ecx, pairsin
ispair_next:
        jecxz ispair_no
        dec ecx
        cmp pairs[ecx*4], esi
        jnz ispair_next
        pop esi
        pop ecx
        pop eax
        clc
        ret
ispair_no:
        pop esi
        pop ecx
        pop eax
        stc
        ret
ispair endp

;--- the I/O handler for port 20h, the master PIC's command port. VSBPCM
;--- traps it for its virtual SB interrupt; while none is in progress
;--- (its stub's PIC word is not 0FFFFh) an EOI or a read goes straight to
;--- the PIC from here, and the pump check rides it. Otherwise, and for
;--- anything wider than a byte, the access is VSBPCM's.
pic_io proc
        call pulse
        test cl, STRING_IO or WORD_IO or DWORD_IO
        jnz forward
        push ebx
        mov ebx, piccell
        cmp word ptr [ebx], 0FFFFh
        jz pic_fwd
        mov ebx, share
        inc dword ptr [ebx+SH_PICIO]
        pop ebx
        test cl, OUTPUT
        jz @F
        out dx, al
        ret
@@:
        in al, dx
        ret
pic_fwd:
        pop ebx
        jmp forward
pic_io endp

;--- the I/O handler for the DSP write port (base+0Ch) ---------------------
dsp_io proc
        call pulse
        test cl, STRING_IO or WORD_IO or DWORD_IO
        jnz forward
        push ebx
        push edi
        mov ebx, share
        test cl, OUTPUT
        jnz dsp_wr
        ; write status: vsb.c's 2-bit counter, ready (7Fh) three reads in
        ; four, busy (0FFh) on the fourth -- the stub's isws, verbatim
        inc dword ptr [ebx+SH_DACWS]
        mov edi, SH_WSSITE
        call site
        test byte ptr flags, F_POLL
        jz @F
        call pollpatch
@@:
        mov ebx, wscell
        inc byte ptr [ebx]
        mov al, 7Fh
        test byte ptr [ebx], 3
        jnz @F
        mov al, 0FFh
@@:
        pop edi
        pop ebx
        ret
dsp_wr:
        cmp byte ptr [ebx+SH_PEND10], 0
        jnz dsp_sample
        cmp pairsin, 0                    ; a remembered sample OUT: its 10h
        jz dsp_cmd                      ; was patched out (pairpatch)
        call ispair
        jc dsp_cmd
        test byte ptr flags, F_V86
        jz dsp_sample
        call v86patch
        jmp dsp_sample
dsp_cmd:
        cmp al, 10h                     ; direct DAC, if it is a command:
        jnz dsp_fwd                     ; only vsb.c knows, and it says so
        cmp byte ptr [ebx+SH_DSPIDLE], 0    ; in SH_DSPIDLE
        jz dsp_fwd
        mov byte ptr [ebx+SH_PEND10], 1
        test byte ptr flags, F_PAIR
        jz dsp_ready
        call pairpatch
        jmp dsp_ready
dsp_sample:
        mov byte ptr [ebx+SH_PEND10], 0
        push ecx
        push edx
        inc dword ptr [ebx+SH_DCOUNT]
        movzx ecx, word ptr [ebx+SH_DHEAD]
        lea edx, [ecx+1]
        and edx, rmask
        cmp dx, word ptr [ebx+SH_DTAIL]
        jz dsp_full
        mov [ebx+SH_RING+ecx], al
        mov word ptr [ebx+SH_DHEAD], dx
        jmp dsp_put
dsp_full:
        inc dword ptr [ebx+SH_DLOST]
dsp_put:
        pop edx
        pop ecx
dsp_ready:
        mov edi, SH_WRSITE
        call site
        ; vsb.c's CMD10NOWAIT: 10h and its sample leave the DSP ready
        mov ebx, wscell
        mov byte ptr [ebx], 0
        pop edi
        pop ebx
        ret
dsp_fwd:
        inc dword ptr [ebx+SH_DACFWD]
        pop edi
        pop ebx
        jmp forward
dsp_io endp

;--- one line to the console through DOS, char by char -------------------
say proc uses ebx esi ebp
        @VMMCall Get_Cur_VM_Handle
        mov ebp, [ebx].cb_s.CB_Client_Pointer
        push [ebp].Client_Reg_Struc.Client_EAX
        push [ebp].Client_Reg_Struc.Client_EDX
        @VMMCall Begin_Nest_Exec
        mov esi, offset banner
say_next:
        movzx eax, byte ptr [esi]
        or eax, eax
        jz say_done
        inc esi
        mov [ebp].Client_Reg_Struc.Client_EDX, eax
        mov [ebp].Client_Reg_Struc.Client_EAX, 0200h
        mov eax, 21h
        @VMMCall Exec_Int
        jmp say_next
say_done:
        @VMMCall End_Nest_Exec
        pop [ebp].Client_Reg_Struc.Client_EDX
        pop [ebp].Client_Reg_Struc.Client_EAX
        ret
say endp

;--- JLM load / unload ------------------------------------------------------
DllMain proc stdcall public hModule:dword, dwReason:dword, dwRes:dword
        mov eax, dwReason
        cmp eax, DLL_PROCESS_ATTACH
        jnz @F
        call say
        mov eax, 1
        ret
@@:
        cmp eax, DLL_PROCESS_DETACH
        jnz dm_ok
        xor eax, eax
        cmp share, eax                  ; armed: VSBPCM still relies on us
        jnz dm_ret
dm_ok:
        mov eax, 1
dm_ret:
        ret
DllMain endp

        end DllMain
