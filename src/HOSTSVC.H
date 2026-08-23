#ifndef _HOSTSVC_H_
#define _HOSTSVC_H_

/* ==========================================================================
 *  HOST SERVICES -- the handful of things DJGPP and Open Watcom spell
 *  differently, in one place, so the card backends can be built twice.
 *  --------------------------------------------------------------------
 *  WHY THIS FILE EXISTS. vsbpcm.exe is a 32-bit DPMI client and can only
 *  serve 32-bit protected-mode games; DPMI 0.9 gives 16-bit clients their
 *  own interrupt table (vsbhda.txt:51-57), so 16-bit PM titles -- Tyrian and
 *  the rest of the Borland RTM / Phar Lap 286 / DOS16M catalogue -- need a
 *  SECOND binary that is itself a 16-bit client: vsbpcm16.exe, built by
 *  tools/build16.sh. The engine was already dual-build (upstream maintains
 *  djgpp.mak AND ow16.mak); the DJGPP-only code is ours -- the three PCMCIA
 *  backends and the telemetry we added to sndisr.c. This header is that
 *  code's compatibility layer.
 *
 *  THE INVARIANT, AND WHY IT SHAPES EVERYTHING BELOW. Every DJGPP branch
 *  here is VERBATIM the text it replaced -- same bodies, same storage class,
 *  same types, and still a per-file static rather than one shared copy -- so
 *  that the 32-bit build's generated code does not move. With no bench to
 *  retest on, that is the only safety property available, so it was measured
 *  rather than assumed: a pre-port tree was reconstructed (it rebuilds to
 *  the shipping sha256 6ce34ce3... exactly) and every object compared
 *  instruction by instruction.
 *      sc_es1688.o, sc_vew211.o, sndisr.o  IDENTICAL instruction streams;
 *                                          only a static's NAME changed
 *                                          (DPMI_DisableInterrupt ->
 *                                          HOST_DisableInterrupt).
 *      sc_tp755.o                          same function set; gcc allocated
 *                                          differently around the now-inlined
 *                                          DPMI 0100h helper. TP755_adetect
 *                                          +4 instructions, TP755_close one
 *                                          push from memory instead of an
 *                                          immediate. INIT AND TEARDOWN ONLY:
 *                                          checked function by function, the
 *                                          ISR and render paths are identical
 *                                          (TP755_getbufpos and tp755_pt_ops
 *                                          appear in the tool's attribution
 *                                          only because the padding after
 *                                          them disassembles as junk and
 *                                          shifts).
 *      everything else                     unchanged.
 *  Reproduce with tools/cmp32.sh. (That was the state at the PORT. The
 *  SBENOI8/SBENOI5 bench knobs added during the first 16-bit bench campaign
 *  move sc_es1688.o deliberately -- runtime-gated, load-path only.)
 *
 *  It is also why the two interrupt-flag flavours below are kept side by
 *  side instead of unified: sc_es1688/sc_vew211 go through the DPMI host
 *  (int 31h ax=0900h/0901h) and sc_tp755 uses a raw cli/sti, and deciding
 *  which is right is a bench question, not a port question.
 *
 *  WHAT "16-BIT" MEANS HERE. The generated code is still 32-bit (.386, USE32
 *  segments -- startup/cstrt16x.asm is headed "DOS 32-bit startup code for
 *  16-bit client"). What is 16-bit is the DPMI CLIENT TYPE: the host is
 *  entered through its 16:16 API entry, so interrupt frames are words not
 *  dwords (config.inc switches insIRET/insPUSHF/SEGOFS/PFAR on NOTFLAT) and
 *  the host files us in its 16-bit interrupt table. Two consequences drive
 *  most of what follows:
 *    - DS has a NON-ZERO BASE, so a linear address is not a pointer and
 *      never was -- linear.h's NearPtr subtracts DSBase in both builds.
 *      (The DS LIMIT is a full 4GB in both; see the low-memory note below.)
 *    - the MZ relocation format is why upstream splits ITS 16-bit build
 *      into vsbhda16.exe + sndcard.drv; ours links ONE module instead --
 *      the backends are two-way coupled to the engine, and CSGT64K plus
 *      the 4GB DGROUP make the split unnecessary. See ow16.mak.
 * ========================================================================== */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef DJGPP
