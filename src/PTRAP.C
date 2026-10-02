
/* port trapping
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <dos.h>    /* includes pc.h; for outp() */
//#include <fcntl.h>  /* for _dos_open() */
#include <assert.h>
#ifdef DJGPP
#include <go32.h>
#include <sys/ioctl.h>
#else
#include <conio.h>  /* contains outp()/inp() in OW */
#endif

#include "CONFIG.H"
#include "PLATFORM.H"
#include "LINEAR.H"
#include "PTRAP.H"
#include "VOPL3.H"
#include "FMVOL.H"
#include "FMSHIM.H"
#include "PTOPS.H"
#include "VDMA.H"
#include "VIRQ.H"
#include "VSB.H"
#include "HAPI.H"
#if VMPU
#include "VMPU.H"
#endif
#if DACRING
#include "JLMSHARE.H"
#endif
#if IRQONPORTACC
extern void SNDISR_IrqOnPortAcc( void );
#endif

#define DOSMEMSTART 0x60 /* offset in PSP, bits 0-3 must be zero */
#define HDPMI_MAXRANGE 8 /* hdpmi is restricted to 8 port ranges */

// next 2 defines must match EQUs in rmcode1.asm!
#ifdef CARD_TP755
/* TP755: no FM hardware anywhere, dbopl carries AdLib -- and era drivers pad
 * every OPL register write with 6-24 status/data-port reads (pure delay).
 * Each one is a full V86 monitor round-trip on the 486 = the FM "trap tax"
 * that wedged DOOM/MI2 (bench 2026-08-14). The dormant v86 stub answers
 * those reads in real mode with zero round-trips; writes stay fully trapped. */
#define HANDLE_IN_388H_DIRECTLY 1
#else
#define HANDLE_IN_388H_DIRECTLY 0
#endif
#define RMPICTRAPDYN 0 /* 1=trap PIC for v86-mode dynamically when needed */

extern struct globalvars gvars;
uint32_t _hdpmi_rmcbIO( void(*Fn)( __dpmi_regs *), __dpmi_regs *reg, __dpmi_raddr * );
void _hdpmi_CliHandler( void );
void SwitchStackIOIn(  void );
void SwitchStackIOOut( void );

static __dpmi_regs QPI_regs;   /* used for QPI access (either Qemm's or QPIEMU's) */
static __dpmi_raddr QPI_OldCallback;
static __dpmi_raddr rmcb;      /* realmode callback used to handle trapped port access in v86 mode */

static int maxports;
static int maxranges;
#if RMPICTRAPDYN
static int PICIndex;
#endif
#if HANDLE_IN_388H_DIRECTLY || !RMPICTRAPDYN
extern void * copyrmcode( void *, int );
extern uint32_t rmcodesize( int );
void * dosheap;
#endif

static uint32_t traphdl[HDPMI_MAXRANGE+1]; /* hdpmi32i trap handles */
static int portranges[HDPMI_MAXRANGE+1]; /* contains index into PortTable/PortHandler */

struct HDPMIAPI_ENTRY HDPMIAPI_Entry; /* vendor API entry (FAR32/FAR16) */

void    (*UntrappedIO_OUT_Handler)(uint16_t port, uint8_t value) = (void (*)(uint16_t, uint8_t))&outp;
uint8_t (*UntrappedIO_IN_Handler)(uint16_t port) = (uint8_t (*)(uint16_t))&inp;

static const uint8_t ChannelPageMap[] = { 0x87, 0x83, 0x81, 0x82, -1, 0x8b, 0x89, 0x8a };

#define OPL3_PDT  0
#define MPIC_PDT  1
#define SPIC_PDT  2
#define DMA_PDT   3
#define DMAPG_PDT 4
#if SB16
#define HDMA_PDT  5
#define SB_PDT    6
#define MPU_PDT   7
#else
#define SB_PDT    5
#define MPU_PDT   6
#endif

static uint16_t PortTable[] = {
	0x388, 0x389, 0x38A, 0x38B | 0x8000,
	0x20, 0x21 | 0x8000,
	/* 0xA0 is trapped ONLY so multi-byte accesses that START there reach our
	 * decomposer: the CPU faults a word/dword access if ANY of its bytes has
	 * an IOPM bit set, but both Jemm (v5.84+ raw-splits unmatched accesses to
	 * real hardware) and hdpmi report just the STARTING port. A guest 16-bit
	 * "OUT 0A0h,AX" (OCW+IMR in one instruction) therefore put AH on the REAL
	 * slave IMR behind VPIC's back -- masking IRQ10, the TP755 engine clock
	 * (bench 2026-08-14, the MI1 resurrection). Byte content on 0xA0 is never
	 * virtualized: VPIC_PassAcc, and the v86 stub short-circuits it. */
	0xA0, 0xA1 | 0x8000,
	0x02, 0x03,                   /* ch 1; will be modified if LDMA != 1 */
	0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F | 0x8000,
#if SB16
	0x83, 0x8B | 0x8000,          /* ch 1 & ch 5: page regs */
#else
	0x83 | 0x8000,
#endif
#if SB16
	0xC4, 0xC6,                   /* ch 5; will be modified if HDMA != 5 */
	0xD0, 0xD2, 0xD4, 0xD6, 0xD8, 0xDA, 0xDC, 0xDE | 0x8000,
#endif
	0x220, 0x221, 0x222, 0x223, /* FM */
	0x224, 0x225, 0x226,
	0x228, 0x229, /* FM */
	0x22A, 0x22C,
	0x22E, 0x22F | 0x8000,
#if VMPU
	0x330, 0x331 | 0x8000,
#endif
    0xffff
};


/* PortHandler array must match port array */
static PORT_TRAP_HANDLER PortHandler[] = {
	VOPL3_388, VOPL3_389, VOPL3_38A, VOPL3_38B,
	VPIC_Acc, VPIC_Acc,    /* 0x20, 0x21 */
	VPIC_PassAcc,          /* 0xA0: passthrough (trapped for the decomposer) */
	VPIC_Acc,              /* 0xA1 */
	VDMA_Acc, VDMA_Acc,    /* base+cnt for ch 1; will be modified if LDMA != 1 */
	VDMA_Acc, VDMA_Acc, VDMA_Acc, VDMA_Acc, VDMA_Acc, VDMA_Acc, VDMA_Acc, VDMA_Acc, /* 0x08-0x0F */
	VDMA_Acc,              /* page reg for ch 1; will be modified if LDMA != 1 */
#if SB16
	VDMA_Acc,              /* page reg for ch 5; will be modified if HDMA != 5 */
#endif
#if SB16
	VDMA_Acc, VDMA_Acc,    /* base+cnt for ch 5; will be modified if HDMA != 5 */
	VDMA_Acc, VDMA_Acc, VDMA_Acc, VDMA_Acc, VDMA_Acc, VDMA_Acc, VDMA_Acc, VDMA_Acc, /* 0xD0-0xDE */
#endif
	VOPL3_388, VOPL3_389, VOPL3_38A, VOPL3_38B, /* 0x220-0x223 */
	VSB_MixerAddr, VSB_MixerData,               /* 0x224-0x225 */
	VSB_DSP_Reset,                              /* 0x226 */
	VOPL3_388, VOPL3_389,                       /* 0x228, 0x229 */
	VSB_DSP_Acc0A, VSB_DSP_Acc0C,               /* 0x22a, 0x22c */
	VSB_DSP_Acc0E, VSB_DSP_Acc0F,               /* 0x22e, 0x22f */
#if VMPU
	VMPU_Acc, VMPU_Acc,
#endif
};

/* state of trapped ports */
static uint16_t PortState[countof(PortHandler)];

#if HANDLE_IN_388H_DIRECTLY
static void SyncOplStatusCache( void );
static uint8_t OplIndexShadow( void );
static int IsOplHandler( PORT_TRAP_HANDLER h );
void PTRAP_DrainOplRing( void );
/* TP755's rmcode1 stub outgrew the 160 bytes of PSP after 0x60 (the write
 * ring pushed it to ~180), so the whole real-mode home moves into DOS-block
 * slack that sc_tp755 allocates: [0..191 stub][192..223 OPL ring][224..255
 * SB-ISR stub]. 0 = not registered (fall back to the PSP home). */
static uint32_t RMStubLinear;

/* Ring size. MUST match OPLRING_ENTRIES in rmcode1.asm, and sc_tp755's
 * TP_RMHOME_PARA must equal PTRAP_RMHOME_PARA below. */
#define OPLRING_ENTRIES 1024
#define OPLRING_MASK    (OPLRING_ENTRIES - 1)
/* where the ring starts inside the real-mode home. The stub blob (vars +
 * code) has to fit below this. It was 186 bytes when the ring went in and
 * the offset was 256; the SB-base fast paths took it to 279, and from then
 * on Prepare_RM_PortTrap left the ring disarmed (rseg 0 = the stub's pre-ring
 * synchronous path, "ring DISABLED" at load). 512 leaves it room again. */
#define OPLRING_OFF     512
/* the whole home: stub + ring + 32 spare = sc_tp755's TP_RMHOME_PARA */
#define PTRAP_RMHOME_PARA ((OPLRING_OFF + OPLRING_ENTRIES * 2 + 32) / 16)
#else
/* /LPT: the stub variant that drives the OPL3LPT from V86 (rmcode1.asm
 * assembled with LPTSTUB, blob RMCODE_LPT) does not fit the PSP either, so
 * it gets a DOS block of its own and RMStubLinear points there; RMLptStub
 * says that variant is the one installed. Without /LPT both stay 0 and the
 * ordinary stub sits at PSP:60h as before. DJGPP build only. */
static uint32_t RMStubLinear;
static int RMLptStub;
#define RMCODE_LPT 2
/* LPTSTUB's variables, appended to struct rmcode1 ahead of the code */
struct rmlpt {
    uint16_t wLpt;      /* OPL3LPT data port; 0 = not armed */
    uint8_t  bDly;      /* control-port reads after each strobe */
    uint8_t  bCli;      /* 1 = interrupts off around each LPT write */
};
#endif
/* PSP:60h up to the end of the PSP, which is all that stays in conventional
 * memory once VSBPCM is resident (INT 21h/31h with DX=10h). The rmcode1 stub
 * and the IRQ7 stub _SB_InstallISR copies after it (rmcode2) share it: 142 +
 * 17 of the 160 bytes in the plain build, 279 + 17 with the OPL fast path. */
