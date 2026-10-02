#ifndef _JLMSHARE_H_
#define _JLMSHARE_H_
#include <stdint.h>
#include <stddef.h>

/* The block VSBPCM shares with VSBPCMJ.DLL, its ring-0 companion JLM
 * (jlm/VSBPCMJ.ASM; the SH_* offsets there must match this layout). It lives
 * in DOS memory, where the JLM's ring-0 code and VSBPCM's protected-mode code
 * reach it at the same linear address.
 *
 * Without the JLM the same structure is a static in ptrap.c and carries only
 * the direct-DAC ring: protected-mode and QPI-trapped direct DAC fill it from
 * vsb.c, and sndisr.c drains it either way. 32-bit build only (DACRING). */

#define VSBJ_SIG     0x4A425356UL   /* "VSBJ" */
#define VSBJ_VER     5
#define VSBJ_RING    2048           /* DAC ring bytes, a power of two */
#define VSBJ_DEVID   0x5650         /* the JLM's device id (INT 2Fh 1684h) */

#define VSBJ_F_FM    0x01           /* FM ports: timer shim, /LPT, patching */
#define VSBJ_F_DAC   0x02           /* the DSP write port */
#define VSBJ_F_PATCH 0x04           /* FM delay reads rewritten to NOP */
#define VSBJ_F_POLL  0x08           /* DSP write-status polls patched out */
#define VSBJ_F_PIC   0x10           /* port 20h at ring 0 unless we virtualize */
#define VSBJ_F_PAIR  0x20           /* 10h-and-sample sequences made one trap */
#define VSBJ_F_V86   0x40           /* ... and then a V86 call, no trap */

struct vsbj_share {
    uint32_t sig;                   /* 00 VSBJ_SIG */
    uint16_t ver;                   /* 04 VSBJ_VER */
    uint16_t ringsize;              /* 06 VSBJ_RING */
    uint16_t lpt;                   /* 08 OPL3LPT data port; 0 = status only */
    uint8_t  lptdly;                /* 0A control-port reads after a strobe */
    uint8_t  flags;                 /* 0B VSBJ_F_* asked for */
    uint16_t sbbase;                /* 0C emulated SB base */
    uint16_t pad0;                  /* 0E */
    uint32_t qpicb;                 /* 10 seg:off of our QPI stub */
    uint32_t wscell;                /* 14 linear address of the DSP
                                     *    write-status counter */
    volatile uint8_t  dspidle;      /* 18 vsb.c: 1 = the next base+0Ch write
                                     *    is a command byte */
    volatile uint8_t  pend10;       /* 19 1 = a 10h was taken, its sample is
                                     *    the next write */
    volatile uint16_t dhead;        /* 1A DAC ring: producer index */
    volatile uint16_t dtail;        /* 1C           consumer index (sndisr) */
    volatile uint16_t dacrate;      /* 1E sndisr: the rate direct DAC is fed
                                     *    to the card at, Hz (diagnostic) */
    volatile uint32_t dcount;       /* 20 DAC samples received (wraps) */
    volatile uint32_t dlost;        /* 24 ... dropped, ring full */
    volatile uint32_t fmw;          /* 28 JLM: FM writes */
    volatile uint32_t fmr;          /* 2C JLM: FM status reads */
    volatile uint32_t fmpatch;      /* 30 JLM: IN AL,DX made NOP */
    volatile uint32_t dacfwd;       /* 34 JLM: DSP writes handed to our stub */
    volatile uint32_t dacws;        /* 38 JLM: DSP write-status reads */
    volatile uint32_t armed;        /* 3C JLM: its version, written at arm */
    /* bench black box, read with a memory dump after a game */
    volatile uint32_t wsip;         /* 40 JLM: last write-status read: the
                                     *    linear address of its IN ... */
    volatile uint8_t  wscode[8];    /* 44 ... and the code from there */
    volatile uint32_t wrip;         /* 4C JLM: last direct-DAC write (OUT) */
    volatile uint8_t  wrcode[8];    /* 50 */
    volatile uint32_t dcalls;       /* 58 sndisr: direct-DAC drains */
    volatile uint32_t dfed;         /* 5C ... samples fed to the card */
    volatile uint32_t dflush;       /* 60 ... dropped at a stream's end */
    volatile uint32_t dmaxdt;       /* 64 ... longest time between two
                                     *    drains, 1/32768 s */
    volatile uint32_t dspace;       /* 68 ... drains the card's room cut short */
    volatile uint32_t dnorate;      /* 6C ... drains held for a first estimate */
    volatile uint16_t dmaxocc;      /* 70 ... most samples in the ring at a
                                     *    drain */
    volatile uint16_t dstreams;     /* 72 ... streams started */
    volatile uint16_t drestart;     /* 74 ... rate changes found */
    volatile uint16_t dgaps;        /* 76 ... sample-less ticks inside a stream */
    /* the pump guard (VSBPCMJ.ASM pulse) */
    volatile uint32_t tick;         /* 78 sndisr: counts its interrupts */
    volatile uint8_t  guard;        /* 7C 1 = the JLM keeps our RTC pump alive */
    uint8_t  pad2[3];               /* 7D */
    volatile uint16_t rtcpie;       /* 80 JLM: RTC periodic interrupt found off */
    volatile uint16_t rtcmask;      /* 82 JLM: IRQ8 found masked */
    volatile uint16_t rtcheal;      /* 84 JLM: stuck RTC flags cleared */
    uint16_t pad3;                  /* 86 */
    volatile uint32_t dsppatch;     /* 88 JLM: DSP status polls patched out */
    uint32_t piccell;               /* 8C linear address of the stub's PIC word
                                     *    (0FFFFh: an SB IRQ is virtualized) */
    volatile uint32_t picio;        /* 90 JLM: port 20h accesses done at ring 0 */
    volatile uint32_t dacpair;      /* 94 JLM: 10h+sample pairs made one trap */
    volatile uint32_t v86puts;      /* 98 samples queued by ringput, in V86 */
    volatile uint32_t v86patch;     /* 9C JLM: sequences made calls to ringput */
    volatile uint8_t  ring[VSBJ_RING]; /* A0 */
    uint8_t  v86code[96];           /* 8A0 JLM: its copy of ringput (16-bit) */
    /* sndisr.c only, past everything the JLM uses */
    volatile uint16_t secmin;       /* 900 lowest one-second arrival rate, Hz */
    volatile uint16_t secmax;       /* 902 highest */
    volatile uint16_t dmaxroom;     /* 904 most card room left unfilled at a
                                     *     drain while samples were arriving */
    volatile int16_t  dtrim;        /* 906 the fill trim on the feed rate now,
                                     *     Hz (negative = playing slower) */
    volatile uint16_t dholds;       /* 908 gaps filled with a held sample */
    volatile int16_t  dtrimmin;     /* 90A the lowest trim seen, Hz */
    volatile int16_t  dtrimmax;     /* 90C the highest */
};

#define VSBJ_RMASK (VSBJ_RING - 1)

/* the ring offset is the JLM's SH_RING: a mismatch fails the build */
typedef char vsbj_ring_at_a0h[ ( offsetof( struct vsbj_share, ring ) == 0xA0 ) ? 1 : -1 ];

/* the live block (DOS memory with the JLM, ptrap.c's static without);
 * NULL only before PTRAP_Prepare */
struct vsbj_share *PTRAP_Share( void );

#endif /* _JLMSHARE_H_ */
