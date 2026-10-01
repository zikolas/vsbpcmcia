#ifndef _FMSHIM_H_
#define _FMSHIM_H_
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Timer-only OPL3 status shim -- see FMSHIM.C. One trap handler serves all
 * four OPL ports; pass the 0x388-0x38B port (the SB-base FM aliases map onto
 * it first, exactly as FM_Alias does for real silicon). */
uint8_t FMSHIM_Acc( uint16_t port, uint8_t val, uint16_t flags );
void    FMSHIM_Reset( void );
/* /LPT: also send every write to an OPL3LPT at this LPT base (0 = off),
 * with `delay` control-port reads after each strobe (6 = FastDoom's).
 * Arming it silences the chip; so does FMSHIM_LptSilence at unload. */
void    FMSHIM_SetLpt( uint16_t base, int delay );
void    FMSHIM_LptSilence( void );
/* the /LPT v86 stub's index shadow and status cache, kept in step from here
 * (NULL, NULL = no such stub) */
void    FMSHIM_SetStubCells( uint8_t *index, uint8_t *status );

#ifdef __cplusplus
}
#endif

#endif//_FMSHIM_H_