#define PSP_STUB_ROOM   (0x100 - DOSMEMSTART)

#ifdef VSBJ_FMRING
/* An FM access that reaches us (a protected-mode guest's, or one that the
 * JLM does not own) is newer than whatever V86 FM writes wait in the JLM's
 * FM ring, so they go into vopl3 first. */
static int IsVopl3Handler( PORT_TRAP_HANDLER h )
{
    return h == VOPL3_388 || h == VOPL3_389 || h == VOPL3_38A || h == VOPL3_38B;
}
#endif

/* One byte of a decomposed multi-byte access: route it through the port's
 * registered handler if the port is trapped, else to real hardware. */
static uint8_t PTRAP_TrapByte( uint16_t port, uint8_t val, uint16_t flags )
{
    int i;
    for ( i = 0; i < maxports; i++ )
        if ( PortTable[i] == port ) {
#ifdef VSBJ_FMRING
            if ( IsVopl3Handler( PortHandler[i] ) )
                PTRAP_DrainJlmFm();
#endif
#if HANDLE_IN_388H_DIRECTLY
            if ( IsOplHandler( PortHandler[i] ) ) {
                PTRAP_DrainOplRing();       /* ring entries are older: first */
                val = PortHandler[i]( port, val, flags );
                SyncOplStatusCache();
                return val;
            }
#endif
            return PortHandler[i]( port, val, flags );
        }
    if ( flags & TRAPF_OUT ) {
        UntrappedIO_OUT( port, val );
        return val;
    }
    return UntrappedIO_IN( port );
}

/* real-mode port trap handler;
 * called by SwitchStackIOrmcb().
 */

static void RM_TrapHandler( __dpmi_regs * regs)
///////////////////////////////////////////////
{
    uint16_t port = regs->x.dx;
    int i;

    /* regs.x.cl:
     * bit[2]: 1=out, 0=in;
     * bits 3,4 word/dword access, not used here
     * regs.x.ch:
     * bit[1]: IF
     */
    /* Jemm's v86 monitor size bits: CL.3=word, CL.4=dword (QPIEMU passes CL
     * through). Decompose byte-wise, low byte first (Jemm's own Simulate_IO
     * order), so e.g. the IMR half of a word "OUT 0A0h,AX" goes through
     * VPIC_Write's engine-IRQ filter and a word OUT to 0x388 delivers BOTH
     * the index and the data byte (the old code fed AL to the start port's
     * handler and silently dropped AH). */
    if ( regs->x.cx & 0x18 ) {
        uint16_t fl = regs->x.cx & ~0x18;
        int n = ( regs->x.cx & 0x10 ) ? 4 : 2;
        if ( fl & TRAPF_OUT ) {
            for ( i = 0; i < n; i++ )
                PTRAP_TrapByte( port + i, (uint8_t)(regs->d.eax >> (8*i)), fl );
        } else {
            uint32_t v = 0;
            for ( i = 0; i < n; i++ )
                v |= (uint32_t)PTRAP_TrapByte( port + i, 0, fl ) << (8*i);
            if ( n == 2 )
                regs->x.ax = (uint16_t)v;   /* client EAX.hi round-trips */
            else
                regs->d.eax = v;
        }
        regs->x.flags &= ~CPU_CFLAG;
        return;
    }
    for ( i = 0; i < maxports; i++ ) {
        if( PortTable[i] == port ) {
#if HANDLE_IN_388H_DIRECTLY
            if ( IsOplHandler( PortHandler[i] ) ) {
                PTRAP_DrainOplRing();       /* buffered writes are older */
                /* The stub keeps non-timer 388h index writes to itself (the
                 * low byte of data) and buffers their data writes. A 389h
                 * write that still reaches here -- ring disarmed or full --
                 * belongs to that index, not to whatever vopl3 saw last:
                 * without this, every note went to the last timer register
                 * (Monkey Island silent, T2130CT, 2026-09-30). */
                if ( port == 0x389 && ( regs->x.cx & TRAPF_OUT ) )
                    VOPL3_388( 0x388, OplIndexShadow(), TRAPF_OUT );
                regs->h.al = PortHandler[i]( port, regs->h.al, regs->x.cx );
                SyncOplStatusCache();
                regs->x.flags &= ~CPU_CFLAG;
                return;
            }
#endif
            regs->h.al = PortHandler[i]( port, regs->h.al, regs->x.cx );
            regs->x.flags &= ~CPU_CFLAG; /* clear carry flag, indicates that access was handled */
#if IRQONPORTACC
            /* give the sound HW interrupt a chance to be triggered if:
             * + interrupts disabled and OUT instr is emulated
             * + port access isn't ISA DMA or PIC
             * + no DSP DMA op is running
             */
            if ( ((regs->x.cx & TRAPF_IF) == TRAPF_OUT) && port >= 0x100 && !VSB_Running() )
                SNDISR_IrqOnPortAcc();
#endif
            return;
        }
    }

    /* A port we do not own. That IS reachable, despite what this comment used
     * to say -- it is only unreachable while vsbhda is the sole QPI client.
     *
     * QPI has ONE global trap-handler slot (fn 1A07) but traps ports
     * individually (fn 1A09), so the LAST client to register owns dispatch
     * for EVERY trapped port -- including ports another client trapped and is
     * still waiting on. VSBPCM loads last in the MPUSHIM stack, so MPUSHIM's
     * MPU ports 330/331 arrive here, and returning carry-set dropped every
     * MIDI byte before it could reach MPUSHIM's real-mode blob. Measured:
     * real-mode MIDI (DOSMid) dead while protected-mode MIDI (DOOM) worked,
     * because the PM side goes through HDPMI32i, which registers per client
     * and has no shared slot.
     *
     * Chain to whoever held the slot before us instead. Only genuinely
     * unowned ports get here -- everything in PortTable returned above -- so
     * the fast path is untouched.
     *
     * TWO LIMITS, both deliberate and both worth knowing before relying on
     * this as a general chain:
     *  - It is BYTE-ONLY. A word/dword trap is decomposed at the top of this
     *    function and its unowned bytes go to UntrappedIO_*, i.e. to real
     *    hardware, not here. MPU MIDI is byte-wise so it does not care; a
     *    client trapping a word-accessed port would.
     *  - Only AL is taken back, so a chained word/dword IN could not return
     *    its value anyway. Consistent with the first limit.
     * The discarded alternative was to pass unowned ports straight to real
     * hardware (UntrappedIO_*), which is wrong here: there is no MPU behind
     * 330h, only MPUSHIM's emulation.
     *
     * COST: one PM->RM simulate per unowned trapped access. At MIDI byte
     * rates that is ~1 per 320us worst case, affordable on the 486SX floor.
     * Note this nests a simulate INSIDE a real-mode callback, on the
     * switched ISR stack -- HDPMI and QPIEMU are expected to tolerate it,
     * but that is the part of this change only a bench can confirm. */

    dbgprintf(("RM_TrapHandler: unowned port=%x val=%x out=%x -> chain (OldCB=%x:%x)\n", regs->x.dx, regs->h.al, regs->h.cl, QPI_OldCallback.v86.segment, QPI_OldCallback.v86.offset ));
    if ( QPI_OldCallback.v86.segment ) {
        __dpmi_regs r = *regs;
        r.x.ip = QPI_OldCallback.v86.offset;
        r.x.cs = QPI_OldCallback.v86.segment;
        __dpmi_simulate_real_mode_procedure_retf(&r);
        regs->x.flags |= r.x.flags & CPU_CFLAG;
        regs->h.al = r.h.al;
    } else {
        regs->x.flags |= CPU_CFLAG;   /* nobody behind us: "not handled" */
    }
    return;
}

static int FmShimOn;   /* set in PTRAP_Prepare: this card has no real FM */
static uint8_t FM_Alias( uint16_t port, uint8_t val, uint16_t flags );

#ifndef NOTFLAT
/* SBEFMPATCH: rewrite a guest's delay-read IN AL,DX (EC) to NOP (90) in
 * place, so it stops trapping (see FMSHIM.C for the rule). cs:eip is the
 * trapped instruction; the byte is checked before it is written, and the
 * count of patched instructions goes to IAC 0x4F1 -- the render-guard skip
 * counter, which should stay 0 and is lent while the knob is set.
 * 32-bit build only: stackio.asm passes cs:eip there and not in VSBPCM16,
 * where the same addition hung the load (see stackio.asm). */
static unsigned FmPatchCount;

static void PatchDelayIn( uint32_t cs, uint32_t eip )
{
    unsigned long base;
    uint8_t *op;
    if ( __dpmi_get_segment_base_address( (int)cs, &base ) != 0 )
        return;
    op = NearPtr( (uint32_t)base + eip );
    if ( *op == 0xEC ) {
        *op = 0x90;
        if ( FmPatchCount < 255 )
            FmPatchCount++;
        *(uint8_t *)NearPtr( 0x4F1 ) = (uint8_t)FmPatchCount;
    }
}
#endif

/* protected-mode port trap handler;
 * called by SwitchStackIO();
 */

/* hdpmi's errcode size bits differ from Jemm's: 10h=word, 20h=dword (see
 * stackio.asm, which already stores AX/EAX back for them). Return widened to
 * uint32_t so a decomposed word/dword IN reaches the client; the asm side
 * reads AL for byte accesses either way, so the ABI is unchanged.
 * cs:eip (32-bit build) is the guest's trapped instruction, for PatchDelayIn. */
#ifdef NOTFLAT
uint32_t PTRAP_PM_TrapHandler( uint16_t port, uint16_t flags, uint32_t value )
#else
uint32_t PTRAP_PM_TrapHandler( uint16_t port, uint16_t flags, uint32_t value,
                               uint32_t cs, uint32_t eip )
