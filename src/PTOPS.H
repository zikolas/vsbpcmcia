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
    /* Max frames the engine may render in ONE ISR pass; 0 = no limit.
     * Only meaningful for a render-path card whose ISR is a fast pump: with
     * no cap, sndisr sizes the pass from AU_cardbuf_space(), which on an
     * empty ring asks for the WHOLE buffer (8192 frames on the VEW211) --
     * far more work than fits between two 2048 Hz pump ticks, so the machine
     * livelocks inside the ISR before a game makes a sound. The passthrough
     * path has always had the equivalent (PT_MODE_SAMPLES). */
    int  render_cap;
    /* Render on every Nth interrupt only; 0/1 = every one. The pump and the
     * engine render share one ISR, but they want opposite rates: the CS4231A
     * FIFO is 16 frames (~1.5 ms at 11 kHz) so feeding must be FAST, while
     * the render path (cardbuf_space + mixer + writedata) is heavy and wants
     * to be RARE -- sc_tp755 does it 43x/s. Running both at pump rate costs
     * ~24x the fixed ISR overhead on a 486, starves the game, and makes
     * passes overrun their tick (= missed feeds = static). So: pump every
     * tick, render every Nth. render_cap must cover one render interval. */
    int  render_div;
};

#define PTF_TAP      0x0001  /* arm the sndisr tap: raw guest DMA -> feed() */
#define PTF_REAL_FM  0x0002  /* real FM silicon behind 0x388 (no shim needed) */

extern const struct pt_ops_s *PT_Ops;

/* Called from the backend's adetect (init context, never ISR): installs the
 * table, derives SNDISR_PassThru from PTF_TAP, runs the one-time TSC probe
 * for the duration telemetry. */
void PTOPS_Register( const struct pt_ops_s *ops );

/* Fork-side driver options, filled by main.c's command-line parser.
 * Deliberately NOT fields of struct globalvars: that layout is an ABI shared
 * with config.inc on the ASM side, and these are ours alone.
 *
 * These were environment variables until 2026-08-17. Switches replace them
 * because env values PERSIST BETWEEN RUNS: a base or a card name left over
 * from an earlier launcher silently redirected the next one, which is why
 * every batch grew a row of defensive "set X=" clears. A switch dies with
 * the invocation, so that whole class of bug cannot occur. Genuinely
 * transient bench knobs (SBERTC, SBEPTLAT, SBENORS, ESNOI8, IRQTONE,
 * FIFOTEST) stay in the environment on purpose: they are set once at a
 * prompt for one experiment and never select hardware. */
struct fork_opts_s {
    const char *card;   /* /CARD: backend name; NULL = not given (fatal) */
    int base;           /* /BASE: the REAL card's I/O base; 0 = card default.
                         * NOT /A, which is the EMULATED SB base. One switch
                         * for all three cards is unambiguous only because
                         * /CARD is mandatory -- exactly one backend reads it. */
    int dacrate;        /* /DACRATE: codec rate, Hz; 0 = card default */
    int cvol;           /* /CVOL: codec DAC attenuation 0-63; -1 = default */
    int fmshim;         /* /FMSHIM: force the FM-less detection path */
    int resamp;         /* /RESAMP: engine resampling instead of passthrough
                         * (VEW211 for now) -- the codec runs at ONE fixed
                         * table rate and SBEMU resamples every guest format
                         * onto it, so the frame stepper never engages. */
};
extern struct fork_opts_s FOpts;

/* Does /CARD name this backend? Case-insensitive, exact. There is no
 * probing any more: a backend runs only when it is asked for by name. */
int PTOPS_CardIs( const char *id );

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
