;--- PMISR: chained / iret protected-mode interrupt trampolines.
;
;--- Replaces _go32_dpmi_chain_protected_mode_interrupt_vector, which is a
;--- DJGPP libc facility with no Open Watcom equivalent and therefore the one
;--- real blocker to a 16-bit (NOTFLAT) build.  The card backends install the
;--- IRQ0 watchdog heartbeat through it: the PIT always ticks, so a guest that
;--- kills the RTC (Theme Hospital does, for 6.5s at a stretch) is healed
;--- within ~110ms by a handler that does not depend on the guest at all.
;
;--- STRUCTURE IS DELIBERATELY stackisr.asm's SwitchStackISR, not something
;--- new: private ISR stack through __djgpp_stack_top, our DS fetched from a
;--- cs:-relative word, insIRET or a far jmp to the saved PFAR.  That routine
;--- is the proven dual-build PM interrupt handler in this tree; every
;--- NOTFLAT-vs-flat difference here has a precedent there or in sbisr.asm.
;
;--- WHY A FIXED POOL.  In protected mode CS is not writable, so a trampoline
;--- cannot patch its own state, and one shared trampoline cannot tell which
;--- vector it was entered for.  Each slot is therefore ASSEMBLED with its
;--- index as a literal - no self-modifying code, no runtime dispatch.  Four
;--- is one more than the backends can currently ask for (each installs at
;--- most an IRQ0 heartbeat plus an optional IRQ5 TC handler, and only one
;--- backend is ever live).
;
;--- WHERE THE STATE LIVES.  Under NOTFLAT the trampolines reach their slot
;--- data with a cs: prefix, so the table must sit in the CODE segment and be
;--- written through _csalias - exactly what sbisr.asm does with oldSBvec and
;--- stackisr.asm with currSndvec.  Under flat, cs: and ds: share a base, so
;--- the table goes in .data.  READS therefore always carry a cs: prefix
;--- (valid in both builds); WRITES go through the alias under NOTFLAT.
;
;--- CHAINING.  On the chain path the trampoline must leave the interrupt
;--- frame exactly as it arrived and jump to the previous handler, which then
;--- owns the EOI and the iret.  FLAGS are dead at that point (the frame's
;--- copy is what iret restores), so testing the chain byte after popad is
;--- safe.

	.386
	.MODEL ?MODEL, c
	option casemap:none
	option proc:private

	include config.inc
	include debug.inc

PMISR_SLOTS equ 4

externdef c __djgpp_stack_top:dword
ifdef STKDIAG
externdef c DSBase:dword
endif

ifdef NOTFLAT
STACKCORR equ 2048
else
STACKCORR equ 4096
endif

;--- slot state.  Parallel arrays rather than a struct: PFAR is 4 bytes under
;--- NOTFLAT and 6 under flat, so a struct's stride would differ per build and
;--- every index expression would need SIZEOF gymnastics for no gain.

ifdef NOTFLAT

	.code

pm_pfn   dd PMISR_SLOTS dup (?)		; the C handler for each slot
pm_old   PFAR PMISR_SLOTS dup (?)	; the PM vector that was there before
pm_chain db PMISR_SLOTS dup (?)		; 1 = fall through to it, 0 = iret
pm_used  db PMISR_SLOTS dup (?)
pm_vec   db PMISR_SLOTS dup (?)
pmDS     dd ?				; our DS, readable through cs:

	.data

else

	assume ds:_DATA

	.data

pm_pfn   dd PMISR_SLOTS dup (?)
	align 4
pm_old   PFAR PMISR_SLOTS dup (?)
	align 4
pm_chain db PMISR_SLOTS dup (?)
pm_used  db PMISR_SLOTS dup (?)
pm_vec   db PMISR_SLOTS dup (?)
	align 4
pmDS     dd ?

endif

PFARSIZE equ SIZEOF PFAR

	.code

;--- one trampoline per slot; n is a literal at assembly time.

MAKETRAMP macro n
	align 4
pmisr_&n&:
	pushad
	push ds
	push es
	cld
	mov ebx, cs:[pmDS]
	mov ds, ebx
	mov es, ebx
;--- switch to the private ISR stack.  The sub/add pair is what makes this
;--- nesting-safe, and sharing it with the sound ISR is deliberate: a
;--- watchdog that fires while SNDISR is running must not land on its frame.
	mov edx, ss
	mov ecx, esp
	sub [__djgpp_stack_top], STACKCORR