#endif
//////////////////////////////////////////////////////////////////////////////
{
    int i;
    if ( flags & 0x30 ) {
        uint16_t fl = flags & ~0x30;
        int n = ( flags & 0x20 ) ? 4 : 2;
        uint32_t v = 0;
        for ( i = 0; i < n; i++ ) {
            if ( fl & TRAPF_OUT )
                PTRAP_TrapByte( port + i, (uint8_t)(value >> (8*i)), fl );
            else
                v |= (uint32_t)PTRAP_TrapByte( port + i, 0, fl ) << (8*i);
        }
        return v;
    }
    for( i = 0; i < maxports; i++ )
        if( PortTable[i] == port) {
#ifdef VSBJ_FMRING
            if ( IsVopl3Handler( PortHandler[i] ) )
                PTRAP_DrainJlmFm();
#endif
#if HANDLE_IN_388H_DIRECTLY
            /* drain-first + keep the v86 stub's 0x388 status cache fresh
             * across worlds (a PM game's OPL writes must be visible to a
             * later v86 read, and must not overtake buffered v86 writes) */
            if ( IsOplHandler( PortHandler[i] ) ) {
                PTRAP_DrainOplRing();
                value = PortHandler[i](port, (uint8_t)value, flags );
                SyncOplStatusCache();
                return value;
            }
#endif
#ifdef NOTFLAT
            return PortHandler[i](port, (uint8_t)value, flags );
#else
            value = PortHandler[i](port, (uint8_t)value, flags );
            /* a plain byte IN the FM shim answered (388h-38Bh, or an SB
             * alias while the shim owns them) that it calls padding */
            if ( !( flags & ( TRAPF_OUT | 0x08 ) )
                 && ( PortHandler[i] == FMSHIM_Acc
                      || ( PortHandler[i] == FM_Alias && FmShimOn ) )
                 && FMSHIM_IsDelayRead() )
                PatchDelayIn( cs, eip );
            return value;
#endif
        }

    /* ports that are trapped, but not handled; this may happen, since
     * hdpmi32i's support for port trapping is limited to 8 ranges.
     */
    if ( flags & TRAPF_OUT) {
        UntrappedIO_OUT( port, (uint8_t)value );
        return value;
    } else
        return UntrappedIO_IN( port );
}


uint16_t PTRAP_GetQEMMVersion(void)
///////////////////////////////////
{
    __dpmi_regs r;
    r.x.ss = r.x.sp = 0;
    r.x.flags = 0x202;
#if 0 /* OW doesn't know ioctl() */
    uint32_t entryfar = 0;
    int fd = 0;
    unsigned int result = _dos_open("QEMM386$", O_RDONLY, &fd);
    //ioctl - read from character device control channel
    if (result == 0) { //QEMM detected?
        int count = ioctl(fd, DOS_RCVDATA, 4, &entryfar);
        _dos_close(fd);
        if(count == 4) {
            QPI_regs.x.ip = entryfar & 0xFFFF;
            QPI_regs.x.cs = entryfar >> 16;
        }
    }
#else
    if ( ReadLinearD( 0x67*4 ) ) { /* int 67h initialized? */
        r.x.cx = 0x5145; /* "QE" */
        r.x.dx = 0x4d4d; /* "MM" */
        r.x.ax = 0x3f00;
        __dpmi_simulate_real_mode_interrupt(0x67, &r);
        if ( r.h.ah == 0 && r.x.es ) {
            QPI_regs.x.ip = r.x.di;
            QPI_regs.x.cs = r.x.es;
        }
    }
#endif
    /* if Qemm hasn't been found, try Jemm's QPIEMU ... */
    if ( QPI_regs.x.cs == 0 ) {
        /* QPIEMU installation check;
         * getting the entry point of QPIEMU is non-trivial in protected-mode, since
         * the int 2Fh must be executed as interrupt ( not just "simulated" ). Here
         * a small ( 3 bytes ) helper proc is constructed on the fly, at PSP:005Ch:
         * a INT 2Fh, followed by an RETF.
         */
        uint32_t *dosmem = NearPtr(_my_psp() + 0x5C);
        *dosmem = 0xCB2FCD;  /* INT 2Fh & IRET */
        r.x.ax = 0x1684;
        r.x.bx = 0x4354;
        r.x.cs = _my_psp() >> 4;
        r.x.ip = 0x5C;
        if( __dpmi_simulate_real_mode_procedure_retf(&r) != 0 || r.h.al )
            return 0;
        QPI_regs.x.ip = r.x.di;
        QPI_regs.x.cs = r.x.es;
    }
    QPI_regs.h.ah = 0x03; /* get version */
    if( __dpmi_simulate_real_mode_procedure_retf(&QPI_regs) == 0 ) {
        return QPI_regs.x.ax;
    }
    return 0;
}

/* v1.6: extracted from PTRAP_Prepare_RM_PortTrap() because that function may be optional.
 * Variables maxports, maxranges, portranges[] and PortTable[] are initialized.
 */

void PTRAP_InitPortMax( void )
//////////////////////////////
{
    int i, j;
    /* setup port ranges */
    for ( i = 0, j = 1, portranges[0] = 0; PortTable[i] != 0xffff; i++ ) {
        if ( PortTable[i] & 0x8000 ) {
            portranges[j] = i+1;
            PortTable[i] &= 0x7fff;
            j++;
        }
    }
    maxports = i;
    maxranges = j - 1;
}

/*
 * Prepare real-mode port trapping.
 * This isn't called if /RM0 has been set or QPI API hasn't been found!
 */

#if HANDLE_IN_388H_DIRECTLY || !RMPICTRAPDYN

struct rmcode1 {   /* structure must match definitions in rmcode1.asm! */
    uint32_t rmcb; /* realmode callback */
    uint16_t data; /* LOW byte: current OPL index shadow; HIGH: 0x388 status cache */
    uint16_t wPort; /* used for PIC port trapping; contains either 0x0020 or 0xffff */
    uint32_t qpi;  /* QPI entry */
    uint16_t wFmSB;  /* emulated SB base whose FM aliases the stub forwards to
                      * the real OPL3 at 0x388 itself; 0xFFFF = not armed */
    uint16_t wDspWS; /* emulated DSP write-status port (base+0xC) the stub
                      * answers itself; 0xFFFF = not armed */
    uint8_t  bDspWS; /* DSP_Read0C's busy counter, shared with vsb.c */
    uint8_t  bDspPad;
#if HANDLE_IN_388H_DIRECTLY
    uint16_t rseg;  /* OPL write ring: real-mode segment (paragraph-aligned) */

    uint16_t rhead; /*   producer slot (v86 stub) */
    uint16_t rtail; /*   consumer slot (PTRAP_DrainOplRing) */
    uint8_t rfull;  /*   ring-full events counted by the stub (wraps) */
    uint8_t rpad;   /*   keeps codev86 even -- see rmcode1.asm */
#endif
    uint8_t codev86[]; /* v86 code */
};

/* The live stub variable block, once PTRAP_Prepare_RM_PortTrap has copied
 * the template into DOS memory; NULL without real-mode support. The two
 * stub-answered SB ports are armed through this (PTRAP_Prepare), and the
 * DSP write-status counter is shared with vsb.c through it. */
static int RMStubReady;
static struct rmcode1 *RMVars( void )
{
    if ( !RMStubReady )
        return NULL;
    return RMStubLinear ? (struct rmcode1 *)NearPtr(RMStubLinear)
                        : (struct rmcode1 *)NearPtr(_my_psp() + DOSMEMSTART);
}

uint8_t *PTRAP_DspStatusCell( void )
{
    struct rmcode1 *dm = RMVars();
    return dm ? &dm->bDspWS : NULL;
}

#if DACRING
/* ---- VSBPCMJ.DLL, the ring-0 companion (jlm/VSBPCMJ.ASM) ----
 * QPIEMU runs our V86 stub as a nested execution for every trapped access,
 * and that cost is the floor for the two densest kinds of V86 traffic: FM
 * register writes with their delay reads (Theme Hospital's Miles driver runs
 * in V86), and direct DAC, two trapped writes per sample (Another World).
 * When the JLM is loaded, it takes those ports at ring 0 and they stay off
 * the QPI traps: the FM ports while the timer shim owns them (no chip,
 * /FMSHIM, /LPT) or the software OPL3 does (builds without NOFM: the JLM
 * then puts the writes in its FM ring and PTRAP_DrainJlmFm replays them
 * into vopl3), and the DSP write port always. Protected-mode guests still
 * come through HDPMI to the handlers here. SBENOJLM=1 (bench knob) leaves the
 * JLM unarmed, so everything takes QPI as before.
 *
 * The JLM and we share one block in DOS memory (jlmshare.h): its config,
 * vsb.c's "next DSP write is a command" flag, the direct-DAC ring that
 * sndisr.c drains, and the JLM's counters. Without the JLM the same struct
 * is JlmLocal and carries only the ring. */
static struct vsbj_share JlmLocal;
static struct vsbj_share *JlmShare = &JlmLocal;
static __dpmi_raddr JlmEntry;   /* the JLM's V86 API; segment 0 = not loaded */
static uint16_t JlmSel;         /* DPMI selector of the shared DOS block */
static uint8_t JlmArmed;        /* VSBJ_F_* the JLM took */
static uint16_t JlmSB;          /* emulated SB base it serves */
static uint32_t QPI_StubCB;     /* the seg:off we gave QPI (fn 1A07h) */
static int PrepSB;              /* PTRAP_Prepare's sbaddr */
static int PrepLptDly = 6;      /* ... and the /LPT strobe delay it chose */
static int PrepIrq;             /* ... and the card's IRQ (8 = the RTC pumps) */
static int PrepOpl;             /* ... and whether the software OPL3 is on */

struct vsbj_share *PTRAP_Share( void )
{
    return JlmShare;
}

/* INT 2Fh AX=1684h BX=device id must run as an interrupt in V86, so call a
 * three-byte INT 2Fh + RETF at PSP:5Ch, as the QPIEMU check does. */
