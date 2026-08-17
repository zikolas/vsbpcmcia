/* AUDMIX.C -- interactive mixer for the Audigy 2 ZS Notebook wavetable stack.
 *
 * Four sliders, live while music plays:
 *
 *   MAIN    WM8768 DAC volume over SPI (analogue -- costs no bits)
 *   MASTER  FX-engine master, GPR 8/9 (everything digital)
 *   WAVE    SB digital + PCM stream group, GPR 10 (FX buses 0/1)
 *   MIDI    hardware wavetable group, GPR 11 (FX buses 4/5)
 *
 * WAVE/MIDI need the VSBPCMA build whose DSP program pre-mixes the groups
 * (GPR 10/11); on an older driver they read as zero -- leave them alone.
 *
 * Keys: Up/Down select  Left/Right adjust  PgUp/PgDn coarse  M mute  ESC quit
 *
 * A driver reload rewrites everything here to its defaults -- this is a live
 * tuning tool, not persistence.  The value you like belongs in the driver.
 *
 * 16-bit real mode + 386 I/O opcodes, C89, OpenWatcom: wcc -ms
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <dos.h>

#define EMU_PTR     0x00
#define EMU_DATA    0x04
#define PTR2        0x20
#define DATA2       0x24
#define P17V_SPI    0x3c
#define GPR_BASE    0x400

#define N_SLIDERS   4
#define STEPS       32          /* slider resolution */

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

/* ---------------------------------------------------------------- PCI find */

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

/* -------------------------------------------------- register access, cli'd */
/* The driver's ISR shares the PTR index register; keep each pair atomic.   */

static unsigned long gpr_read(unsigned gpr)
{
    unsigned long v;
    _disable();
    outd(g_io + EMU_PTR, (unsigned long)(GPR_BASE + gpr) << 16);
    v = ind(g_io + EMU_DATA);
    _enable();
    return v;
}

static void gpr_write(unsigned gpr, unsigned long val)
{
    _disable();
    outd(g_io + EMU_PTR, (unsigned long)(GPR_BASE + gpr) << 16);
    outd(g_io + EMU_DATA, val);
    _enable();
}

static unsigned long ptr20_read(unsigned reg)
{
    unsigned long v;
    _disable();
    outd(g_io + PTR2, (unsigned long)reg << 16);
    v = ind(g_io + DATA2);
    _enable();
    return v;
}

static void ptr20_write(unsigned reg, unsigned long data)
{
    _disable();
    outd(g_io + PTR2, (unsigned long)reg << 16);
    outd(g_io + DATA2, data);
    _enable();
}

/* WM8768 volume registers (ALSA's spi_dac_init[] 0xff entries) */
static const unsigned vol_regs[] = {
    0x00ff, 0x02ff, 0x08ff, 0x0aff, 0x0cff, 0x0eff, 0x10ff, 0x1aff, 0x1cff
};

static int spi_write(unsigned data)
{
    unsigned long reset, set, tmp;
    int n;

    tmp   = ptr20_read(P17V_SPI);
    reset = (tmp & ~0x3FFFFUL) | 0x20000UL;
    set   = reset | 0x10000UL;

    ptr20_write(P17V_SPI, reset | data);
    tmp = ptr20_read(P17V_SPI);
    ptr20_write(P17V_SPI, set | data);
    for (n = 0; n < 100; n++) {
        int k;
        for (k = 0; k < 40; k++)
            inp(0x80);              /* ~10us */
        tmp = ptr20_read(P17V_SPI);
        if (!(tmp & 0x10000UL))
            break;
    }
    if (n >= 100)
        return 1;
    ptr20_write(P17V_SPI, reset | data);
    tmp = ptr20_read(P17V_SPI);
    return 0;
}

static void dac_set(unsigned level)     /* 0x00 - 0xFF, update bit included */
{
    int i;
    for (i = 0; i < (int)(sizeof(vol_regs) / sizeof(vol_regs[0])); i++)
        spi_write((vol_regs[i] & 0xFE00) | 0x0100 | (level & 0xFF));
}

/* ------------------------------------------------------------- the mixer  */

/* audio-taper: gain = (step/STEPS)^2 * 0x7fffffff, step table precomputed
 * at startup so the loop stays integer-only */
static unsigned long taper[STEPS + 1];

static void taper_init(void)
{
    int i;
    for (i = 0; i <= STEPS; i++) {
        unsigned long q = (unsigned long)i * i;             /* 0..1024   */
        taper[i] = (0x7FFFFFFFUL / (STEPS * STEPS)) * q;    /* ~unity top */
    }
    taper[STEPS] = 0x7FFFFFFFUL;
}

