/* FMVOL - carrier-level FM volume for a real OPL3 behind the port traps.
 * See fmvol.h.  Logic is a straight port of FMVOL.ASM (vew21xgo repo),
 * which is hardware-validated on the CF-VEW211's YMF262; keep the two in
 * step if either changes.
 *
 * (C) 2026 zikolas.  GPL v2, like the rest of this tree (see COPYING).
 * Ported from our own MIT FMVOL.ASM; as its author we relicense this port
 * under GPL v2 here.  Do not copy GPL code from this tree back into the
 * MIT vew21xgo repo.
 */
#include <stdint.h>
#include <stdbool.h>
#include "platform.h"
#include "ptrap.h"
#include "fmvol.h"

#define FV_OUTB(p,v)  UntrappedIO_OUT_Handler((uint16_t)(p),(uint8_t)(v))
#define FV_INB(p)     UntrappedIO_IN_Handler((uint16_t)(p))

static int      fv_steps = -1;          /* TL steps to add; <0 = disabled */
static uint16_t fv_base  = 0x388;

/* live shadow of the chip, so we know which operators are heard */
static uint8_t fv_curreg[2];            /* latched register, per bank */
static uint8_t fv_rhythm;               /* BDh bit 5 */
static uint8_t fv_fourop;               /* 104h bits 0..5 */
static uint8_t fv_cnt[18];              /* connection bit, 9 ch x 2 banks */
static uint8_t fv_origtl[44];           /* the level the guest asked for,
                                           WHOLE byte: KSL bits included */
static uint8_t fv_havetl[44];           /* 1 once we have seen a level */

/* operator offset (00h-15h) -> channel, and whether it is that channel's
 * second operator.  0xFF marks the holes in the OPL map. */
static const uint8_t fv_opch[0x16] = {
    0,1,2, 0,1,2, 0xFF,0xFF,
    3,4,5, 3,4,5, 0xFF,0xFF,
    6,7,8, 6,7,8
};
static const uint8_t fv_opis2[0x16] = {
    0,0,0, 1,1,1, 0,0,
    0,0,0, 1,1,1, 0,0,
    0,0,0, 1,1,1
};
/* first operator of each channel, for putting levels back */
static const uint8_t fv_chop1[9] = { 0x00,0x01,0x02, 0x08,0x09,0x0A, 0x10,0x11,0x12 };

static void fv_delay(void)
{
    (void)FV_INB(0x80); (void)FV_INB(0x80);
}

/* is operator `op` of `bank` heard directly, so that its level is the
 * volume? */
static int fv_iscarrier(unsigned bank, unsigned op)
{
    unsigned ch, is2;
    if (op >= 0x16) return 0;
    ch = fv_opch[op];
    if (ch == 0xFF) return 0;
    is2 = fv_opis2[op];

    /* rhythm mode, bank 0 only: the four one-operator drums are all heard;
     * the bass drum (ch6) is an ordinary pair - the 2-op rule is right */
    if (!bank && fv_rhythm && ch >= 7)
        return 1;

    /* part of a 4-operator pair?  channels 6..8 never are */
    if (ch < 6) {
        unsigned pair = (ch < 3) ? ch : ch - 3;
        unsigned tail = (ch >= 3);
        uint8_t  bit  = (uint8_t)((1u << pair) << (bank ? 3 : 0));
        if (fv_fourop & bit) {
            /* op4 always; op1 with CNT1; op2 with CNT2; op3 when both */
            uint8_t cnt1 = fv_cnt[bank*9 + pair];
            uint8_t cnt2 = fv_cnt[bank*9 + pair + 3];
            if (tail) return is2 ? 1 : (cnt1 && cnt2);
            return is2 ? (cnt2 != 0) : (cnt1 != 0);
        }
    }

    /* an ordinary 2-operator channel */
    if (is2) return 1;
    return fv_cnt[bank*9 + ch] != 0;
}

/* return the level byte with the TL raised by fv_steps if the operator is
 * heard; the KSL bits (6-7) always survive */
static uint8_t fv_scale(unsigned bank, unsigned op, uint8_t val)
{
    unsigned tl;
    if (fv_steps <= 0) return val;
    if (!fv_iscarrier(bank, op)) return val;    /* modulator: timbre alone */
    tl = (val & 0x3F) + (unsigned)fv_steps;
    if (tl > 0x3F) tl = 0x3F;                   /* 63 is silent; never wrap */
    return (uint8_t)((val & 0xC0) | tl);
}