static void JlmDetect( void )
{
    __dpmi_regs r;
    uint32_t *dosmem = NearPtr(_my_psp() + 0x5C);
    memset( &r, 0, sizeof(r) );
    *dosmem = 0xCB2FCD;  /* INT 2Fh & RETF */
    r.x.ax = 0x1684;
    r.x.bx = VSBJ_DEVID;
    r.x.cs = _my_psp() >> 4;
    r.x.ip = 0x5C;
    r.x.flags = 0x202;
    if ( __dpmi_simulate_real_mode_procedure_retf( &r ) == 0 && r.h.al == 0
         && r.x.es ) {
        JlmEntry.v86.segment = r.x.es;
        JlmEntry.v86.offset  = r.x.di;
    }
}

/* one call into the JLM's V86 API: 0 on success, else its error (AX) */
static int JlmCall( __dpmi_regs *r )
{
    r->x.cs = JlmEntry.v86.segment;
    r->x.ip = JlmEntry.v86.offset;
    r->x.ss = r->x.sp = 0;
    r->x.flags = 0x202;
    if ( __dpmi_simulate_real_mode_procedure_retf( r ) != 0 )
        return -1;
    return ( r->x.flags & CPU_CFLAG ) ? r->x.ax : 0;
}

/* Does the JLM own this port (so QPI must not trap it)? */
static int JlmOwns( uint16_t port )
{
    if ( ( JlmArmed & VSBJ_F_FM )
         && ( ( port >= 0x388 && port <= 0x38B )
              || ( port >= JlmSB && port <= JlmSB + 3 )
              || port == JlmSB + 8 || port == JlmSB + 9 ) )
        return 1;
    if ( ( JlmArmed & VSBJ_F_DAC ) && port == JlmSB + SB_PORT_DSP_WRITE_WS )
        return 1;
    if ( ( JlmArmed & VSBJ_F_PIC ) && port == 0x20 )
        return 1;
    return 0;
}

static void JlmFreeBlock( void )
{
    if ( JlmSel ) {
        __asm__ __volatile__("int $0x31"
                             : : "a"(0x0101), "d"((uint32_t)JlmSel)
                             : "cc", "memory");
        JlmSel = 0;
    }
}

/* Arm the JLM, if it is loaded: runs from PTRAP_Install_RM_PortTraps, after
 * PTRAP_Prepare and VSB_Init, before any QPI trap goes in. Any failure
 * leaves every port on QPI, as without the JLM. */
static void JlmArm( void )
{
    __dpmi_regs r;
    struct vsbj_share *s;
    struct rmcode1 *dm = RMVars();
    uint32_t eax = 0x0100, edx = 0, lin, stub;
    uint8_t want = VSBJ_F_DAC, err;
    const char *e;
    int rc;

    if ( !JlmEntry.v86.segment || !dm || !QPI_StubCB || !PrepSB )
        return;
    if ( ( e = getenv("SBENOJLM") ) && *e == '1' ) {
        printf("VSBPCMJ: loaded, left unarmed (SBENOJLM=1)\n");
        return;
    }
    /* A JLM that reports itself armed was armed by a VSBPCM that is gone
     * (IsInstalled found none running), so its block is stale: disarm. */
    memset( &r, 0, sizeof(r) );
    if ( JlmCall( &r ) == 0 && r.x.bx ) {
        memset( &r, 0, sizeof(r) );
        r.x.ax = 2;
        JlmCall( &r );
    }
    if ( FmShimOn ) {
        want |= VSBJ_F_FM;
        /* the delay-read patch is the point of taking FM to ring 0, so it
         * is on here unless SBEFMPATCH=0 (protected mode: =1 turns it on) */
        if ( !( ( e = getenv("SBEFMPATCH") ) && *e == '0' ) )
            want |= VSBJ_F_PATCH;
    }
#ifdef VSBJ_FMRING
    else if ( PrepOpl ) {
        /* The software OPL3: the JLM takes the V86 FM ports as it does for
         * the shim and answers status from the same timer model, and its
         * data writes go into the FM ring for PTRAP_DrainJlmFm, one fault a
         * write where the stub took a QPI round trip (and, for array 1 and
         * the SB aliases, an RMCB as well). */
        want |= VSBJ_F_FM | VSBJ_F_OPL;
        if ( !( ( e = getenv("SBEFMPATCH") ) && *e == '0' ) )
            want |= VSBJ_F_PATCH;
    }
#endif
    /* the same for a direct-DAC guest's write-status busy-waits, two of
     * its four traps per sample, then its 10h-and-sample pairs, and for
     * Another World's exact sequence a call into the JLM's V86 copy of the
     * ring producer, no trap at all; SBEDSPPATCH=0 keeps all three */
    if ( !( ( e = getenv("SBEDSPPATCH") ) && *e == '0' ) )
        want |= VSBJ_F_POLL | VSBJ_F_PAIR | VSBJ_F_V86;
    /* Port 20h: a guest's EOIs, which a timer-driven direct-DAC game sends
     * at its sample rate, went through QPIEMU and back into QPI for the real
     * OUT (~20 us each on a DX4/75). The JLM does them at ring 0 and hands
     * them back to us only while an SB IRQ is virtualized; SBEJLMPIC=0
     * leaves port 20h on QPI. */
    if ( !( ( e = getenv("SBEJLMPIC") ) && *e == '0' ) )
        want |= VSBJ_F_PIC;
    __asm__ __volatile__("int $0x31; setc %0"
                         : "=q"(err), "+a"(eax), "=d"(edx)
                         : "b"(( sizeof(struct vsbj_share) + 15 ) / 16)
                         : "cc", "memory");
    if ( err ) {
        printf("VSBPCMJ: no DOS memory for its block, not used\n");
        return;
    }
    JlmSel = (uint16_t)edx;
    lin = ( eax & 0xFFFFUL ) << 4;
    s = NearPtr( lin );
    memset( s, 0, sizeof(*s) );
    s->sig = VSBJ_SIG;
    s->ver = VSBJ_VER;
    s->ringsize = VSBJ_RING;
    s->lpt = ( ( want & ( VSBJ_F_FM | VSBJ_F_OPL ) ) == VSBJ_F_FM ) ? (uint16_t)FOpts.lpt : 0;
    s->lptdly = (uint8_t)PrepLptDly;
    s->flags = want;
    s->sbbase = (uint16_t)PrepSB;
    s->qpicb = QPI_StubCB;
#ifdef VSBJ_FMRING
    s->osize = ( want & VSBJ_F_OPL ) ? VSBJ_FMRING : 0;
#endif
    stub = RMStubLinear ? RMStubLinear : _my_psp() + DOSMEMSTART;
    s->wscell = stub + offsetof(struct rmcode1, bDspWS);
    s->piccell = stub + offsetof(struct rmcode1, wPort);
    s->dspidle = JlmLocal.dspidle;     /* vsb.c has published here so far */
    /* An RTC pump (card IRQ 8) can be switched off under a game; the JLM's
     * pulse puts it back from the guest's own FM and DSP accesses. */
    s->guard = ( PrepIrq == 8 );

    memset( &r, 0, sizeof(r) );
    r.x.ax = 1;
    r.d.edx = lin;
    if ( ( rc = JlmCall( &r ) ) != 0 ) {
        if ( rc == 3 )
            printf("VSBPCMJ: port %Xh is trapped already, not used\n", r.x.dx );
        else
            printf("VSBPCMJ: refused the shared block (error %d), not used\n", rc );
        JlmFreeBlock();
        return;
    }
    JlmShare = s;
    JlmArmed = want;
    JlmSB = (uint16_t)PrepSB;
    PTOPS_PumpGuard = s->guard;
    /* worst case 79 columns: FM 388h+240h (dbopl, no patching), DSP 24Ch */
    if ( want & VSBJ_F_FM )
        printf("VSBPCMJ: ring 0 serves FM 388h+%Xh (%s%s), DSP %Xh; block %04lXh\n",
               PrepSB, ( want & VSBJ_F_OPL ) ? "dbopl, " : FOpts.lpt ? "LPT, " : "",
               ( want & VSBJ_F_PATCH ) ? "patching" : "no patching",
               PrepSB + SB_PORT_DSP_WRITE_WS, (unsigned long)( lin >> 4 ) );
    else
        printf("VSBPCMJ: ring 0 serves direct DAC at DSP %Xh; block %04lXh\n",
               PrepSB + SB_PORT_DSP_WRITE_WS, (unsigned long)( lin >> 4 ) );
}

/* Unload: stop the pump guard before the card closes its RTC pump, so
 * nothing switches the periodic interrupt back on behind it. */
void PTRAP_JlmQuiesce( void )
{
    JlmShare->guard = 0;
}

static void JlmDisarm( void )
{
    __dpmi_regs r;
    if ( !JlmArmed )
        return;
    memset( &r, 0, sizeof(r) );
    r.x.ax = 2;
    JlmCall( &r );
    JlmArmed = 0;
    JlmShare = &JlmLocal;
    JlmFreeBlock();
}

#ifdef VSBJ_FMRING
/* The JLM's FM ring: a V86 guest's FM register writes in its order, as
 * value | index << 8 | register array << 16, replayed into vopl3 before
 * each render (sndisr.c) and before any FM access that still comes here.
 * The sound interrupt and a protected-mode trap can both drain, so entries
 * are taken with interrupts off, 32 at a time: one long cli would hold off
 * COMRADE's UART past its FIFO (PTRAP_DrainOplRing, the same reasoning). */
void PTRAP_DrainJlmFm( void )
{
    struct vsbj_share *s = JlmShare;
    const uint16_t mask = VSBJ_FMRING - 1;
    uint16_t head, tail, occ;
    uint32_t f;
    int n;

    if ( !( JlmArmed & VSBJ_F_OPL ) || s->ohead == s->otail )
        return;
    do {
        __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(f) :: "memory");
        head = s->ohead;
        tail = s->otail;
        occ = (uint16_t)( ( head - tail ) & mask );
        if ( occ > s->ohigh )
            s->ohigh = occ;
        for ( n = 32; tail != head && n; n-- ) {
            uint32_t e = s->oring[tail];
            if ( e & 0x10000UL ) {
                VOPL3_38A( 0x38A, (uint8_t)( e >> 8 ), TRAPF_OUT );
                VOPL3_38B( 0x38B, (uint8_t)e, TRAPF_OUT );
            } else {
                VOPL3_388( 0x388, (uint8_t)( e >> 8 ), TRAPF_OUT );
                VOPL3_389( 0x389, (uint8_t)e, TRAPF_OUT );
            }
            tail = (uint16_t)( ( tail + 1 ) & mask );
        }
        s->otail = tail;
        if ( f & 0x200 )
            __asm__ __volatile__("sti" ::: "memory");
    } while ( tail != head );
}
#endif
#endif /* DACRING */