# include <go32.h>
# include <sys/farptr.h>
# include <pc.h>
#else
# include <conio.h>
/* djdpmi.h has this one, but the backends cannot include djdpmi.h -- it is
 * what collides with the system <dpmi.h>. sc_tp755 frees its DMA buffer with
 * it; the prototype is djdpmi.h's, verbatim. */
extern int __dpmi_free_dos_memory(int _selector);
/* DJGPP's pc.h spells the port accessors inportb/outportb; Open Watcom's
 * conio.h spells them inp/outp and marks them #pragma intrinsic, so this is
 * a rename at compile time, not a call. (pcibios.h does the same thing in
 * the other direction for upstream's PCI drivers: inb -> inp.) */
# define inportb(p)      ((unsigned char)inp((unsigned)(p)))
# define outportb(p,v)   ((void)outp((unsigned)(p),(unsigned)(v)))
#endif

/* ---- low memory (BIOS data area + the 0x4F0 telemetry block) -----------
 * Addresses stay ABSOLUTE (0x46C, 0x4F2, ...) exactly as the DJGPP code
 * spelled them, so the diff at the ~40 call sites is a rename and nothing
 * more. The engine's map for 0x4F0-0x4FF is in doc/NOTES.md.
 *
 * BOTH builds reach low memory the same way, and that is worth spelling out
 * because it is the opposite of what "16-bit" suggests. The 64K cap on the
 * NOTFLAT build is on the MODULE (the MZ relocation format), not on the
 * runtime descriptor: startup/init1632.asm re-bases DGROUP and then sets its
 * limit to 0xFFFFFFFF -- "for linear access, we need full 4GB descriptor".
 * So a near pointer wraps round to any linear address here exactly as it
 * does under DJGPP, and linear.h's NearPtr is the whole mechanism. No extra
 * selector, no far pointers.
 *
 * NearPtr is re-rolled rather than included: linear.h drags in the fork's
 * djdpmi.h, which collides with the system <dpmi.h> above. */
extern uint32_t DSBase;
#define LOW_Window(addr)      ((uint8_t *)((uint32_t)(addr) - DSBase))
#define LOW_WinClear(p,n)     memset((p), 0, (n))

#ifdef DJGPP
# define LOW_PokeB(addr,val)  _farpokeb(_dos_ds, (addr), (val))
# define LOW_PeekD(addr)      _farpeekl(_dos_ds, (addr))
#else
# define LOW_PokeB(addr,val)  (*(volatile uint8_t *)LOW_Window(addr) = (uint8_t)(val))
# define LOW_PeekD(addr)      (*(volatile uint32_t *)LOW_Window(addr))
#endif

/* ---- virtual interrupt flag ------------------------------------------ */
#ifdef DJGPP
/* verbatim from sc_es1688.c / sc_vew211.c -- do not "improve" */
static uint8_t HOST_DisableInterrupt(void)
{
 unsigned f;
 __asm__ __volatile__("pushfl; popl %0" : "=r"(f));
 __asm__ __volatile__("movw $0x0900,%%ax; int $0x31" ::: "eax","cc");
 return (f & 0x200) ? 1 : 0;
}
static void HOST_RestoreInterrupt(uint8_t prev)
{ if(prev) __asm__ __volatile__("movw $0x0901,%%ax; int $0x31" ::: "eax","cc"); }

