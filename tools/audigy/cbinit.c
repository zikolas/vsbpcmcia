/* CBINIT.C -- Audigy 2 ZS Notebook (SB0530) CardBus wake-up sequence.
 *
 * The CA0108 on this card comes up with its I/O register file inert: it
 * claims cycles in its BAR but never completes a read, which hard-hangs the
 * host.  Linux's snd_emu10k1_cardbus_init() calls this out directly -- the
 * poke sequence at port+0x38 runs "before the rest of the IO-Ports become
 * active".  Writes complete fine on an uninitialised card, so we can run the
 * whole sequence blind.
 *
 * Linux interleaves dummy reads between the writes, but they are discarded
 * (__always_unused), so by default we issue writes only and substitute a
 * short delay.  /R adds the reads back for cards where they matter.
 *
 *   CBINIT [/IO=1400] [/R] [/NOVOL]
 *
 * Run after CBGO.  Does not read anything by default, so it cannot wedge the
 * machine; test reads afterwards with CBTEST /D.
 *
 * 16-bit real mode + 386 I/O opcodes, C89, OpenWatcom: wcc -ms
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <dos.h>

#define PTR2            0x20    /* indexed register set 2 pointer */
#define DATA2           0x24    /* indexed register set 2 data    */
#define SPECIAL         0x38    /* CardBus wake-up port           */
#define TINA2_VOLUME    0x71    /* without it output is 12dB hot  */

unsigned long ind(unsigned port);
#pragma aux ind =           \
    0x66 0xED               /* in    eax, dx                      */ \
    0x66 0x8B 0xD0          /* mov   edx, eax                     */ \
    0x66 0xC1 0xEA 0x10     /* shr   edx, 16                      */ \
    parm [dx]               \
    value [dx ax]           \
    modify [dx ax];

void outd(unsigned port, unsigned long val);
#pragma aux outd =          \
    0x66 0x0F 0xB7 0xC0     /* movzx eax, ax  ; eax = val low     */ \
    0x66 0xC1 0xE2 0x10     /* shl   edx, 16  ; edx = val high<<16*/ \
    0x66 0x0B 0xC2          /* or    eax, edx ; eax = full value  */ \
    0x8B 0xD1               /* mov   dx, cx   ; dx  = port        */ \
    0x66 0xEF               /* out   dx, eax                      */ \
    parm [cx] [dx ax]       \
    modify [dx ax];

static unsigned g_io = 0x1400;
static int g_reads = 0;
static int g_vol = 1;
static char g_msg[160];

static void logstr(char *s)
{
    FILE *f;

    printf("%s\n", s);
    f = fopen("CBINIT.LOG", "a");
    if (f) {
        fprintf(f, "%s\n", s);
        fclose(f);
    }
}

static void tick_delay(int ticks)
{
    unsigned long volatile __far *bios;
    unsigned long start;

    bios = (unsigned long volatile __far *)MK_FP(0x0040, 0x006C);
    start = *bios;
    while ((*bios - start) < (unsigned long)ticks)
        /* spin */;
}

/* short settling gap standing in for Linux's discarded dummy read */
static void io_pause(void)
{
    int i;

    for (i = 0; i < 4000; i++)
        inp(0x80);
}

static void poke(unsigned long v)
{
    if (g_reads) {
        sprintf(g_msg, "  read  io+38 (dummy)");
        logstr(g_msg);
        ind(g_io + SPECIAL);
    }
    sprintf(g_msg, "  write io+38 = %08lX", v);
    logstr(g_msg);
    outd(g_io + SPECIAL, v);
    io_pause();
}

int main(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        if (argv[i][0] != '/' && argv[i][0] != '-')
            continue;
        if (!strnicmp(argv[i] + 1, "IO=", 3))
            g_io = (unsigned)strtoul(argv[i] + 4, NULL, 16);
        else if (!strnicmp(argv[i] + 1, "NOVOL", 5))
            g_vol = 0;
        else if (argv[i][1] == 'R' || argv[i][1] == 'r')
            g_reads = 1;
    }

    logstr("--- CBINIT start ---");
    sprintf(g_msg, "io=%04X  dummy-reads=%s", g_io, g_reads ? "yes" : "no");
    logstr(g_msg);

    /* the sequence from snd_emu10k1_cardbus_init() */
    poke(0x00D00000UL);
    poke(0x00D00001UL);
    poke(0x00D0005FUL);
    poke(0x00D0007FUL);
    poke(0x0090007FUL);

    if (g_vol) {
        logstr("  TINA2_VOLUME <- FEFEFEFE (playback attenuation)");
        outd(g_io + PTR2, ((unsigned long)TINA2_VOLUME << 16) | 0UL);
        outd(g_io + DATA2, 0xFEFEFEFEUL);
    }

    tick_delay(5);          /* Linux sleeps 200ms here */

    logstr("--- CBINIT done (no reads performed) ---");
    return 0;
}