#if HANDLE_IN_388H_DIRECTLY
/* The v86 stub answers byte reads of 0x388 from rmcode1.data's high byte
 * (vars._0005) with ZERO logic of its own; this refresh -- run after every
 * OPL trap the C side handles -- makes vopl3.cpp the single source of truth
 * for timer semantics (mask bits, RST, both-timers OR). The stub can never
 * diverge again the way the old in-stub start-bit test did. */
static struct rmcode1 *RMStub( void )
{
    return RMStubLinear ? (struct rmcode1 *)NearPtr(RMStubLinear)
                        : (struct rmcode1 *)NearPtr(_my_psp() + DOSMEMSTART);
}

static void SyncOplStatusCache( void )
{
    struct rmcode1 *dm = RMStub();
    dm->data = (dm->data & 0xFF) | ((uint16_t)VOPL3_388( 0x388, 0, 0 ) << 8);
}

/* the 388h index the stub last saw (vars._0004, rmcode1.data's low byte) */
static uint8_t OplIndexShadow( void )
{
    return (uint8_t)RMStub()->data;
}

/* ---- the OPL write ring (see rmcode1.asm for the producer side) ----
 * The v86 stub buffers non-timer OPL register writes as (index,value)
 * entries so the guest's music handler stops paying an RMCB mode switch
 * per write -- the DX4/75 FM killer (IRQ0 monopoly; saturation meter 4FD).
 * This side replays them into vopl3 in exact write order: once per SNDISR
 * tick, and always BEFORE any synchronous OPL access is applied. */

static uint8_t *OplRing;    /* near ptr; 16 entries x 2 bytes (lo=val, hi=idx) */

static uint8_t Drain_Cli(void)
{
    uint32_t f;
    __asm__ __volatile__("pushfl; popl %0; cli":"=r"(f)::"memory");
    return (uint8_t)((f >> 9) & 1);
}
static void Drain_Sti(uint8_t on)
{
    if (on) __asm__ __volatile__("sti":::"memory");
}

static uint32_t OplRingLinear;   /* kept: copyrmcode() re-zeroes the stub struct */

/* Register the real-mode home: PTRAP_RMHOME_PARA paragraphs of DOS memory
 * (sc_tp755 allocates them; PTRAP_Prepare_RM_PortTrap does for any other
 * backend). Layout owned here:
 *   [0..511 stub][512..2559 ring, OPLRING_ENTRIES entries][2560..2591 spare]
 * Must run BEFORE PTRAP_Prepare_RM_PortTrap (card detect precedes traps). */
void PTRAP_SetOplRing( uint32_t base )
{
    RMStubLinear  = base;
    OplRingLinear = base + OPLRING_OFF;
    OplRing = NearPtr( OplRingLinear );
    memset( OplRing, 0, OPLRING_ENTRIES * 2 );
}

void PTRAP_DrainOplRing( void )
{
    struct rmcode1 *dm;
    uint16_t head, tail;
    uint8_t f;
    if ( !OplRing ) return;
    dm = RMStub();
    if ( dm->rhead == dm->rtail ) return;
    /* two contexts drain (SNDISR tick + trap-path drain-before-sync); the
     * cli guard makes consumption single-file, but a full-ring drain under
     * one cli would stall IRQ4 past the 16550 FIFO (COMRADE) -- so consume
     * in 32-entry chunks with interrupt windows between them. */
    {   /* ring telemetry (BIOS IAC): 4F7 = stub-side ring-full events,
         * 4FE = occupancy high-water in units of 4 entries (the ring
         * outgrew a byte: 255 here = 1020 = full), 4FF = drain calls */
        uint8_t *iac = NearPtr(0x4F0);
        uint16_t occ = (uint16_t)((dm->rhead - dm->rtail) & OPLRING_MASK);
        if ( (occ >> 2) > iac[0x0E] ) iac[0x0E] = (uint8_t)(occ >> 2);
        iac[0x07] = dm->rfull;
        iac[0x0F]++;
    }
    do {
        int n = 32;
        f = Drain_Cli();
        head = dm->rhead;
        tail = dm->rtail;
        while ( tail != head && n-- ) {
            VOPL3_388( 0x388, OplRing[tail*2+1], TRAPF_OUT );
            VOPL3_389( 0x389, OplRing[tail*2],   TRAPF_OUT );
            tail = (uint16_t)(( tail + 1 ) & OPLRING_MASK);
        }
        dm->rtail = tail;
        Drain_Sti(f);
    } while ( tail != head );
    SyncOplStatusCache();
}


/* is this table slot one of the OPL handlers? (covers the 0x220/0x228 SB
 * FM aliases too, which must not overtake buffered writes either) */
static int IsOplHandler( PORT_TRAP_HANDLER h )
{
    return h == VOPL3_388 || h == VOPL3_389 || h == VOPL3_38A || h == VOPL3_38B;
}
#endif

#endif /* HANDLE_IN_388H_DIRECTLY || !RMPICTRAPDYN */

bool PTRAP_Prepare_RM_PortTrap()
////////////////////////////////
{
    static __dpmi_regs TrapHandlerREG; /* static RMCS for RMCB */
#if HANDLE_IN_388H_DIRECTLY || !RMPICTRAPDYN
    struct rmcode1 *dosmem;
    uint32_t stubbytes;
#endif

    QPI_regs.x.ax = 0x1A06;
    /* get current trap handler */
    if(__dpmi_simulate_real_mode_procedure_retf(&QPI_regs) != 0 || (QPI_regs.x.flags & CPU_CFLAG))
        return false;
    QPI_OldCallback.v86.offset  = QPI_regs.x.di;
    QPI_OldCallback.v86.segment = QPI_regs.x.es;
    dbgprintf(("PTRAP_Prepare_RM_PortTrap: old callback=%x:%x\n",QPI_OldCallback.v86.segment, QPI_OldCallback.v86.segment));

    /* get a realmode callback */
    if ( _hdpmi_rmcbIO( &RM_TrapHandler, &TrapHandlerREG, &rmcb ) == 0 )
        return false;

#if HANDLE_IN_388H_DIRECTLY || !RMPICTRAPDYN
    /* copy 16-bit code to DOS memory (PSP:60h -- or the registered
     * DOS-block home when the TP755 write-ring build outgrew the PSP) */
#if HANDLE_IN_388H_DIRECTLY
    /* This build's stub (with the OPL write-ring code, ~186 bytes) does not
     * fit PSP_STUB_ROOM, and only sc_tp755 registers a home for it. Under any
     * other /CARD the stub was copied past the end of the PSP: DOS frees that
     * memory when VSBPCM goes resident and writes the next MCB over the
     * stub's tail, so the first trapped port access ran into garbage
     * (IBMAUD + Monkey Island on the T2130CT wedged at once, 2026-09-30).
     * So any backend that has not registered a home gets one here. */
    if ( !RMStubLinear ) {
# ifdef DJGPP
        uint32_t eax = 0x0100, edx = 0;
        uint8_t err;
        __asm__ __volatile__("int $0x31; setc %0"
                             : "=q"(err), "+a"(eax), "=d"(edx)
                             : "b"(PTRAP_RMHOME_PARA)
                             : "cc", "memory");
        if ( err ) {
            printf("Error: no DOS memory for the v86 stub (%u paragraphs)\n",
                   (unsigned)PTRAP_RMHOME_PARA );
            return false;
        }
        PTRAP_SetOplRing( (eax & 0xFFFFUL) << 4 );
# else
#  error "HANDLE_IN_388H_DIRECTLY needs a DOS allocation for the stub home here"
# endif
    }
    dosmem = RMStubLinear ? NearPtr(RMStubLinear)
                          : NearPtr(_my_psp() + DOSMEMSTART);
    dosheap = copyrmcode( (void *)dosmem, 0 );
    stubbytes = (uint32_t)((uint8_t *)dosheap - (uint8_t *)dosmem);
    if ( RMStubLinear ) {
        /* the SB-ISR stub must stay INSIDE the PSP: _SB_InstallISR builds
         * its real-mode vector as PSPseg:(ptr - PSP), so anything farther
         * than 64K yields a garbage INT 0Fh vector (DOOM/MI2 crashed on
         * the first injected SB interrupt). rmcode1's move to the DOS
         * block freed PSP:60h -- the 17-byte stub goes right back there. */
        dosheap = NearPtr(_my_psp() + DOSMEMSTART);
        /* copyrmcode just wrote the template: re-arm the OPL write ring.
         * If the stub ever grows past OPLRING_OFF it would be writing its
         * own tail over ring entries, so leave the ring disarmed instead --
         * rseg 0 sends the stub down its synchronous path (slower FM, but
         * correct) rather than corrupting the buffered writes. */
        if ( OplRingLinear ) {
            if ( stubbytes > OPLRING_OFF ) {
                dosmem->rseg = 0;
                printf("OPL: v86 stub %lu bytes, ring at %u -- ring off"
                       " (raise OPLRING_OFF)\n",
                       (unsigned long)stubbytes, OPLRING_OFF );
            } else {
                dosmem->rhead = dosmem->rtail = 0;
                dosmem->rseg = (uint16_t)(OplRingLinear >> 4);
            }
        }
    }
#else
    dosmem = NearPtr(_my_psp() + DOSMEMSTART);
# ifdef DJGPP
    /* /LPT: install the LPTSTUB variant in a DOS block of its own (see
     * RMLptStub). Running out of DOS memory is not fatal: the ordinary stub
     * then serves /LPT through the RMCB, as before the fast path existed. */
    if ( FOpts.lpt ) {
        uint32_t eax = 0x0100, edx = 0;
        uint8_t err;
        __asm__ __volatile__("int $0x31; setc %0"
                             : "=q"(err), "+a"(eax), "=d"(edx)
                             : "b"(( rmcodesize( RMCODE_LPT ) + 15 ) / 16)
                             : "cc", "memory");
        if ( err )
            printf("FM: no DOS memory for the /LPT v86 stub, FM takes the slow path\n");
        else {
            RMStubLinear = (eax & 0xFFFFUL) << 4;
            RMLptStub = 1;
            dosmem = NearPtr( RMStubLinear );
        }
    }
    if ( RMLptStub ) {
        dosheap = copyrmcode( (void *)dosmem, RMCODE_LPT );
        stubbytes = (uint32_t)((uint8_t *)dosheap - (uint8_t *)dosmem);
        ((struct rmlpt *)dosmem->codev86)->wLpt = 0;  /* armed by PTRAP_Prepare */
        /* the SB-ISR stub stays inside the PSP (see the TP755 note above) */
        dosheap = NearPtr(_my_psp() + DOSMEMSTART);
    } else
# endif
    {
        dosheap = copyrmcode( (void *)dosmem, 0 );
        stubbytes = (uint32_t)((uint8_t *)dosheap - (uint8_t *)dosmem);
    }
#endif
    /* Nothing past the PSP survives going resident: stubs that do not fit
     * there must not install. rmcode2 is measured by copying it to dosheap,
     * where _SB_InstallISR puts the same bytes when the SB IRQ is 7. */
    if ( (uint8_t *)dosmem == (uint8_t *)NearPtr(_my_psp() + DOSMEMSTART) ) {
        uint32_t isrbytes = (uint32_t)((uint8_t *)copyrmcode( dosheap, 1 )
                                       - (uint8_t *)dosheap);
        if ( stubbytes + isrbytes > PSP_STUB_ROOM ) {
            printf("Error: v86 stubs (%u + %u bytes) do not fit the PSP\n",
                   (unsigned)stubbytes, (unsigned)isrbytes );
            return false;
        }
    }

    /* the code starts with a rmcode1 struct, now to be initialized...  */
    dosmem->rmcb = rmcb.segofs;
    dosmem->wFmSB  = 0xFFFF;    /* armed by PTRAP_Prepare when the config allows */
    dosmem->wDspWS = 0xFFFF;
    dosmem->bDspWS = 0;
    RMStubReady = 1;

#if !RMPICTRAPDYN
    dosmem->qpi = (QPI_regs.x.cs << 16) | QPI_regs.x.ip;
#endif
    /* set new trap handler ES:DI */
    //r.x.di = 4+2+2+4;
    QPI_regs.x.di = offsetof(struct rmcode1, codev86);
#if !HANDLE_IN_388H_DIRECTLY
    if ( RMLptStub )
        QPI_regs.x.di += sizeof(struct rmlpt);  /* LPTSTUB's code starts later */
#endif
    QPI_regs.x.es = RMStubLinear ? (uint16_t)(RMStubLinear >> 4)
                                 : ((_my_psp() + DOSMEMSTART) >> 4);
#else
    QPI_regs.x.di = rmcb.v86.offset;
    QPI_regs.x.es = rmcb.v86.segment;
#endif
#if DACRING
    /* VSBPCMJ hands the DSP writes it does not keep to this same entry */
    QPI_StubCB = ((uint32_t)QPI_regs.x.es << 16) | QPI_regs.x.di;
#endif
    QPI_regs.x.ax = 0x1A07; /* set trap handler */
    if( __dpmi_simulate_real_mode_procedure_retf(&QPI_regs) != 0 || (QPI_regs.x.flags & CPU_CFLAG))
        return false;
#if DACRING
    JlmDetect();
#endif
    return true;
}