ifdef STKDIAG
;--- live nesting depth at 0x4FC, high-water at 0x4FD. These trampolines
;--- share __djgpp_stack_top with stackisr/stackio but have no STACKCHECK,
;--- so a storm HERE surfaces as stackisr fatal error 3 with an innocent-
;--- looking SNDISR depth; this pair points the finger correctly.
;--- KNOWN COLLISION: on a TSC-capable CPU the engine's duration probe
;--- (sndisr.c dbg_exit) also writes 0x4FC/0x4FD, so these bytes are only
;--- trustworthy on a no-TSC box or in the first instants after load.
;--- The 0x4F0 map is full; live with it (STKDIAG is a bench build).
	mov eax, 4FCh
	sub eax, [DSBase]
	inc byte ptr [eax]
	mov bl, [eax]
	cmp bl, [eax+1]
	jbe @F
	mov [eax+1], bl
@@:
	mov ebx, cs:[pmDS]	; reload: bl was clobbered
endif
	mov ss, ebx
	mov esp, [__djgpp_stack_top]
	push edx
	push ecx
	call dword ptr cs:[pm_pfn + n*4]
	lss esp, [esp]
	add [__djgpp_stack_top], STACKCORR
ifdef STKDIAG
	mov eax, 4FCh
	sub eax, [DSBase]
	dec byte ptr [eax]	; DS still ours; balanced on BOTH exits below
endif
	pop es
	pop ds
	popad
	cmp byte ptr cs:[pm_chain + n], 0
	jz  @F
	jmp cs:[pm_old + n*PFARSIZE]
@@:
	insIRET
endm

	MAKETRAMP 0
	MAKETRAMP 1
	MAKETRAMP 2
	MAKETRAMP 3

;--- entry-point table, so Install can index the trampolines.
	align 4
pm_entry dd offset pmisr_0, offset pmisr_1, offset pmisr_2, offset pmisr_3

;--- int PMISR_Install( uint8_t vec, void (*fn)(void), int chain );
;--- returns the slot index, or -1 if the pool is full or the host refuses.

PMISR_Install proc public uses ebx esi edi vec:byte, fn:dword, chain:dword

	xor esi, esi			; esi = slot index
nextslot:
	cmp byte ptr cs:[pm_used + esi], 0
	jz  gotslot
	inc esi
	cmp esi, PMISR_SLOTS
	jc  nextslot
	jmp failed
gotslot:
	mov eax, esi
	mov edi, PFARSIZE
	mul edi				; eax = slot * sizeof(PFAR), edx dead
	mov edi, eax			; edi = byte offset into pm_old

;--- read the vector we are displacing BEFORE installing ours.  cx:edx must
;--- survive until they are stored, so nothing between here and the stores
;--- may use them.
	mov bl, vec
	mov ax, 204h
	int 31h
	jc  failed

ifdef NOTFLAT
	push ds
	mov ds, cs:[_csalias]
	assume ds:_TEXT
endif
	mov dword ptr [pm_old + edi], edx
	mov word ptr [pm_old + edi + SEGOFS], cx
	mov eax, fn
	mov dword ptr [pm_pfn + esi*4], eax
	mov eax, chain
	mov byte ptr [pm_chain + esi], al
	mov al, vec
	mov byte ptr [pm_vec + esi], al
	mov byte ptr [pm_used + esi], 1
	mov pmDS, ss
ifdef NOTFLAT
	pop ds
	assume ds:DGROUP
endif

	mov ecx, cs
	mov edx, cs:[pm_entry + esi*4]
	mov bl, vec
	mov ax, 205h
	int 31h
	jc  failed2
	mov eax, esi
	ret

failed2:
ifdef NOTFLAT
	push ds
	mov ds, cs:[_csalias]
	assume ds:_TEXT
endif
	mov byte ptr [pm_used + esi], 0
ifdef NOTFLAT
	pop ds
	assume ds:DGROUP
endif
failed:
	mov eax, -1
	ret

	align 4
PMISR_Install endp

;--- void PMISR_Uninstall( int slot );

PMISR_Uninstall proc public uses ebx esi edi slot:dword

	mov esi, slot
	cmp esi, PMISR_SLOTS
	jnc done
	cmp byte ptr cs:[pm_used + esi], 0
	jz  done

	mov eax, esi
	mov edi, PFARSIZE
	mul edi
	mov edi, eax
	mov edx, dword ptr cs:[pm_old + edi]
	mov cx,  word ptr cs:[pm_old + edi + SEGOFS]
	mov bl,  byte ptr cs:[pm_vec + esi]
	mov ax, 205h
	int 31h

ifdef NOTFLAT
	push ds
	mov ds, cs:[_csalias]
	assume ds:_TEXT
endif
	mov byte ptr [pm_used + esi], 0
ifdef NOTFLAT
	pop ds
	assume ds:DGROUP
endif
done:
	ret

	align 4
PMISR_Uninstall endp

	END
