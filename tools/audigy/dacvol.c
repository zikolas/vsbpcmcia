/* DACVOL.C -- set the Wolfson WM8768 DAC volume on the Audigy 2 ZS Notebook.
 *
 *   DACVOL          show what would be written (no change)
 *   DACVOL C0       rewrite every volume register with data byte C0
 *
 * This is the RIGHT place to attenuate on this board.  Turning the level down
 * in the FX engine (see FXVOL) works, but throws away bits and you hear the
 * quantisation noise; the DAC's own volume costs no resolution.
 *
 * ALSA's spi_dac_init[] leaves nine registers at 0xff (maximum).  We keep each
 * entry's register field exactly as ALSA has it and replace only the low data
 * byte, so this stays correct regardless of whether the word is packed as
 * 7-bit-addr/9-bit-data or 8/8 -- the register field is untouched either way.
 *
 * SPI goes through the p17v shim at register 0x3c, same as the driver uses.
 *
 * 16-bit real mode + 386 I/O opcodes, C89, OpenWatcom: wcc -ms
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <dos.h>

#define PTR2        0x20
#define DATA2       0x24
#define P17V_SPI    0x3c

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

/* the entries of ALSA's spi_dac_init[] whose data byte is 0xff, i.e. the
 * per-channel volume registers sitting at maximum */
static const unsigned vol_regs[] = {
    0x00ff, 0x02ff, 0x08ff, 0x0aff, 0x0cff, 0x0eff, 0x10ff, 0x1aff, 0x1cff
};

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

static unsigned long ptr20_read(unsigned reg)
{
    outd(g_io + PTR2, (unsigned long)reg << 16);
    return ind(g_io + DATA2);
}

static void ptr20_write(unsigned reg, unsigned long data)
{
    outd(g_io + PTR2, (unsigned long)reg << 16);
    outd(g_io + DATA2, data);
}

/* same handshake as ALSA's snd_emu10k1_spi_write() */
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

static int g_noupdate = 0;
static int g_raw_valid = 0;
static unsigned g_raw = 0;

int main(int argc, char **argv)
{
    int i, nregs, have_val = 0, bad = 0;
    unsigned val = 0, word;

    for (i = 1; i < argc; i++) {
        if (!strnicmp(argv[i], "/IO=", 4))
            g_io = (unsigned)strtoul(argv[i] + 4, NULL, 16);
        else if (!strnicmp(argv[i], "/W=", 3)) {
            g_raw = (unsigned)strtoul(argv[i] + 3, NULL, 16) & 0xFFFF;
            g_raw_valid = 1;
        } else if (!strnicmp(argv[i], "/NOU", 4))
            g_noupdate = 1;
        else if (argv[i][0] != '/' && argv[i][0] != '-') {
            val = (unsigned)strtoul(argv[i], NULL, 16) & 0xFF;
            have_val = 1;
        }
    }

    if (!g_io)
        g_io = find_card();
    if (!g_io) {
        printf("Audigy not found (run CBGO first?)\n");
        return 1;
    }
    printf("card I/O base %04X, SPI via p17v %02X\n", g_io, P17V_SPI);

    nregs = (int)(sizeof(vol_regs) / sizeof(vol_regs[0]));

    if (g_raw_valid) {           /* /W=xxxx : send one raw 16-bit SPI word */
        printf("raw SPI word %04X: %s\n", g_raw,
               spi_write(g_raw) ? "TIMED OUT" : "ok");
        return 0;
    }

    if (!have_val) {
        printf("%d volume registers, ALSA leaves them at FF (max):\n", nregs);
        for (i = 0; i < nregs; i++)
            printf("  %04X", vol_regs[i]);
        printf("\nusage: DACVOL <hex byte 00-FF>   e.g. DACVOL C0\n");
        printf("       /NOU  omit the volume-update bit\n");
        printf("       /W=xxxx  send one raw 16-bit SPI word\n");
        return 0;
    }

    /* The word is 7-bit register + 9-bit data.  Bit 8 of the data is the
     * volume UPDATE bit on this Wolfson part: without it the level is latched
     * but never applied, which is why plain byte writes did nothing.  Keep the
     * 7-bit register field, set the update bit, and supply the new level. */
    for (i = 0; i < nregs; i++) {
        word = (vol_regs[i] & 0xFE00) | val;
        if (!g_noupdate)
            word |= 0x0100;
        if (spi_write(word)) {
            printf("  %04X TIMED OUT\n", word);
            bad++;
        } else {
            printf("  %04X\n", word);
        }
    }
    printf("wrote level %02X to %d registers%s (%d failed)\n",
           val, nregs, g_noupdate ? "" : " with update bit", bad);
    return 0;
}
