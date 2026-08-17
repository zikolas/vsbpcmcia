/* Timer-only OPL3 status shim: makes an FM-LESS card detectable.
 *
 * WHY THIS EXISTS
 * A Sound Blaster's FM chip answers at the SB base aliases (base+0..3 and
 * base+8/9) as well as at 0x388, and era games probe it BEFORE they will
 * touch the DSP at all: Duke Nukem II runs a full AdLib timer test there and
 * silently concludes "no Sound Blaster" if it fails (see the FM_Alias notes
 * in ptrap.c). On the cards this driver normally serves that costs nothing --
 * an ES1688 has its ESFM and a CF-VEW211 a discrete YMF262, so the aliases
 * are simply forwarded to the real chip at 0x388.
 *
 * The ThinkPad 755C's planar CS4248 has NO FM chip anywhere. Forwarding to
 * 0x388 there reads open bus (0xFF), and the probe's first check is
 * "status & 0xE0 == 0" -- which 0xFF fails, and a floating 0x00 fails the
 * second check ("== 0xC0"). So on an FM-less card the guest loses not just
 * music but DIGITAL SOUND, because detection never gets past the FM gate.
 *
 * WHAT THIS IS NOT
 * It is not an OPL. There is no synthesis, no DBOPL::Chip, no libm, no
 * per-sample work -- nothing runs outside a trapped port access. Register
 * writes other than the timer control register are accepted and dropped, so
 * AdLib MUSIC stays silent on an FM-less card; what comes back is the card's
 * DIGITAL path, which is the part it actually has. (When a software OPL is
 * compiled in -- CARD_TP755/CARD_AUDIGY builds, where gvars.opl3 is set --
 * VOPL3 owns these ports instead and this shim is never installed.)
 *
 * The timer model is lifted from vopl3.cpp's VOPL3_PrimaryRead /
 * VOPL3_PrimaryWriteData (timers read back as expired the moment they are
 * started unmasked) so that a probe sees byte-for-byte what today's TP755
 * build already passes with the full emulation.
 */

#include <stdint.h>
#include <stdbool.h>   /* ptrap.h uses bool */

#include "CONFIG.H"
#include "PLATFORM.H"
#include "PTRAP.H"
#include "FMSHIM.H"

/* vopl3.cpp's names/values: the MASK constants are the STATUS bits each
 * timer reports (bit 7 = IRQ, bit 6 = T1, bit 5 = T2), not the control bits */
#define FMS_TIMER_REG_INDEX 4
#define FMS_TIMER1_MASK     0xC0
#define FMS_TIMER2_MASK     0xA0
#define FMS_TIMER1_START    0x01
#define FMS_TIMER2_START    0x02

static uint8_t fms_index[2];   /* index latch: [0] = 388/389, [1] = 38A/38B */
static uint8_t fms_timer[2];   /* last timer-control write, per timer */

void FMSHIM_Reset( void )
/////////////////////////
{
    fms_index[0] = fms_index[1] = 0;
    fms_timer[0] = fms_timer[1] = 0;
}

/* The OPL3 status register is shared by both port pairs, so a read of 0x38A
 * returns the same byte as 0x388 (vopl3.cpp's SecondaryRead falls through to
 * PrimaryRead for everything but the AdLib Gold volume regs, which need a
 * chip we do not have). */
static uint8_t FMSHIM_Status( void )
////////////////////////////////////
{
    uint8_t val = 0;
    if ( ( fms_timer[0] & ( FMS_TIMER1_MASK | FMS_TIMER1_START ) ) == FMS_TIMER1_START )
        val |= FMS_TIMER1_MASK;
    if ( ( fms_timer[1] & ( FMS_TIMER2_MASK | FMS_TIMER2_START ) ) == FMS_TIMER2_START )
        val |= FMS_TIMER2_MASK;
    return val;
}

uint8_t FMSHIM_Acc( uint16_t port, uint8_t val, uint16_t flags )
////////////////////////////////////////////////////////////////
{
    int bank = ( port >> 1 ) & 1;      /* 388/389 -> 0, 38A/38B -> 1 */

    if ( !( flags & TRAPF_OUT ) )
        return FMSHIM_Status();

    if ( port & 1 ) {                  /* data port */
        if ( bank == 0 && fms_index[0] == FMS_TIMER_REG_INDEX ) {
            /* Both halves are latched from the same byte, exactly as
             * VOPL3_PrimaryWriteData does -- starting one timer must not
             * discard the other's control state. */
            if ( val & ( FMS_TIMER1_START | FMS_TIMER1_MASK ) )
                fms_timer[0] = val;
            if ( val & ( FMS_TIMER2_START | FMS_TIMER2_MASK ) )
                fms_timer[1] = val;
        }
        /* any other register: accepted and dropped (no synthesis) */
    } else {                           /* index port */
        fms_index[bank] = val;
    }
    return val;
}
