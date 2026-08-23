#ifndef _HOSTISR_H_
#define _HOSTISR_H_

/* ==========================================================================
 *  Chained / iret protected-mode interrupt vectors, for both toolchains.
 *  Split out of hostsvc.h because it is the one piece that needs the system
 *  <dpmi.h>, and that header's __dpmi_meminfo collides with the fork's own
 *  djdpmi.h -- which linear.h pulls in, so the engine cannot see both. Only
 *  the card backends need this API, and they do not include linear.h.
 * ========================================================================== */

#ifdef DJGPP
# include <dpmi.h>       /* _go32_dpmi_* : the chain wrapper itself */
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- chained / iret protected-mode interrupt vectors -------------------
 * THE ONE PIECE WITH NO PORTABLE ANCESTOR, and the reason this port was
 * filed as "days, not hours". The IRQ0 watchdog heartbeat -- the
 * guest-independent host that resurrects a killed RTC within ~110ms --
 * installs through _go32_dpmi_chain_protected_mode_interrupt_vector, which
 * is a DJGPP libc facility, not a DPMI call: go32 allocates a trampoline
 * that saves registers, loads the app's selectors, calls the C function and
 * then jumps to the handler that was there before. Open Watcom has no
 * equivalent, so src/PMISR.ASM provides it -- built out of this codebase's
 * own dual-build ISR idiom (stackisr.asm's SwitchStackISR: private ISR
 * stack via __djgpp_stack_top, our DS fetched through a cs: word, insIRET
 * or a far jmp to the saved PFAR).
 *
 * Names and signature are unchanged from the copy each backend used to
 * carry privately, so no call site moved.
 *
 * BENCH-UNPROVEN. Under NOTFLAT this trampoline is entered with a 16-bit
 * interrupt frame on a stack the 16-bit host chose, and none of that has
 * met hardware. PMISR=1 ./tools/build.sh puts the SAME trampoline into the
 * 32-bit binary, so it can be A/B'd against go32 on the stack that IS
 * proven before anyone trusts it here. See doc/16BIT.md. */
#if defined(DJGPP) && !defined(PMISR_CHAIN)

typedef struct { int intno; _go32_dpmi_seginfo si, oldsi; } DPMI_ISR_HANDLE;

/* verbatim the helper each backend used to carry, and still a per-file
 * static: three identical copies is what the shipping binary contains, and
 * collapsing them to one would move every object after it. */
static int DPMI_InstallISR(int intno, void (*isr)(void), DPMI_ISR_HANDLE *h, int chain)
{
 h->intno = intno;
 if(_go32_dpmi_get_protected_mode_interrupt_vector(intno, &h->oldsi)) return -1;
 h->si.pm_offset = (unsigned long)isr;
 h->si.pm_selector = _go32_my_cs();
 if(chain)
  return _go32_dpmi_chain_protected_mode_interrupt_vector(intno, &h->si) ? -1 : 0;
 if(_go32_dpmi_allocate_iret_wrapper(&h->si)) return -1;
 return _go32_dpmi_set_protected_mode_interrupt_vector(intno, &h->si) ? -1 : 0;
}
static void DPMI_UninstallISR(DPMI_ISR_HANDLE *h)
{ _go32_dpmi_set_protected_mode_interrupt_vector(h->intno, &h->oldsi); }

#else

typedef struct { int intno; int slot; } DPMI_ISR_HANDLE;

int  DPMI_InstallISR(int intno, void (*isr)(void), DPMI_ISR_HANDLE *h, int chain);
void DPMI_UninstallISR(DPMI_ISR_HANDLE *h);

#endif

#ifdef __cplusplus
}
#endif

#endif /* _HOSTISR_H_ */
