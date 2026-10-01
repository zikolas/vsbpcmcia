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
 * /LPT: AN OPL3 ON THE PARALLEL PORT
 * An OPL3LPT is a real YMF262 behind the printer port. It is write-only --
 * the status register never reaches the port -- so a stock game cannot
 * detect it, which is the half this shim already provides. With /LPT every
 * index and data write is also sent out the parallel port as it arrives, so
 * the guest drives the chip exactly as it would at 0x388. Status reads still
 * come from the timer model below. For real-mode guests the 32-bit build
 * installs a stub variant that does the same from V86 without an RMCB
 * (rmcode1.asm, LPTSTUB) and sends only timer writes here.
 *
 * The timer model is lifted from vopl3.cpp's VOPL3_PrimaryRead /
 * VOPL3_PrimaryWriteData (timers read back as expired the moment they are
 * started unmasked) so that a probe sees byte-for-byte what today's TP755
 * build already passes with the full emulation.
 */

#include <stdint.h>
#include <stdbool.h>   /* ptrap.h uses bool */
#include <dos.h>       /* includes pc.h; for outp() */
#include <conio.h>     /* contains outp()/inp() in OW */

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
static uint16_t fms_lpt;       /* /LPT: OPL3LPT data port; 0 = no chip */
static int fms_lpt_dly = 6;    /* control-port reads after each strobe */

/* The /LPT v86 stub (rmcode1.asm, LPTSTUB) sends most index and data writes
 * to the LPT itself, so its index shadow -- not fms_index -- knows which
 * register a data write reaches, and it answers status reads from a cache
 * this file keeps current. Both cells live in the stub; NULL without it. */
static uint8_t *fms_stub_index;
static uint8_t *fms_stub_status;

/* SBEFMPATCH: era drivers pad each register write with dummy status reads
 * (DMX: 6 after the index, 24 after the value), and in protected mode each
 * one is a full HDPMI trap -- DOOM's music lost tempo on a DX4/75. ptrap.c
 * can rewrite such a read's IN AL,DX to NOP in the guest's code, as ADLiPT
 * does in V86. A read only counts as padding while the last index written
 * is 20h or above: AdLib detection runs on registers 1-4 and reads status
 * for real there. The chip's own spacing comes from the LPT write delays. */
static uint8_t fms_last_index; /* last index written, either array */
static int fms_patch;          /* SBEFMPATCH set */

/* OPL3LPT wire protocol: the byte goes out on the data lines, then the
 * control port pulses it into the chip -- 13/9/13 latches an index for the
 * first register array, 5/1/5 for the second, 12/8/12 latches a value. The
 * OPL3 needs 3.3 us after either write; six control-port reads cover it on
 * ISA (SBELPTDLY changes the count). Sequence and timing as in FastDoom's
 * OPL3LPT support (ns_sbmus.c).
 * The LPT ports are never trapped, so plain outp()/inp() is safe here. */
static void LptPulse( uint8_t on, uint8_t off )
{
    uint16_t ctrl = fms_lpt + 2;
    int i;
    outp( ctrl, on );
    outp( ctrl, off );
    outp( ctrl, on );
    for ( i = 0; i < fms_lpt_dly; i++ )
        inp( ctrl );
}

static void LptIndex( int bank, uint8_t idx )
{
    outp( fms_lpt, idx );
    if ( bank )
        LptPulse( 5, 1 );
    else
        LptPulse( 13, 9 );
}

static void LptData( uint8_t val )
{
    outp( fms_lpt, val );
    LptPulse( 12, 8 );
}

static void LptReg( int bank, uint8_t idx, uint8_t val )
{
    LptIndex( bank, idx );
    LptData( val );
}

/* Key every voice off with full attenuation and the fastest release, in both
 * arrays, then drop back to OPL2 mode as at power-up. Zeroing the registers
 * instead would leave release rate 0 on any note still decaying, and that
 * note would then hang at its current level. */
void FMSHIM_LptSilence( void )
//////////////////////////////
{
    int b, r;
    if ( !fms_lpt )
        return;
    LptReg( 1, 0x05, 0x01 );           /* NEW: open the second array */
    for ( b = 0; b < 2; b++ ) {
        for ( r = 0x40; r <= 0x55; r++ )
            LptReg( b, r, 0x3F );      /* total level: full attenuation */
        for ( r = 0x80; r <= 0x95; r++ )
            LptReg( b, r, 0x0F );      /* release rate 15 */
        for ( r = 0xB0; r <= 0xB8; r++ )
            LptReg( b, r, 0x00 );      /* key off */
    }
    LptReg( 0, 0xBD, 0x00 );           /* rhythm mode off, drums keyed off */
    LptReg( 1, 0x04, 0x00 );           /* no 4-op pairs */
    LptReg( 1, 0x05, 0x00 );           /* OPL2 mode */
}

void FMSHIM_SetLpt( uint16_t base, int delay )
//////////////////////////////////////////////
{
    fms_lpt = base;
    fms_lpt_dly = delay;
    FMSHIM_LptSilence();
}

void FMSHIM_Reset( void )
/////////////////////////
{
    fms_index[0] = fms_index[1] = 0;
    fms_timer[0] = fms_timer[1] = 0;
    if ( fms_stub_status )
        *fms_stub_status = 0;
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

void FMSHIM_SetPatch( int on )
//////////////////////////////
{
    fms_patch = on;
}

int FMSHIM_IsDelayRead( void )
//////////////////////////////
{
    return fms_patch && fms_last_index >= 0x20;
}

void FMSHIM_SetStubCells( uint8_t *index, uint8_t *status )
///////////////////////////////////////////////////////////
{
    fms_stub_index  = index;
    fms_stub_status = status;
    if ( index )
        *index = fms_index[0];
    if ( status )
        *status = FMSHIM_Status();
}

uint8_t FMSHIM_Acc( uint16_t port, uint8_t val, uint16_t flags )
////////////////////////////////////////////////////////////////
{
    int bank = ( port >> 1 ) & 1;      /* 388/389 -> 0, 38A/38B -> 1 */

    if ( !( flags & TRAPF_OUT ) )
        return FMSHIM_Status();

    if ( port & 1 ) {                  /* data port */
        uint8_t idx = fms_stub_index ? *fms_stub_index : fms_index[0];
        if ( bank == 0 && idx == FMS_TIMER_REG_INDEX ) {
            /* Both halves are latched from the same byte, exactly as
             * VOPL3_PrimaryWriteData does -- starting one timer must not
             * discard the other's control state. */
            if ( val & ( FMS_TIMER1_START | FMS_TIMER1_MASK ) )
                fms_timer[0] = val;
            if ( val & ( FMS_TIMER2_START | FMS_TIMER2_MASK ) )
                fms_timer[1] = val;
            if ( fms_stub_status )
                *fms_stub_status = FMSHIM_Status();
        }
        /* without /LPT any other register is accepted and dropped */
        if ( fms_lpt )
            LptData( val );
    } else {                           /* index port */
        fms_index[bank] = val;
        fms_last_index = val;
        if ( fms_stub_index )
            *fms_stub_index = val;
        if ( fms_lpt )
            LptIndex( bank, val );
    }
    return val;
}
