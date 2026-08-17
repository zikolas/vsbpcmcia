/* Host-side test of the timer-only FM shim: replays the exact AdLib timer
 * probe (the gate Duke Nukem II runs before it will touch the DSP) and the
 * SB-base alias mapping FM_Alias applies. */
#include <stdio.h>
#include <stdint.h>
#include "FMSHIM.H"
#define OUT 4
#define IN  0
static int fails;
static void chk(const char *what, int got, int want) {
    printf("  %-52s got %02X want %02X  %s\n", what, got, want,
           got == want ? "ok" : "FAIL");
    if (got != want) fails++;
}
/* what FM_Alias does for an SB-base alias port */
static uint8_t alias(uint16_t sbport, uint8_t val, uint16_t flags) {
    return FMSHIM_Acc(0x388 + (sbport & 3), val, flags);
}
int main(void) {
    uint8_t s;
    printf("\n[1] AdLib timer probe direct at 388h/389h (DN2 sequence)\n");
    FMSHIM_Reset();
    FMSHIM_Acc(0x388, 4, OUT); FMSHIM_Acc(0x389, 0x60, OUT);   /* mask both */
    FMSHIM_Acc(0x388, 4, OUT); FMSHIM_Acc(0x389, 0x80, OUT);   /* IRQ reset */
    s = FMSHIM_Acc(0x388, 0, IN);
    chk("status after 60h/80h (probe needs &E0 == 00)", s & 0xE0, 0x00);
    FMSHIM_Acc(0x388, 2, OUT); FMSHIM_Acc(0x389, 0xFF, OUT);   /* T1 preload */
    FMSHIM_Acc(0x388, 4, OUT); FMSHIM_Acc(0x389, 0x21, OUT);   /* start T1 */
    s = FMSHIM_Acc(0x388, 0, IN);
    chk("status after FFh/21h (probe needs &E0 == C0)", s & 0xE0, 0xC0);
    printf("  => a card with this shim %s the AdLib gate\n",
           fails ? "FAILS" : "PASSES");

    printf("\n[2] same probe through the SB-base aliases at 228h/229h\n");
    FMSHIM_Reset();
    alias(0x228, 4, OUT); alias(0x229, 0x60, OUT);
    alias(0x228, 4, OUT); alias(0x229, 0x80, OUT);
    chk("alias status after 60h/80h", alias(0x228, 0, IN) & 0xE0, 0x00);
    alias(0x228, 2, OUT); alias(0x229, 0xFF, OUT);
    alias(0x228, 4, OUT); alias(0x229, 0x21, OUT);
    chk("alias status after FFh/21h", alias(0x228, 0, IN) & 0xE0, 0xC0);

    printf("\n[3] secondary port pair shares the status register\n");
    chk("read 38Ah mirrors 388h", FMSHIM_Acc(0x38A, 0, IN),
        FMSHIM_Acc(0x388, 0, IN));

    printf("\n[4] non-timer traffic is inert (no synthesis, no state)\n");
    FMSHIM_Reset();
    FMSHIM_Acc(0x388, 0x20, OUT); FMSHIM_Acc(0x389, 0x01, OUT); /* op regs */
    FMSHIM_Acc(0x388, 0xA0, OUT); FMSHIM_Acc(0x389, 0x98, OUT);
    FMSHIM_Acc(0x388, 0xB0, OUT); FMSHIM_Acc(0x389, 0x31, OUT); /* KEY-ON */
    chk("status still 00 after a note-on burst", FMSHIM_Acc(0x388, 0, IN), 0x00);
    FMSHIM_Acc(0x38A, 4, OUT); FMSHIM_Acc(0x38B, 0x21, OUT);   /* 2nd bank */
    chk("timer write via the SECONDARY index is ignored",
        FMSHIM_Acc(0x388, 0, IN), 0x00);

    printf("\n[5] masked timer must not report expiry (0xE0 gate stays clean)\n");
    FMSHIM_Reset();
    FMSHIM_Acc(0x388, 4, OUT); FMSHIM_Acc(0x389, 0x61, OUT);  /* start+mask T1 */
    chk("T1 started but MASKED reports no timeout",
        FMSHIM_Acc(0x388, 0, IN) & 0xE0, 0x00);

    printf("\n%s (%d failure%s)\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED",
           fails, fails == 1 ? "" : "s");
    return fails != 0;
}
