#ifndef _CONFIG_H_
#define _CONFIG_H_

/* ES1688 486 build: passthrough SB ONLY. Software OPL3/MPU/soundfont are dead
 * weight on a 486, so strip them. NOFM also makes PTRAP skip the 0x388 trap
 * (ptrap.c:609) -> guest AdLib falls through to the card's real ESFM = hardware
 * FM for free. VMPU 0 = no 0x330 MPU trap. SOUNDFONT 0 = no TinySoundFont MIDI. */
/* VSBPCMCIA: our fork version (the banner shows this; VERMAJOR/VERMINOR in
 * version.h stay upstream's VSBHDA core version for lineage/merge sanity). */
#define VSBPCM_VER "0.6.1"

/* VSBPCMCIA card backend: default = ES1688 (sc_es1688.c). Define CARD_VEW211
 * (tools/build.sh CARD=VEW211 does it via CFLAGS) to build the Panasonic
 * CF-VEW211 / CS4231A backend instead -- the two provide the same passthrough
 * ABI (ES1688_PT_*) and are compile-time exclusive. */
/* #define CARD_VEW211 1 */

/* VSBPCMCIA is PCMCIA-only: none of the fleet has PCI, so the PCI card
 * drivers are dead weight (~15% of the binary). Excluded via upstream's own
 * per-card guards -- the source files stay in the tree untouched so
 * `git merge upstream/main` stays cheap. */
/* CARD_AUDIGY (tools/build.sh CARD=AUDIGY) is the exception: the Audigy 2 ZS
 * Notebook is a CardBus -- i.e. PCI -- card, so that build keeps sc_sbliv. */
#define NOES1371  1
#define NOICH     1
#define NOHDA     1
#ifndef CARD_AUDIGY
#define NOSBLIVE  1
#endif
#define NOVIA82   1

/* The ES1688/VEW211 fleet has real hardware FM at 0x388 to fall through to, so
 * stripping the software OPL3 is free there. The Audigy has none, so a
 * CARD_AUDIGY build must emulate it or AdLib music is simply silent. */
#ifndef CARD_AUDIGY
#define NOFM      1   /* strip DOSBox OPL3 emulation; leave 0x388 to the hardware */
#endif

#define SB16      1   /* 1=support SB16 */
#define ADPCM     1   /* 1=support ADPCM */
#define SETIF     1   /* KEEP 1: VSBHDA's guest IRQ injection needs interrupts on (SETIF=0
                       * starved the guest -> VSB never "running" -> no audio). Reentrancy of
                       * the heavy render path is instead bounded by the es_in_render guard in
                       * sndisr.c (SETIF's _enable_ints runs AFTER the injection, so guarding
                       * only the render loop is safe). MUST match config.inc. */
#define COMPAT4   1   /* 1=support /CF4 - mask PIT interrupt during snd interrupt */
#ifndef NOFM
#define REINITOPL 1   /* 1=reinit OPL during RESET */
#else
#define REINITOPL 0
#endif
#define SETABSVOL 0   /* 0=the master volume is set by /VOL cmdline option and isn't modified by the application */
/* CARD_AUDIGY: the 0x330 trap feeds the EMU10K2 hardware wavetable (emu_wt.c),
 * so VMPU comes back. SOUNDFONT comes with it: vmpu.c's MIDI parser lives
 * under that gate, and enabling both also makes struct globalvars agree with
 * config.inc's GLOBALVARS again (the ASM side has always been built with
 * VMPU=1/SOUNDFONT=1). TSF itself stays dormant unless /SF: is given. */
#ifdef CARD_AUDIGY
#define VMPU      1
#define SLOWDOWN  1   /* 1=support /SD option */
#define SOUNDFONT 1
#else
#define VMPU      0   /* ES1688: no MPU virtualization (inert on 486; guest 0x330 untrapped) */
#define SLOWDOWN  1   /* 1=support /SD option */
#define SOUNDFONT 0   /* ES1688: no soundfont MIDI (needs VMPU anyway) */
#endif
#define PT0V86    1   /* 1=support read access to v86 page table 0 */
#define HOSTRT    0   /* 0=protect SB irq, so it's not modified by programs; must match setting in SBISR.ASM */

#define HW_CHANNELS 2
#define HW_BITS 16

#if SOUNDFONT
#define VOICES_DEFAULT 64
#endif

#ifdef __cplusplus
extern "C"
{
#endif

struct globalvars {
	int base; /* /A value, default 220 */
	int irq;  /* /I value, default 7 */
	int dma;  /* /D value */
#if SB16
	int hdma; /* /H value */
#endif
	int type; /* /T value */
#if VMPU
	int mpu;  /* /P value, default 330 */
#endif
	int opl3;
	int rm;
	int pm;
	int vol;
	int buffsize;
#if SLOWDOWN
	int slowdown;
#endif
#if SOUNDFONT
	char *soundfont;
	int voices;
#endif
	int compatflags; /* compatibility flags */
	int freq;
	int pin;
	int device;
	int period_size;
#ifdef _DEBUG
	char *logfile;
#endif
	int fmvol; /* /FMVOL: TL steps added to carriers on the REAL OPL3; -1 = off.
	              Runtime default set in main() (the init above is positional). */
};

/* compatflags bits */
enum {
    CF_INSTANTIRQ = 1, /* "instant" SB interrupt triggered on DSP cmd 0xF2 */
    CF_STIONEOI   = 2, /* run STI when OUT 20h,al (al=20h) is trapped */
    CF_MASKPIT    = 4, /* mask PIT during Snd interrupt */
    CF_NODIVERR   = 8, /* 16-bit pmode only: catch divide error (borland rtm bug) */
};
enum {
    OM_DOS = 1,
    OM_DIRECT = 2,
    OM_DEBUGGER = 4,
    OM_BUFFERED = 8,  /* debug version only */
};

int _dprintf(const char *fmt, ... );
int IsDebuggerPresent( void );
void SetBorder( uint8_t );
void fatal_error( int );

#ifdef __cplusplus
}
#endif

#ifdef _DEBUG
#define dbgprintf(x) _dprintf x
void dbgcondbrk(void *);
int LogfileInit( void );
int LogfileExit( void );
#define LOGFILESIZE 0x1000000 /* size of logfile buffer (16 MB) */
#else
#define dbgprintf(x)
#define dbgcondbrk(x)
#endif

#endif//_CONFIG_H_
