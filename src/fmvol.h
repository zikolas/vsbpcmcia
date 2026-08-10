#ifndef FMVOL_H
#define FMVOL_H
/* FMVOL - FM volume for a REAL OPL3 behind the port traps.
 *
 * (C) 2026 zikolas.  GPL v2, like the rest of this tree (see COPYING).
 * The algorithm is ported from our own FMVOL.ASM JLM (vew21xgo repo, MIT);
 * as its author we relicense this port under GPL v2 here.  The MIT master
 * copy stays in vew21xgo - do not copy GPL code from this tree back there.
 *
 * The CF-VEW211's YMF262 sums into the output amp after the codec with no
 * attenuator anywhere on the card, so the only volume the OPL model offers
 * is each operator's Total Level - and that belongs to the game.  These
 * handlers sit on ports 388h-38Bh (both trap worlds: QPI/V86 for real-mode
 * games, HDPMI32i port traps for protected-mode ones - the half a JLM can
 * never reach), rescale every Total Level write aimed at a CARRIER
 * operator, and forward everything to the real chip.  Modulators pass
 * untouched: attenuating a modulator changes the timbre, not the volume.
 *
 * Ported from our hardware-validated FMVOL.ASM JLM (vew21xgo repo),
 * including the KSL-preserve, bass-drum-additive and 4-op-partner-resend
 * fixes.  Guest reads are forwarded from the real chip (timer status).
 */
#include <stdint.h>

void    FMVOL_Init(int steps, uint16_t fmbase); /* steps 0..63; <0 = off */
int     FMVOL_Active(void);                     /* 1 when armed */

uint8_t FMVOL_388(uint16_t port, uint8_t val, uint16_t flags);
uint8_t FMVOL_389(uint16_t port, uint8_t val, uint16_t flags);
uint8_t FMVOL_38A(uint16_t port, uint8_t val, uint16_t flags);
uint8_t FMVOL_38B(uint16_t port, uint8_t val, uint16_t flags);

#endif