/* verbatim from sc_tp755.c */
static uint8_t HOST_CliSave(void)
{
 uint32_t f;
 __asm__ __volatile__("pushfl; popl %0; cli":"=r"(f)::"memory");
 return (uint8_t)((f >> 9) & 1);
}
static void HOST_StiRestore(uint8_t on)
{
 if(on) __asm__ __volatile__("sti":::"memory");
}
#else
/* RAW int 31h, deliberately NOT platform.h's _disable_ints/_enable_ints:
 * that pair reaches the host through cs:[oldint31], and oldint31 is only
 * filled in by _InstallInt31 -- main.c line ~747. The RTC pump and the IRQ0
 * heartbeat are already ticking from AU_init (main.c line ~616), and
 * irq_routine disables interrupts on EVERY tick, so the first tick to land
 * in that window far-called through NULL: the 235's first GO16 boot hung
 * hard with no exception report (2026-08-23) -- died inside an ISR. The
 * DJGPP original is a direct int $0x31 with no ordering dependency
 * whatsoever; these pragmas are its exact Open Watcom spelling. (The
 * engine's own oldint31 route is correct FOR THE ENGINE: upstream installs
 * every ISR before anything can tick.) */
extern unsigned HOST_getflags(void);
#pragma aux HOST_getflags =     \
    "pushf"                     \
    "pop  ax"                   \
    parm []                     \
    value [eax]                 \
    modify exact [eax];

extern void HOST_dpmi_vif_off(void);
#pragma aux HOST_dpmi_vif_off = \
    "mov  ax, 900h"             \
    "int  31h"                  \
    parm []                     \
    modify exact [eax];

extern void HOST_dpmi_vif_on(void);
#pragma aux HOST_dpmi_vif_on =  \
    "mov  ax, 901h"             \
    "int  31h"                  \
    parm []                     \
    modify exact [eax];

extern void HOST_cli(void);
#pragma aux HOST_cli = "cli" parm [] modify exact [];
extern void HOST_sti(void);
#pragma aux HOST_sti = "sti" parm [] modify exact [];

static uint8_t HOST_DisableInterrupt(void)
{
 unsigned f = HOST_getflags();
 HOST_dpmi_vif_off();
 return (f & 0x200) ? 1 : 0;
}
static void HOST_RestoreInterrupt(uint8_t prev)
{ if(prev) HOST_dpmi_vif_on(); }

static uint8_t HOST_CliSave(void)
{
 unsigned f = HOST_getflags();
 HOST_cli();
 return (uint8_t)((f >> 9) & 1);
}
static void HOST_StiRestore(uint8_t on)
{ if(on) HOST_sti(); }
#endif

/* ---- CPUID / TSC ------------------------------------------------------
 * rdtsc #UDs on a real 486 -- the fleet this port exists for -- so every
 * use is gated on this one-time probe (SNDISR_HasTsc). Never call it from
 * an ISR: it flips the EFLAGS ID bit to decide whether CPUID exists.
 * NOTFLAT only; the DJGPP build keeps its own copy in sndisr.c so that
 * build's object code does not move. */
#ifndef DJGPP
int HOST_HasTsc(void);
#endif

/* ---- DOS conventional memory (DPMI 0100h) -----------------------------
 * Returns the real-mode segment, or -1; *sel gets the selector to free
 * with __dpmi_free_dos_memory. sc_tp755 uses this for its DMA buffer. */
#ifdef DJGPP
/* verbatim from sc_tp755.c's tp_dos_alloc, and still a per-file static so
 * that build's object code does not move */
static int HOST_DosAlloc(unsigned paragraphs, int *sel)
{
 uint32_t eax = 0x0100, edx = 0;
 uint8_t err;
 __asm__ __volatile__("int $0x31; setc %0"
                      : "=q"(err), "+a"(eax), "=d"(edx)
                      : "b"(paragraphs)
                      : "cc", "memory");
 if(err) return -1;
 *sel = (int)(edx & 0xFFFF);
 return (int)(eax & 0xFFFF);
}
#else
int HOST_DosAlloc(unsigned paragraphs, int *sel);
#endif

/* The chained / iret protected-mode interrupt-vector API is NOT here: it
 * needs the system <dpmi.h>, whose __dpmi_meminfo collides with the fork's
 * own djdpmi.h (which linear.h pulls in, so sndisr.c and the engine cannot
 * see both). It lives in hostisr.h, which only the card backends include. */

#ifdef __cplusplus
}
#endif

#endif /* _HOSTSVC_H_ */
