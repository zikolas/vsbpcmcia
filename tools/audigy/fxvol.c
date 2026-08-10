/* FXVOL.C -- read/set the EMU10K2 FX-engine master volume GPRs, live.
 *
 *   FXVOL                show GPR 8 / GPR 9
 *   FXVOL 08000000       set both (hex, full scale = 7FFFFFFF)
 *   FXVOL %10            set both to 10% of full scale
 *
 * sc_sbliv.c's fx_init multiplies the PCM bus by A_GPR(8)/A_GPR(9) on the way
 * to the external outputs, and defaults them to 66% of 0x7fffffff.  This is
 * the real fader in the digital path -- unlike TINA2_VOLUME, which on the ZS
 * Notebook behaves close to a mute switch.  Use it to tell hard clipping
 * (quieter => clean) from mangled samples (quieter => still noise).
 *
 * A_FXGPREGBASE = 0x400, so GPR n lives at PTR reg 0x400+n, channel 0.
 * Safe at the DOS prompt: the driver only touches PTR/DATA while the emulated
 * SB is actually running.
 *
 * 16-bit real mode + 386 I/O opcodes, C89, OpenWatcom: wcc -ms
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <dos.h>

#define EMU_PTR         0x00
#define EMU_DATA        0x04
#define A_FXGPREGBASE   0x400

unsigned long ind(unsigned port);
#pragma aux ind =           \
    0x66 0xED               \
    0x66 0x8B 0xD0          \
    0x66 0xC1 0xEA 0x10     \
    parm [dx]               \
    value [dx ax]           \
    modify [dx ax];

void outd(unsigned port, unsigned long val);
#pragma aux outd =          \
    0x66 0x0F 0xB7 0xC0     \
    0x66 0xC1 0xE2 0x10     \
    0x66 0x0B 0xC2          \
    0x8B 0xD1               \
    0x66 0xEF               \
    parm [cx] [dx ax]       \
    modify [dx ax];

static unsigned g_io = 0;

static unsigned cfg_rw(unsigned bus, unsigned devfn, unsigned reg)
{
    union REGS r;

    memset(&r, 0, sizeof(r));
    r.h.ah = 0xB1; r.h.al = 0x09;
    r.h.bh = (unsigned char)bus;
    r.h.bl = (unsigned char)devfn;
    r.x.di = reg;
    int86(0x1A, &r, &r);
    if (r.x.cflag || r.h.ah)
        return 0xFFFF;
    return r.x.cx;
}

static unsigned find_card(void)
{
    union REGS r;

    memset(&r, 0, sizeof(r));
    r.h.ah = 0xB1; r.h.al = 0x02;
    r.x.cx = 0x0008;
    r.x.dx = 0x1102;
    r.x.si = 0;
    int86(0x1A, &r, &r);
    if (r.x.cflag || r.h.ah)
        return 0;
    return cfg_rw(r.h.bh, r.h.bl, 0x10) & 0xFFFC;
}

static unsigned long gpr_read(unsigned n)
{
    outd(g_io + EMU_PTR, ((unsigned long)(A_FXGPREGBASE + n) << 16));
    return ind(g_io + EMU_DATA);
}

static void gpr_write(unsigned n, unsigned long v)
{
    outd(g_io + EMU_PTR, ((unsigned long)(A_FXGPREGBASE + n) << 16));
    outd(g_io + EMU_DATA, v);
}

int main(int argc, char **argv)
{
    int i, have_val = 0;
    unsigned long val = 0;

    for (i = 1; i < argc; i++) {
        if (!strnicmp(argv[i], "/IO=", 4)) {
            g_io = (unsigned)strtoul(argv[i] + 4, NULL, 16);
        } else if (argv[i][0] == '%') {
            unsigned long pct = strtoul(argv[i] + 1, NULL, 10);
            if (pct > 100) pct = 100;
            val = (0x7FFFFFFFUL / 100UL) * pct;
            have_val = 1;
        } else if (argv[i][0] != '/' && argv[i][0] != '-') {
            val = strtoul(argv[i], NULL, 16);
            have_val = 1;
        }
    }

    if (!g_io)
        g_io = find_card();
    if (!g_io) {
        printf("Audigy not found (run CBGO first?)\n");
        return 1;
    }
    printf("card I/O base %04X\n", g_io);
    printf("GPR8=%08lX GPR9=%08lX  (full scale 7FFFFFFF)\n",
           gpr_read(8), gpr_read(9));

    if (have_val) {
        gpr_write(8, val);
        gpr_write(9, val);
        printf("wrote %08lX -> GPR8=%08lX GPR9=%08lX\n",
               val, gpr_read(8), gpr_read(9));
    } else {
        printf("usage: FXVOL <hex>   or   FXVOL %%<percent>\n");
    }
    return 0;
}