/* write operator `op` of `bank` back to the chip with the current scaling */
static void fv_putlevel(unsigned bank, unsigned op)
{
    unsigned slot = bank*22 + op;
    uint16_t ap = (uint16_t)(fv_base + bank*2);
    if (!fv_havetl[slot]) return;
    FV_OUTB(ap, 0x40 + op);   fv_delay();
    FV_OUTB(ap+1, fv_scale(bank, op, fv_origtl[slot])); fv_delay();
}

/* put the guest's own register selection back, so the extra writes above
 * are invisible to it */
static void fv_relatch(void)
{
    FV_OUTB(fv_base,   fv_curreg[0]); fv_delay();
    FV_OUTB(fv_base+2, fv_curreg[1]); fv_delay();
}

/* one trapped access.  bank 0 = 388/389, bank 1 = 38A/38B; `data` set for
 * the data port of the pair. */
static uint8_t fv_io(unsigned bank, unsigned data, uint8_t val, uint16_t flags)
{
    uint16_t port = (uint16_t)(fv_base + bank*2 + data);
    unsigned reg;

    if (!(flags & TRAPF_OUT))
        return FV_INB(port);                    /* status from the real chip */

    if (!data) {                                /* address latch: remember it */
        fv_curreg[bank] = val;
        FV_OUTB(port, val);
        return val;
    }

    reg = fv_curreg[bank];

    if (reg >= 0x40 && reg <= 0x55) {           /* Total Level */
        unsigned op = reg - 0x40;
        fv_origtl[bank*22 + op] = val;          /* whole byte: KSL survives */
        fv_havetl[bank*22 + op] = 1;
        FV_OUTB(port, fv_scale(bank, op, val));
        return val;
    }

    if (reg >= 0xC0 && reg <= 0xC8) {           /* feedback / connection */
        unsigned ch = reg - 0xC0;
        uint8_t  c  = val & 1;
        if (fv_cnt[bank*9 + ch] != c) {
            fv_cnt[bank*9 + ch] = c;
            FV_OUTB(port, val);                 /* the guest's write first */
            /* the carriers of this channel may have just moved */
            fv_putlevel(bank, fv_chop1[ch]);
            fv_putlevel(bank, fv_chop1[ch] + 3);
            /* half of an active 4-op pair?  the algorithm mixes BOTH
             * channels' connection bits, so the partner's moved as well */
            if (ch < 6) {
                unsigned pair    = (ch < 3) ? ch : ch - 3;
                unsigned partner = (ch < 3) ? ch + 3 : ch - 3;
                uint8_t  bit     = (uint8_t)((1u << pair) << (bank ? 3 : 0));
                if (fv_fourop & bit) {
                    fv_putlevel(bank, fv_chop1[partner]);
                    fv_putlevel(bank, fv_chop1[partner] + 3);
                }
            }
            fv_relatch();
            return val;
        }
        FV_OUTB(port, val);
        return val;
    }

    /* BDh and 104h changes also move carriers, but no resend on purpose:
     * programs set the mode at init and write levels after, and any stale
     * level heals on the very next TL write anyway. */
    if (reg == 0xBD && !bank) fv_rhythm = val & 0x20;
    if (reg == 0x04 &&  bank) fv_fourop = val & 0x3F;

    FV_OUTB(port, val);
    return val;
}

uint8_t FMVOL_388(uint16_t port, uint8_t val, uint16_t flags) { (void)port; return fv_io(0,0,val,flags); }
uint8_t FMVOL_389(uint16_t port, uint8_t val, uint16_t flags) { (void)port; return fv_io(0,1,val,flags); }
uint8_t FMVOL_38A(uint16_t port, uint8_t val, uint16_t flags) { (void)port; return fv_io(1,0,val,flags); }
uint8_t FMVOL_38B(uint16_t port, uint8_t val, uint16_t flags) { (void)port; return fv_io(1,1,val,flags); }

void FMVOL_Init(int steps, uint16_t fmbase)
{
    unsigned i;
    if (steps > 63) steps = 63;
    fv_steps = steps;
    if (fmbase) fv_base = fmbase;
    fv_curreg[0] = fv_curreg[1] = 0;
    fv_rhythm = fv_fourop = 0;
    for (i = 0; i < sizeof(fv_cnt);    i++) fv_cnt[i] = 0;
    for (i = 0; i < sizeof(fv_origtl); i++) { fv_origtl[i] = 0; fv_havetl[i] = 0; }
}

int FMVOL_Active(void)
{
    return fv_steps >= 0;
}
