#ifndef PTOPS_H
#define PTOPS_H

/* Per-card passthrough ops table.
 *
 * Historically sndisr.c/vsb.c reached the card backend through a cross-TU
 * ABI of ES1688_* symbols that every sc_* backend had to define, which is
 * exactly why only one backend could ever be linked into a binary. The
 * winning backend now registers this table from its adetect via
 * PTOPS_Register(); the engine dispatches through PT_Ops instead.
 *
 * PT_Ops is never NULL: it defaults to an engine-owned table whose dbg
 * hooks are the generic SNDISR_dbg_* telemetry below, so ISR-entry
 * accounting works even when no PCMCIA backend claimed the session (e.g.
 * an Audigy run in a CARD_AUDIGY build).
 */
struct pt_ops_s {
    unsigned flags;                     /* PTF_* below */
    /* passthrough tap (PTF_TAP backends; NULL on render-path cards) */
    int  (*space)(void);                /* guest bytes the ring accepts now */
    void (*feed)(const unsigned char *buf, int bytes,
                 unsigned rate, unsigned bits, unsigned channels);
    /* guest DSP-reset hook (vsb.c calls on every reset); NULL = none */
    void (*watchdog)(void);
    /* ISR diagnostics -- MUST be non-NULL (hot path is unguarded); point
     * them at the SNDISR_dbg_* defaults unless the card has its own
     * instrument (sc_tp755's IAC-window trio) */
    void (*dbg_tick)(void);
    void (*dbg_exit)(void);
    void (*dbg_reenter)(void);
    /* live ISR nesting depth for the sndisr depth limiter; NULL = none */
    int  (*depth)(void);
};

#define PTF_TAP      0x0001  /* arm the sndisr tap: raw guest DMA -> feed() */
#define PTF_REAL_FM  0x0002  /* real FM silicon behind 0x388 (no shim needed) */

extern const struct pt_ops_s *PT_Ops;

/* Called from the backend's adetect (init context, never ISR): installs the
 * table, derives SNDISR_PassThru from PTF_TAP, runs the one-time TSC probe
 * for the duration telemetry. */
void PTOPS_Register( const struct pt_ops_s *ops );

/* Runtime backend selection. SBECARD=es1688|vew211|tp755 pins the session to
 * one backend; unset means "probe in table order". Every backend's adetect
 * calls this first, so a user on an ambiguous box can always force the right
 * one without a rebuild. Init context only (it reads the environment). */
int PTOPS_CardWanted( const char *id );

/* Engine-generic ISR telemetry (the BIOS 0x4F0-0x4FF map -- see
 * doc/NOTES.md), formerly duplicated in sc_es1688.c and sc_vew211.c. */
void SNDISR_dbg_tick(void);
void SNDISR_dbg_exit(void);
void SNDISR_dbg_reenter(void);

extern int SNDISR_PassThru;             /* tap gate (set by PTOPS_Register) */
extern int SNDISR_HasTsc;               /* CPUID-verified TSC present */
extern volatile int SNDISR_ReviveSquelch; /* post-heal guest-IRQ squelch ticks
                                           * (backend guardian sets; sndisr
                                           * decrements and drops status) */
#endif