static int taper_nearest(unsigned long v)
{
    int i, best = 0;
    unsigned long bd = 0xFFFFFFFFUL;
    for (i = 0; i <= STEPS; i++) {
        unsigned long d = (v > taper[i]) ? v - taper[i] : taper[i] - v;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

struct slider {
    const char *name;
    const char *desc;
    int  step;              /* 0..STEPS */
    int  mute;
    int  saved;             /* step to restore on unmute */
};

static struct slider sl[N_SLIDERS] = {
    { "MAIN  ", "WM8768 DAC (analogue)      ", 0, 0, 0 },
    { "MASTER", "FX master, GPR 8/9         ", 0, 0, 0 },
    { "WAVE  ", "SB digital / PCM, GPR 10   ", 0, 0, 0 },
    { "MIDI  ", "wavetable, GPR 11          ", 0, 0, 0 },
};

static void apply(int idx)
{
    int st = sl[idx].mute ? 0 : sl[idx].step;
    switch (idx) {
    case 0:
        /* DAC level: linear byte scale is already gentle enough */
        dac_set((unsigned)(st * 255 / STEPS));
        break;
    case 1:
        gpr_write(8, taper[st]);
        gpr_write(9, taper[st]);
        break;
    case 2:
        gpr_write(10, taper[st]);
        break;
    case 3:
        gpr_write(11, taper[st]);
        break;
    }
}

/* ------------------------------------------------------------ direct video */

static unsigned short far *scr;

static void put(int row, int col, const char *s, unsigned char attr)
{
    unsigned short far *p = scr + row * 80 + col;
    while (*s)
        *p++ = ((unsigned short)attr << 8) | (unsigned char)*s++;
}

static void draw(int sel)
{
    int i, j, row;
    char buf[81];

    put(0, 0, "                                                                                ", 0x1F);
    put(0, 2, "AUDMIX -- Audigy 2 ZS Notebook wavetable stack mixer", 0x1F);
    put(1, 0, "                                                                                ", 0x07);
    put(2, 2, "Up/Down select   Left/Right adjust   PgUp/PgDn coarse   M mute   ESC quit", 0x07);

    for (i = 0; i < N_SLIDERS; i++) {
        unsigned char a = (i == sel) ? 0x70 : 0x07;
        row = 4 + i * 3;
        sprintf(buf, " %s %s", sl[i].name, sl[i].desc);
        put(row, 1, buf, a);

        buf[0] = '[';
        for (j = 0; j < STEPS; j++)
            buf[1 + j] = (j < sl[i].step && !sl[i].mute) ? '#' : '-';
        buf[STEPS + 1] = ']';
        sprintf(buf + STEPS + 2, " %3d%% %s",
                sl[i].step * 100 / STEPS, sl[i].mute ? "MUTED " : "      ");
        put(row + 1, 3, buf, sl[i].mute ? 0x0C : a);
    }

    put(18, 2, "MAIN is write-only hardware: its slider starts at the driver default. ", 0x08);
    put(19, 2, "Changes are live; a driver reload restores the driver's own defaults. ", 0x08);
}

int main(int argc, char **argv)
{
    int sel = 0, i, running = 1;

    for (i = 1; i < argc; i++)
        if (!strnicmp(argv[i], "/IO=", 4))
            g_io = (unsigned)strtoul(argv[i] + 4, NULL, 16);

    if (!g_io)
        g_io = find_card();
    if (!g_io) {
        printf("Audigy not found (run the stack first: GOWT)\n");
        return 1;
    }

    taper_init();

    /* seat the sliders from the hardware where it can be read back */
    sl[0].step = 0xEF * STEPS / 255;            /* ZSNB_DAC_VOLUME default */
    sl[1].step = taper_nearest(gpr_read(8));
    sl[2].step = taper_nearest(gpr_read(10));
    sl[3].step = taper_nearest(gpr_read(11));

    scr = (unsigned short far *)MK_FP(0xB800, 0);
    /* clear screen via BIOS scroll for a clean start */
    {
        union REGS r;
        memset(&r, 0, sizeof(r));
        r.h.ah = 0x06; r.h.al = 0; r.h.bh = 0x07;
        r.x.cx = 0; r.x.dx = 0x184F;
        int86(0x10, &r, &r);
    }

    while (running) {
        int c;
        draw(sel);
        c = getch();
        if (c == 0 || c == 0xE0) {
            c = getch();
            switch (c) {
            case 72: if (sel > 0) sel--; break;                 /* up    */
            case 80: if (sel < N_SLIDERS - 1) sel++; break;     /* down  */
            case 75:                                            /* left  */
                if (sl[sel].step > 0) { sl[sel].step--; apply(sel); }
                break;
            case 77:                                            /* right */
                if (sl[sel].step < STEPS) { sl[sel].step++; apply(sel); }
                break;
            case 73:                                            /* pgup  */
                sl[sel].step += 4;
                if (sl[sel].step > STEPS) sl[sel].step = STEPS;
                apply(sel);
                break;
            case 81:                                            /* pgdn  */
                sl[sel].step -= 4;
                if (sl[sel].step < 0) sl[sel].step = 0;
                apply(sel);
                break;
            }
        } else if (c == 'm' || c == 'M') {
            if (!sl[sel].mute) {
                sl[sel].saved = sl[sel].step;
                sl[sel].mute = 1;
            } else {
                sl[sel].mute = 0;
                sl[sel].step = sl[sel].saved;
            }
            apply(sel);
        } else if (c == 27) {
            running = 0;
        }
    }

    /* leave the mix as set; just restore a sane text screen */
    {
        union REGS r;
        memset(&r, 0, sizeof(r));
        r.h.ah = 0x06; r.h.al = 0; r.h.bh = 0x07;
        r.x.cx = 0; r.x.dx = 0x184F;
        int86(0x10, &r, &r);
    }
    printf("AUDMIX: levels left as set (driver reload restores defaults).\n");
    return 0;
}