/* install a range of port traps using QPI */

static bool Install_RM_PortRangeTrap( uint16_t start, uint16_t end )
////////////////////////////////////////////////////////////////////
{
    int i;

    for( i = start; i < end; i++ ) {
#if DACRING
        if ( JlmOwns( PortTable[i] & 0x7fff ) )
            continue;                   /* VSBPCMJ traps it at ring 0 */
#endif
        if ( QPI_OldCallback.v86.segment ) {
            /* this is unreliable, since if the port was already trapped, there's no
             * guarantee that the previous handler can actually handle it.
             * so it might be safer to ignore the old state and - on exit -
             * untrap the port in any case!
             */
            QPI_regs.x.ax = 0x1A08; /* get port status */
            QPI_regs.x.dx = PortTable[i] & 0x7fff;
            __dpmi_simulate_real_mode_procedure_retf(&QPI_regs);
            PortState[i] |= (QPI_regs.h.bl) << 8; //previously trapped state
        }
        QPI_regs.x.ax = 0x1A09; /* trap port */
        QPI_regs.x.dx = PortTable[i] & 0x7fff;
        __dpmi_simulate_real_mode_procedure_retf(&QPI_regs); /* trap port */
        PortState[i] |= PDT_FLGS_RMINST;
    }
    return true;
}

/* install all real-mode port trap ranges */

bool PTRAP_Install_RM_PortTraps( void )
///////////////////////////////////////
{
    int i;

    dbgprintf(("PTRAP_Install_RM_PortTraps: maxports=%u, maxranges=%u\n", maxports, maxranges ));
#if DACRING
    JlmArm();   /* first: the ports it takes stay off the QPI traps below */
#endif
    for ( i = 0; i < maxranges; i++ ) {
        dbgprintf(("PTRAP_Install_RM_PortTraps: range[%u]: ports %X-%X\n", i, PortTable[portranges[i]], PortTable[portranges[i+1]-1] ));
#if RMPICTRAPDYN
        if ( PortTable[portranges[i]] == 0x20 ) {
            PICIndex = portranges[i];
            continue;
        }
#endif
        Install_RM_PortRangeTrap( portranges[i], portranges[i+1] );
    }
    return true;
}

/* set PIC port trap when a SB IRQ is emulated.
 * if RMPICTRAPDYN==0, the PIC port is permanently trapped;
 * to avoid mode switches, the trapping is handled in v86-mode
 * if the port is accessed in v86-mode and SB IEQ isn't virtualized:
 *  - [psp:86h] = -1      activates SB irq virtualization
 *  - [psp:86h] = 0020h deactivates SB irq virtualization
 */

void PTRAP_SetPICPortTrap( int bSet )
/////////////////////////////////////
{
    /* might be called even if support for v86 is disabled */
    if ( QPI_regs.x.cs ) {
#if RMPICTRAPDYN
        QPI_regs.x.dx = PDispTab[PICIndex].port;
        if ( bSet ) {
            QPI_regs.x.ax = 0x1A09; /* trap */
            PortState[PICIndex] |= PDT_FLGS_RMINST;
        } else {
            QPI_regs.x.ax = 0x1A0A; /* untrap */
            PortState[PICIndex] &= ~PDT_FLGS_RMINST;
        }
        __dpmi_simulate_real_mode_procedure_retf(&QPI_regs); /* trap port */
#else
        /* patch the 16-bit real-mode code stored in the PSP;
         * see rmcode1.asm, wPICp.
         */
        struct rmcode1 *dosmem = RMStubLinear ? NearPtr(RMStubLinear)
                                              : NearPtr(_my_psp() + DOSMEMSTART);
        //WriteLinearW( dosmem, bSet ? 0xffff : 0x0020 );
        dosmem->wPort = (bSet ? 0xffff : 0x0020);
#endif
    }
    return;
}

bool PTRAP_Uninstall_RM_PortTraps( void )
/////////////////////////////////////////
{
    int i;

#if DACRING
    JlmDisarm();
#endif
    for( i = 0; i < maxports; ++i ) {
        if ( !( PortState[i] & 0xff00 )) {
            if( PortState[i] & PDT_FLGS_RMINST ) {
                QPI_regs.x.ax = 0x1A0A; /* clear port trap */
                QPI_regs.x.dx = PortTable[i];
                __dpmi_simulate_real_mode_procedure_retf(&QPI_regs);
                PortState[i] &= ~PDT_FLGS_RMINST;
                //dbgprintf(("PTRAP_Uninstall_RM_PortTraps: port %X untrapped\n", PortTable[i] ));
            }
        }
    }
    QPI_regs.x.ax = 0x1A07; /* set trap handler */
    QPI_regs.x.di = QPI_OldCallback.v86.offset;
    QPI_regs.x.es = QPI_OldCallback.v86.segment;
    if( __dpmi_simulate_real_mode_procedure_retf(&QPI_regs) != 0) //restore old handler
        return false;

    __dpmi_free_real_mode_callback( &rmcb );

    return true;
}

bool PTRAP_DetectHDPMI()
////////////////////////
{
    uint8_t result = _hdpmi_get_vendor_api(&HDPMIAPI_Entry);

#if 0 //detect jhdpmi.dll
	__dpmi_regs r;
	uint32_t *dosmem = NearPtr(_my_psp() + 0x5C);
	*dosmem = 0xCB2FCD; /* INT 2Fh & RETF */
	r.x.ax = 0x1684;
	r.x.bx = 0x4858;
	r.x.cs = _my_psp() >> 4;
	r.x.ip = 0x5C;
	r.x.flags = 0x202;
	r.x.ss = r.x.sp = 0;
	if( __dpmi_simulate_real_mode_procedure_retf(&r) == 0 && r.h.al == 0 )
		jhdpmi = 1;
#endif

	return (result == 0 && HDPMIAPI_Entry.seg);
}

static uint32_t PTRAP_Int_Install_PM_Trap( int start, int end, void(*handlerIn)(void), void(*handlerOut)(void) )
////////////////////////////////////////////////////////////////////////////////////////////////////////////////
{
    struct _hdpmi_traphandler traphandler;
#ifdef NOTFLAT
    traphandler.ofsIn  = (uint16_t)handlerIn;
    traphandler.ofsOut = (uint16_t)handlerOut;
#else
    traphandler.ofsIn  = (uint32_t)handlerIn;
    traphandler.ofsOut = (uint32_t)handlerOut;
#endif
    return _hdpmi_install_trap( start, end - start + 1, &traphandler );
}

