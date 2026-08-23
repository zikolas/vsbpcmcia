/* ==========================================================================
 *  HOST SERVICES -- the out-of-line half of hostsvc.h. See that header for
 *  why this layer exists and for the byte-identical-32-bit-build invariant.
 * ========================================================================== */

#include <stdint.h>
#include <stdlib.h>

#include "PLATFORM.H"
#include "HOSTSVC.H"
#include "HOSTISR.H"

/* Under DJGPP without PMISR_CHAIN every service here is a per-file static in
 * hostsvc.h and this file is deliberately empty -- which is why djgpp.mak
 * does not build it, and why the 32-bit object set did not change at all. */
#if !defined(DJGPP) || defined(PMISR_CHAIN)

/* ---- CPUID / TSC ------------------------------------------------------
 * NOTFLAT only. The DJGPP build keeps its copy of this probe inside
 * sndisr.c so that build's object code does not move; see hostsvc.h. */

#ifndef DJGPP
/* EFLAGS.ID (bit 21) is writable only where CPUID exists. Never in an ISR. */
extern unsigned HOST_id_toggles(void);
#pragma aux HOST_id_toggles =   \
    "pushfd"                    \
    "pop  eax"                  \
    "mov  edx, eax"             \
    "xor  eax, 200000h"         \
    "push eax"                  \
    "popfd"                     \
    "pushfd"                    \
    "pop  eax"                  \
    "push edx"                  \
    "popfd"                     \
    "xor  eax, edx"             \
    "and  eax, 200000h"         \
    parm []                     \
    value [eax]                 \
    modify exact [eax edx];

extern unsigned HOST_cpuid1_edx(void);
#pragma aux HOST_cpuid1_edx =   \
    "mov  eax, 1"               \
    "cpuid"                     \
    "mov  eax, edx"             \
    parm []                     \
    value [eax]                 \
    modify exact [eax ebx ecx edx];

int HOST_HasTsc(void)
{
    if(!HOST_id_toggles()) return 0;     /* ID stuck -> no CPUID -> 486-class */
    return (HOST_cpuid1_edx() >> 4) & 1;
}
#endif

/* ---- DOS conventional memory (DPMI 0100h) ----------------------------- */

#ifndef DJGPP
/* CF is captured into ECX before any arithmetic touches it. 0xFFFFFFFF is
 * the failure marker; a genuine seg:sel of FFFF:FFFF cannot occur. */
extern unsigned long HOST_dosalloc_raw(unsigned paragraphs);
#pragma aux HOST_dosalloc_raw = \
    "mov   ax, 100h"            \
    "int   31h"                 \
    "sbb   ecx, ecx"            \
    "movzx eax, ax"             \
    "movzx edx, dx"             \
    "shl   edx, 16"             \
    "or    eax, edx"            \
    "or    eax, ecx"            \
    parm [ebx]                  \
    value [eax]                 \
    modify exact [eax ecx edx];
#endif

#ifndef DJGPP
int HOST_DosAlloc(unsigned paragraphs, int *sel)
{
    unsigned long r = HOST_dosalloc_raw(paragraphs);
    if(r == 0xFFFFFFFFUL) return -1;
    *sel = (int)((r >> 16) & 0xFFFF);
    return (int)(r & 0xFFFF);
}
#endif

/* ---- chained / iret protected-mode interrupt vectors ------------------
 * src/PMISR.ASM: a fixed pool of trampolines, one per installed vector.
 * PMISR_Install returns the slot index, or -1 when the pool is full or the
 * host refuses. */
extern int  PMISR_Install(unsigned char vec, void (*fn)(void), int chain);
extern void PMISR_Uninstall(int slot);

int DPMI_InstallISR(int intno, void (*isr)(void), DPMI_ISR_HANDLE *h, int chain)
{
    h->intno = intno;
    h->slot  = PMISR_Install((unsigned char)intno, isr, chain);
    return (h->slot < 0) ? -1 : 0;
}

void DPMI_UninstallISR(DPMI_ISR_HANDLE *h)
{
    if(h->slot >= 0){ PMISR_Uninstall(h->slot); h->slot = -1; }
}

#endif /* !DJGPP || PMISR_CHAIN */
