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

#ifdef __cplusplus
}
#endif

#endif//_FMSHIM_H_