#if 0//def _DEBUG
void PTRAP_PrintPorts( void )
/////////////////////////////
{
    int start = 0;
    int i;
    dbgprintf(( "PTRAP_PrintPorts:\n" ));
    for ( i = 0; i < maxports; i++ ) {
        if ( i == ( maxports - 1 ) || ( PortTable[i+1] != PortTable[i]+1 || PortState[i+1] != PortState[i] ) ) {
            if ( i == start )
                dbgprintf(( "%X (%X)\n", PortTable[start], PortState[start] ));
            else
                dbgprintf(( "%X-%X (%X)\n", PortTable[start], PortTable[i], PortState[start] ));
            start = i + 1;
        }
    }
    return;
}
#endif

bool PTRAP_Install_PM_PortTraps( void )
///////////////////////////////////////
{
    int i;
    int start, end;

    /* reset hdpmi=32 option in case it is set */
    _hdpmi_set_context_mode( 0 );

#ifndef NOTFLAT
    /* install CLI handler */
    _hdpmi_set_cli_handler( _hdpmi_CliHandler );
#endif
    for ( i = 0; i < maxranges; i++ ) {
        if ( portranges[i+1] > portranges[i] ) { /* skip if range is empty */
            start = PortTable[portranges[i]];
            end = PortTable[portranges[i+1] - 1];
            dbgprintf(("PTRAP_Install_PM_PortTraps: %X-%X\n", start, end ));
            if (!(traphdl[i] = PTRAP_Int_Install_PM_Trap( start, end, &SwitchStackIOIn, &SwitchStackIOOut)))
                return false;
        }
    }
#if 0//def _DEBUG
    PTRAP_PrintPorts();
#endif
    return true;
}

/* delete 1-x entries in PortTable[] and PortHandler[], adjust port ranges */

static void PDT_DelEntries( int start, int end, int entries )
/////////////////////////////////////////////////////////////
{
    int i;
    for ( i = start; i < end - entries; i++ ) {
        PortTable[i] = PortTable[i + entries];
        PortHandler[i] = PortHandler[i + entries];
    }
    maxports -= entries;
    for ( i = 0; i <= maxranges; i++ ) {
        if ( portranges[i] > start ) {
            portranges[i] -= entries;
        }
    }
}

/* adjust PortTable[] and PortHandler[] to current settings of /D, /H, /A, /OPL
 * note: sndirq is the irq of the real sound hardware!
 */

/* Forward an access to the emulated SB's FM alias on to the REAL OPL3.
 *
 * On a real Sound Blaster, base+0..3 and base+8/9 ARE the FM chip, and games
 * probe them: Duke Nukem II's SB detection runs a full AdLib timer test at
 * base+8 and refuses to touch the DSP at all unless it passes.  Upstream can
 * simply drop these ports under /FM because there the emulated base and the
 * real OPL are the same card.  Our real FM is at 0x388 on a separate PCMCIA
 * base, so dropping them left the alias reading an open bus (0xFF) -- the
 * timer test's first check is "status & 0xE0 == 0", which 0xFF fails.
 *
 * The SB base is 16-byte aligned, so the low two bits of the port select the
 * OPL3 port pair for both alias windows (base+8/9 -> 388/389).
 *
 * With /FMVOL armed the real 0x388-0x38B are themselves trapped for carrier
 * scaling, so alias traffic is routed through those same handlers -- writing
 * straight to the chip here would sneak past the attenuation and play the FM
 * alias at full volume.
 *
 * On a card with NO FM silicon (the 755C's planar CS4248) there is nothing to
 * forward TO -- 0x388 is open bus and the probe fails on 0xFF -- so the same
 * aliases are answered by the timer-only shim instead. See fmshim.c.
 */
/* Write one OPL register the slow way. The chip needs a settle after the
 * index write and a longer one after the data write; ISA port reads are the
 * traditional ~1us delay and cost nothing on a 486. */
static void FM_Reg( uint8_t reg, uint8_t val )
{
    int i;
    UntrappedIO_OUT( 0x388, reg );
    for ( i = 0; i < 6; i++ )  UntrappedIO_IN( 0x388 );
    UntrappedIO_OUT( 0x389, val );
    for ( i = 0; i < 35; i++ ) UntrappedIO_IN( 0x388 );
}

/* Does an OPL actually ANSWER at 0x388? This is the AdLib timer test itself:
 * reset both timers, check the status bits are clear, start timer 1 on its
 * shortest preset, wait, and check T1 reports expired. It is exactly what a
 * guest runs to decide an FM chip exists (Duke Nukem II's SB probe gates the
 * DSP on it), which is what makes trusting it safe: if this fails, the
 * guest's identical probe would fail too, so the shim is the right answer no
 * matter what the card's ops table claims.
 *
 * Why it is needed: one backend can serve two boards. sc_vew211 declares
 * PTF_REAL_FM for the CF-VEW211's discrete YMF262, but the NEC PC-9801N-J04
 * is the same MEI ASIC and CS4231A with NO FM fitted -- and the driver cannot
 * tell them apart. Asking the hardware removes the guess.
 *
 * Runs from PTRAP_Prepare, before the port traps are installed, so this is
 * the real bus and not our own shim answering. Timers are left reset.
 */
static int FM_Answers( void )
{
    uint8_t s1, s2;
    int i;

    FM_Reg( 4, 0x60 );                 /* reset timer 1 + timer 2 */
    FM_Reg( 4, 0x80 );                 /* reset the IRQ flags */
    s1 = UntrappedIO_IN( 0x388 );      /* must read back with bits 5-7 clear */
    FM_Reg( 2, 0xFF );                 /* timer 1 preset: expires in ~80us */
    FM_Reg( 4, 0x21 );                 /* unmask + start timer 1 */
    for ( i = 0; i < 400; i++ )        /* comfortably past 80us */
        UntrappedIO_IN( 0x388 );
    s2 = UntrappedIO_IN( 0x388 );      /* must now report T1 expired (0xC0) */
    FM_Reg( 4, 0x60 );                 /* leave the chip as we found it */
    FM_Reg( 4, 0x80 );

    return ( ( s1 & 0xE0 ) == 0 && ( s2 & 0xE0 ) == 0xC0 );
}

static uint8_t FM_Alias( uint16_t port, uint8_t val, uint16_t flags )
{
    uint16_t fm = 0x388 + (port & 3);
    if ( FmShimOn )
        return FMSHIM_Acc( fm, val, flags );
    if ( FMVOL_Active() ) {
        switch ( port & 3 ) {
        case 0:  return FMVOL_388( fm, val, flags );
        case 1:  return FMVOL_389( fm, val, flags );
        case 2:  return FMVOL_38A( fm, val, flags );
        default: return FMVOL_38B( fm, val, flags );
        }
    }
    /* DIRECT I/O, not UntrappedIO_*: this tail runs only when neither the
     * shim nor the attenuator is active, and that is EXACTLY the branch
     * below that deletes 0x388-0x38B from the port table (search
     * PDT_DelEntries/OPL3_PDT). The destination is therefore untrapped, so
     * the UntrappedIO_* detour -- a second round trip into the host on top
     * of the trap exception that got us here -- buys nothing. Halving that
     * cost matters because guests drive FM from a timer ISR in bursts: on a
     * 486SX the ISR overran its period and Duke3D's music played SLOW with
     * Sound Blaster (not AdLib) selected as the music source. Bench-found
     * on the PC110, 2026-08-23.
     * DO NOT hoist this above the two early returns. FMVOL keeps
     * 0x388-0x38B TRAPPED on purpose and must forward through the host
     * (fmvol.c FV_OUTB); a direct outp there would re-enter its own trap.
     * FMSHIM keeps them trapped too and never touches 388h (with /LPT it
     * writes only the untrapped LPT ports). */
    if ( flags & TRAPF_OUT ) {
        outp( fm, val );
        return val;
    }
    return (uint8_t)inp( fm );
}

void PTRAP_Prepare( int opl, int sbaddr, int dma, int hdma, int sndirq )
////////////////////////////////////////////////////////////////////////
{
    int i;
    dbgprintf(("PTRAP_Prepare: opl=%X, sb=%X, dma=%X, hdma=%X)\n", opl, sbaddr, dma, hdma ));
    /* low dma: adjust the entry for DMA channel addr/count */
    PortTable[portranges[DMA_PDT] + 0] = dma * 2;
    PortTable[portranges[DMA_PDT] + 1] = dma * 2 + 1;
    /* low dma: adjust the entry for DMA page reg */
    PortTable[portranges[DMAPG_PDT]] = ChannelPageMap[ dma ];
    /* if the sound hw IRQ is < 8, the slave PIC doesn't need to be trapped */
    if ( sndirq < 8 ) {
        PDT_DelEntries( portranges[SPIC_PDT], maxports, 2 );  /* 0xA0 + 0xA1 */
    }
#if SB16
    if ( hdma ) {
        /* high dma: adjust the entry for DMA channel addr/count */
        PortTable[portranges[HDMA_PDT] + 0] = hdma * 4 + (0xC0-0x10);
        PortTable[portranges[HDMA_PDT] + 1] = hdma * 4 + 2 + (0xC0-0x10);
        /* high dma: adjust the entry for DMA page reg */
        PortTable[portranges[DMAPG_PDT] + 1] = ChannelPageMap[ hdma ];
    } else {
        /* if no SB16 emulation, remove all HDMA ports */
        PDT_DelEntries( portranges[DMAPG_PDT] + 1, maxports, 1 );
        PDT_DelEntries( portranges[HDMA_PDT], maxports, portranges[HDMA_PDT+1] - portranges[HDMA_PDT] );
    }
#endif
#if VMPU
    if ( gvars.mpu ) {
        PortTable[portranges[MPU_PDT] + 0] = gvars.mpu;
        PortTable[portranges[MPU_PDT] + 1] = gvars.mpu + 1;
    } else {
        PDT_DelEntries( portranges[MPU_PDT], maxports, 2 );
    }
#endif
    /* adjust the SB ports to the selected base */
    if ( sbaddr != 0x220 )
        for( i = portranges[SB_PDT]; i < portranges[SB_PDT+1]; i++ )
            PortTable[i] += sbaddr - 0x220;
#if DACRING
    PrepSB = sbaddr;    /* for VSBPCMJ (JlmArm) */
    PrepIrq = sndirq;
    PrepOpl = opl;
#endif

    /* DSP write-status reads (base+0xC) are answered by the V86 stub from a
     * counter it shares with vsb.c (rmcode1.asm isws, PTRAP_DspStatusCell):
     * the port stays trapped for its writes, only the reads stay in V86.
     * SBENOSTUB=1 (bench knob, transient like SBERTC/SBENORS) leaves both
     * stub-answered paths -- this one and the FM alias forward above --
     * disarmed, so the C handlers serve everything as before: an A/B on the
     * box without a rebuild. */
    { struct rmcode1 *dm = RMVars();
      if ( dm && !getenv("SBENOSTUB") ) dm->wDspWS = (uint16_t)( sbaddr + SB_PORT_DSP_WRITE_WS ); }



    /* if no OPL3 emulation, skip ports 0x388-0x38b, 0x220-0x223 and 0x228-0x229 */
    if ( !opl ) {
        /* Does this card have FM silicon behind 0x388? The backend says so in
         * its ops table (ptops.h). /FMSHIM forces the FM-less path on a card
         * that does have a chip -- a bench knob for exercising the shim
         * (expect FM music to go silent while SB detection keeps working). */
        /* Ask the hardware rather than trusting the paperwork: a backend can
         * serve two boards with different silicon (CF-VEW211 vs J04). */
        /* /LPT forces the same path: the OPL3LPT is write-only, so its
         * status has to come from the shim, and the card's own chip (if
         * any) is left untouched behind the trapped 388h. */
        FmShimOn = 1;
        if ( FOpts.fmshim || FOpts.lpt )
            ;                                  /* forced: bench knob or /LPT */
        else if ( PT_Ops->flags & PTF_REAL_FM )
            FmShimOn = !FM_Answers();
        if ( FmShimOn && !FOpts.fmshim && !FOpts.lpt && ( PT_Ops->flags & PTF_REAL_FM ) )
            printf("FM: card claims a chip at 388h, none answered\n");

        if ( FmShimOn ) {
            /* No chip anywhere: keep 0x388-0x38B TRAPPED (they would read
             * open bus otherwise) and answer them from the shim, so a guest
             * probing AdLib directly detects an OPL and a guest probing the
             * SB base aliases gets past its FM gate to the DSP. No synthesis
             * is compiled in on this path, so music stays silent -- what this
             * recovers is the card's digital audio. With /LPT the shim also
             * forwards every write to the OPL3LPT, and music plays there. */
            int f = portranges[OPL3_PDT];
            FMSHIM_Reset();
            PortHandler[f+0] = FMSHIM_Acc; PortHandler[f+1] = FMSHIM_Acc;
            PortHandler[f+2] = FMSHIM_Acc; PortHandler[f+3] = FMSHIM_Acc;
            if ( FOpts.lpt ) {
                /* transient bench knobs for the LPT timing: SBELPTDLY sets
                 * the control-port reads after each strobe (1-255, def 6),
                 * SBELPTCLI=0 leaves interrupts on in the stub's writes */
                const char *fast = "";
                const char *e = getenv("SBELPTDLY");
                int lptdly = e ? (int)strtol( e, NULL, 10 ) : 6;
                int lptcli = !( ( e = getenv("SBELPTCLI") ) && *e == '0' );
                if ( lptdly < 1 || lptdly > 255 )
                    lptdly = 6;
                FMSHIM_SetLpt( (uint16_t)FOpts.lpt, lptdly );
#if DACRING
                PrepLptDly = lptdly;        /* VSBPCMJ's writes use it too */
#endif
#if !HANDLE_IN_388H_DIRECTLY
                /* Arm the LPTSTUB variant: it serves real-mode FM itself --
                 * 388h-38Bh, and through wFmSB the SB-base aliases, which
                 * its isfm hands to islpt instead of 388h -- sharing the
                 * shim's index shadow and status cache. SBENOSTUB=1 leaves
                 * it disarmed, so everything takes the RMCB as before. */
                { struct rmcode1 *dm = RMVars();
                  if ( dm && RMLptStub && !getenv("SBENOSTUB") ) {
                      struct rmlpt *lp = (struct rmlpt *)dm->codev86;
                      FMSHIM_SetStubCells( (uint8_t *)&dm->data,
                                           (uint8_t *)&dm->data + 1 );
                      lp->bDly = (uint8_t)lptdly;
                      lp->bCli = (uint8_t)lptcli;
                      lp->wLpt = (uint16_t)FOpts.lpt;
                      dm->wFmSB = (uint16_t)sbaddr;
                      fast = ", v86 fast path";
                  }
                }
#endif
                printf("FM: OPL3LPT at %Xh%s, 388h status from the timer shim\n",
                       FOpts.lpt, fast );
                if ( lptdly != 6 || !lptcli )
                    printf("FM: LPT bench knobs: %d delay reads, stub cli %s\n",
                           lptdly, lptcli ? "on" : "off" );
            } else {
                /* 80 columns: the old wording ran to 82 and wrapped. Keep the
                 * "no music" half -- it is what stops the silence being
                 * reported as a bug -- and drop "on this card" instead. */
                printf("FM: no chip - timer-only OPL3 shim at 388h"
                       " (detection only, no music)\n");
            }
#ifndef NOTFLAT
            /* transient bench knob: patch protected-mode guests' FM delay
             * reads out of their code (PatchDelayIn, FMSHIM.C) */
            { const char *e = getenv("SBEFMPATCH");
              if ( e && *e == '1' ) {
                  FMSHIM_SetPatch( 1 );
                  printf("FM: SBEFMPATCH - PM delay reads patched to NOP\n");
              }
            }
#endif
        } else if ( FMVOL_Active() ) {
            /* FMVOL: keep 0x388-0x38B trapped, but filtered+forwarded to the
             * REAL OPL3 with carrier-level scaling.  Because these are the
             * ordinary port traps (QPI for V86, HDPMI32i for PM), this is the
             * FM volume protected-mode games get - the half a JLM can't reach.
             * The 0x220-0x223/0x228-0x229 FM aliases still go: the CF-VEW211
             * decodes FM at 0x388 only. */
            int f = portranges[OPL3_PDT];
            PortHandler[f+0] = FMVOL_388; PortHandler[f+1] = FMVOL_389;
            PortHandler[f+2] = FMVOL_38A; PortHandler[f+3] = FMVOL_38B;
        } else {
            /* real chip, no attenuator wanted: leave 0x388-0x38B UNtrapped so
             * guest AdLib music rides the hardware directly */
            PDT_DelEntries( portranges[OPL3_PDT], maxports, 4 );
            /* ...and let the V86 stub forward the SB-base aliases to it
             * itself (rmcode1.asm isfm): with 0x388 untrapped the stub can do
             * the I/O in place, so a real-mode guest's alias traffic -- the
             * register writes AND the 6-35 delay reads era drivers pad each
             * one with -- costs no RMCB round trip at all. FM_Alias below
             * still serves the PM world and decomposed word accesses. Only in
             * THIS branch: the shim and FMVOL keep 0x388 trapped, and a
             * direct OUT from the stub would re-enter the trap. */
            { struct rmcode1 *dm = RMVars();
              if ( dm && !getenv("SBENOSTUB") ) dm->wFmSB = (uint16_t)sbaddr; }
        }


        /* The SB-base FM aliases are KEPT and forwarded to 0x388-0x38B rather
         * than dropped -- games probe them to decide a Sound Blaster exists.
         * See FM_Alias().  (The CF-VEW211 decodes FM at 0x388 only, which is
         * exactly where these are sent.) */
        { int b = portranges[SB_PDT];
          PortHandler[b+0] = FM_Alias;   /* base+0 */
          PortHandler[b+1] = FM_Alias;   /* base+1 */
          PortHandler[b+2] = FM_Alias;   /* base+2 */
          PortHandler[b+3] = FM_Alias;   /* base+3 */
          PortHandler[b+7] = FM_Alias;   /* base+8 */
          PortHandler[b+8] = FM_Alias; } /* base+9 */
    }

    /* delete empty port ranges */
    for ( i = 0; i < maxranges; i++ ) {
        if ( 0 == portranges[i+1] - portranges[i] ) {
            int j;
            for ( j = i; j < maxranges; j++) {
                portranges[j] = portranges[j+1];
            }
            maxranges--;
        }
    }

#ifdef _DEBUG
    dbgprintf(("PTRAP_Prepare: maxports=%u, maxranges=%u\n", maxports, maxranges ));
    for( i = 0; i < maxranges; i++ ) {
        dbgprintf(("PTRAP_Prepare: range[%u]: ports %X-%X\n", i, PortTable[portranges[i]], PortTable[portranges[i+1]-1] ));
    }
#endif

}

bool PTRAP_Uninstall_PM_PortTraps( void )
/////////////////////////////////////////
{
    int i;
    for ( i = 0; traphdl[i]; i++ )
        _hdpmi_uninstall_trap( traphdl[i] );

#ifndef NOTFLAT
    /* uninstall CLI trap handler */
    _hdpmi_set_cli_handler( NULL );
#endif

    return true;
}

void PTRAP_UntrappedIO_OUT(uint16_t port, uint8_t value)
////////////////////////////////////////////////////////
{
    _hdpmi_simulate_byte_out( port, value );
    return;
}

uint8_t PTRAP_UntrappedIO_IN(uint16_t port)
///////////////////////////////////////////
{
    return _hdpmi_simulate_byte_in( port );
}

#if PT0V86

/* v1.8: get physical address of v86 pagetab 0;
 * this is implemented by an addition to QPIEMU - it
 * won't work for Qemm.
 */

uint32_t PTRAP_GetPageTab0v86( void )
/////////////////////////////////////
{
    if ( QPI_regs.x.cs ) {
        QPI_regs.x.ax = 0x5000;
        __dpmi_simulate_real_mode_procedure_retf(&QPI_regs);
        if ( 0 == ( QPI_regs.x.flags & 1 ) )
            return ( QPI_regs.d.edx );
    }
    return 0;
}
#endif

