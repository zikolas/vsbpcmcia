# TP755 backend (sc_tp755.c) — status & FM handoff

Branch `tp755-backend` (off main/v0.6). Bench day 2026-08-14, ThinkPad 755C
(486DX4/75, 20MB). Everything below is hardware-verified on that machine
unless marked otherwise. Full narrative lives in the session memory
(`tp755-audio-project.md`); this doc is the branch-coupled summary.

## What ships on this branch

`CARD=TP755 ./tools/build.sh` → **VSBPCMT.EXE**: VSBPCM backend for the
755C's internal Crystal CS4248 (AD1848/WSS, FRU 84G4289; also 750-family +
360PE per ThinkWiki). Default no-CARD build verified **byte-identical** to
released v0.6 (226488 / crc 3342316118) after every change on this branch.

- Enable: ThinkPad ctl port 0x15E8 idx 0x1C, set bit 0x02 (from Linux
  wss_lib.c; driver does it itself at detect — **no enabler program**).
  Disabled card reads 0x80 on all four codec ports (presence tell; float
  would be 0xFF).
- Codec block at 0x4E30 (Index/Data/Status/PIO), ID: I12 = 0x8A. IRQ 10,
  8237 DMA channel 0 — real hardware-paced autoinit ring in DOS conventional
  memory (own DPMI 0100h alloc: the stock XMS allocator is ISA-DMA-unsafe —
  above 16MB + 64K-crossing possible). Ring 8K, period = /PS (default 512,
  deploy uses 1024). Codec count = period frames → **IRQ10 per period is the
  engine clock** (SC_ICH model; no RTC, no PIO pump, ES1688_PT=0 stubs).
- CARD_TP755 un-NOFMs config.h: dbopl OPL3 emulation compiled in (the 755C
  has no FM hardware anywhere — fleet first).
- Codec gotchas honored: DACs power up MUTED (I6/I7=0x80); I8 readback under
  MCE returns 0x80 (busy) — verify after autocal (~100ms, poll I11.ACI);
  ms-paced verified-MCE writes (sc_vew211 recipe minus PPIO).
- Own 8237 pokes via UntrappedIO_* only (0x08-0x0F are always PM-trapped;
  plain OUTs corrupt the vdma guest shadow). adetect refuses guest /D0.

**Digital status: daily-driver ready.** SBDIAG full pass, Epic Pinball,
DOOM with SFX. Deploy = C:\VSBPCM\VSBPCMT.EXE + GO-TP755.BAT:
`JEMM386 LOAD NOEMS X=DC00-EFFF` (exclusion mandatory — live PCMCIA/planar
windows there; JEMM's UMB scan over them hard-wedges the machine),
BLASTER=A220 I7 D1 T4, DACRATE=11025, `VSBPCMT /A220 /PS1024`.
No /CF4 (total-freeze aggravator suspicion, unresolved). No /T3 (games with
saved SB-Pro configs push stereo at a mono DSP → double speed).

## FM: three clock-killers root-caused, two fixed, one hunt open

The codec IRQ is the backend's only clock; ISA edges are unforgiving. Every
FM failure was some way of killing that clock — never CPU: telemetry showed
**zero ISR overruns at 11025** through all of it (see Telemetry below).

1. **FIXED — chained IRQ10 → IBM BIOS INT72 stub → EOIs the MASTER only**
   → slave in-service bit stuck → IRQ10..15 dead (engine + disk), master
   alive. Proven live (frozen ticks, codec SR=0x89 begging, dead IRQ14).
   Fix: irq_routine claims every IRQ10 unconditionally (line is
   codec-exclusive on this planar; chain path is lethal).
2. **OPEN(ish) — real slave IMR bit 2 masked mid-game** by guest PIC traffic
   that slips the VPIC byte-port filter (prime suspect: 16-bit OUT to 0xA0
   writes both PIC ports in one bus cycle). Proven by the MI1 resurrection:
   slave ISR=00, IRR=00, codec begging; forced IMR clear + SR re-arm →
   engine resumed mid-game. Guardian (below) heals it; the filter hole
   itself is unpatched (upstream-worthy fix).
3. **FIXED — lost first edge at install** → line sticks high forever.
   Post-PEN heal loop in card_start (wait >3 periods, re-arm if unserviced)
   + double-read-qualified heal on guest DSP reset (vsb.c hook made
   unconditional under CARD_TP755 only).

**Trap tax (the real FM burden):** era AdLib code pads every OPL register
write with 6–24 status/data-port reads (pure delay; values ignored). Each
is a full monitor round-trip → games slow progressively as music densifies
(MI2), even a bare Duke-setup AdLib test drags. ~30/32 of FM trap load is
padding.

- **V86 world: FIXED & VALIDATED.** Upstream's dormant `HANDLE_IN_388H_DIRECTLY`
  stub (rmcode1.asm) flipped on CARD_TP755-only + extended: 0x389 reads
  answered in-stub (0xFF). CFLAGS now reaches jwasm (COMPILE.asm.o + the
  rmcode1/2 -bin rules); default rmcode1.bin stays 42B, TP755's is 108B.
  Result: MI1 gameplay survived FM for the first time (only sound died —
  killer #2 above).
- **PM world (DOS4GW: DOOM, Duke): UNBUILT.** The V86 stub doesn't apply;
  PM OPL access still pays the HDPMI trap round-trip. Investigate an
  HDPMI-side equivalent, or accept DOOM-music as out of envelope.

## The IRQ0 guardian — built, SUSPECT, needs revival gating

Chained INT08 hook (go32 chain; sc_es1688 i8-heartbeat precedent) watches
the tick counter; on freeze with PEN on: unmask real IMRs (UntrappedIO from
PM context reads REAL ports — a V86/COMrade read of 0x21/0xA1 gets the
shadow), safe specific EOIs (0x62 → 0xA0 + 0x20; no-op on healthy PICs),
SR re-arm; if codec isn't begging, re-unmask DMA ch0 instead (guest 8237
master-reset case).

**Suspicion:** with the guardian in, MI1's sound wedge was followed by a
game crash to text mode; without it, the game survived its sound wedge.
Likely revival-kills-the-patient: resurrection ~1s later injects a pending
SB IRQ into stale guest state. **Next step: verify-before-revive gating**
(suppress VIRQ injection on the first resumed ticks, or gate the heal on
recent guest DSP activity). Also align the V86 stub's timer semantics with
vopl3.cpp:127 (stub ignores timer MASK bits and stores RST writes; vopl3
honors masks, ignores RST stores — masked-timer drivers may mis-detect).

Header note for sc_tp755.c: it needs system <dpmi.h>+<go32.h> (go32 chain)
which conflict with linear.h/DJDPMI.H — so it declares `extern DSBase` and
rolls its own TP_NEARPTR instead of including linear.h.

## FM endgame session (2026-08-14 pt.2) — what landed

1. **Word-access hole FIXED + fully source-verified.** Mechanism (from
   Jemm/QPIEMU/JLOAD sources): CPU faults a 16-bit OUT if EITHER byte's
   IOPM bit is set; Jemm reports only the START port; JLOAD's 64K vector
   table misses 0xA0; Jemm ≥5.84's Simulate_IO splits unmatched accesses
   into RAW real-hardware byte writes (pre-5.84 re-entered the trap chain —
   an upstream Jemm regression). So "OUT 0A0h,AX" put AH on the REAL slave
   IMR. Fix: trap 0xA0 (VPIC_PassAcc; v86 stub short-circuits byte accesses
   via QPI sim), decompose word/dword in RM_TrapHandler (Jemm CL bits:
   8=word/10h=dword) and PTRAP_PM_TrapHandler (hdpmi errcode: 10h/20h;
   return widened to uint32_t for word INs).
2. **Stub/vopl3 timer alignment BY CONSTRUCTION**: the stub answers 0x388
   reads from a 1-byte status cache that ptrap.c recomputes from
   VOPL3_388() after every OPL trap the C side handles. (NOTE: the old
   bench-day claim "vopl3 ignores RST stores" was WRONG — VOPL3_TIMERx_MASK
   macros are 0xC0/0xA0 so RST-only writes DO store and DO clear flags.)
3. **Guardian v2**: PIT-rate-proof heal qualification (SR begging on 3
   CONSECUTIVE IRQ0 polls with ticks frozen — immune to reprogrammed PIT
   rates), futility budget (8 consecutive cure-less heals → dormant until
   ticks advance; the un-healed freeze was survivable, the heal storm
   never), revive squelch (sndisr.c drops/skips injection for 2 ticks after
   an async heal — the MI1 "revival kills the patient" fix).
4. **ISR depth limiter**: >3 live SNDISR frames → ack+EOI+bail. The wedge
   fossil was 14 nested frames × 4KB ISR stack (exit-window resonance).
5. **OPL WRITE RING**: non-timer register writes cost zero PM round-trips —
   index writes just update the stub's _0004 shadow (MUST be stored on the
   timer-family sync path too — missing that store misrouted timer data
   into the ring and broke MI1's AdLib DETECT = "silent, no music" bug);
   data writes append (index,value) to a 256-entry SPSC ring drained by
   SNDISR each tick (32-entry cli chunks — a full drain under one cli
   would overrun COMRADE's 16550). Timer-family traffic stays synchronous;
   every sync/PM OPL access drains the ring first (order preserved).
   The whole real-mode home (stub 176B + ring 512B + SB-ISR stub) moved to
   DOS-block slack (+46 paragraphs in adetect) — it outgrew the PSP's 160
   bytes; PTRAP_SetOplRing() registers the home before trap install
   (AU_init at main.c:501 precedes Prepare_RM_PortTrap at :524).

**Measured result (MI1 soak, saturation meter 4FC/4FD = IRQ0 polls/tick):**
idle ~1; MI1 normal passages 8-12 (PIT ~350-500Hz) with ZERO heals; dense
iMUSE crescendos still spike to 54-72 with heal churn (engine SURVIVES:
futile stays 0, ticks always resume, game keeps running — vs. the old
terminal wedge). Pre-ring the same passages hit 65-77 and 250+ futile
heals with a dead engine.

**The measured floor**: the ring killed the RMCB leg, so the residual tax
is the trap ENTRY itself — QPIEMU's iocb does Begin_Nest_Exec +
Simulate_Far_Call + Resume_Exec per trapped OUT (~tens of µs on a DX4/75);
iMUSE peaks at ~14k OUTs/s. No ring fixes that.

**Gotcha found late (crash class, FIXED)**: _SB_InstallISR builds the
real-mode SB vector as PSPseg:(dosheap − PSP) — dosheap MUST stay inside
the PSP's 64K. Moving it to the DOS block armed a garbage INT 0Fh vector
system-wide (DOOM/MI2 crashed at the first injected SB interrupt; MI1
"sometimes crashed" via spurious IRQ7). The SB-ISR stub now goes back to
PSP:60h — free again since rmcode1 moved out.

**Duke3D SETUP AdLib test: HARD-WEDGES the machine (agent dead, no dump) —
OPEN, suspected ring-code bug, NOT explained by CPU saturation.** The
contradiction that keeps it open: pre-ring this test ran "slow+quiet" and
SURVIVED on a strictly more expensive build. Duke/AUDIOLIB's profile
differs from SCUMM's in ways the newest paths haven't been stressed by:
~250-reg init sweep at full speed (ring-full boundary + sync fallback
churn), second-bank 0x38A/B writes, timer-family writes interleaved
mid-sweep (drain-from-trap-context path), ~35 delay reads per reg write.
Next session: reproduce with the ring telemetry build (4FE occupancy
high-water / 4FF drain count) sampling live through the test. Also
MEASURE the per-trap cost properly (time 10k in-stub 0x388 reads vs 10k
untrapped-port reads on the box) — the "~40µs" in this doc is an
estimate, and that number sizes the JLM payoff.

**DOOM + AdLib music (PM world): confirmed OUT OF ENVELOPE, not a
regression.** Bench crash dump (2026-08-14 pt.2): Exception 0D in ring 0
inside HDPMI (CS base FF800000), IRQ10 in service, HDPMI-internal 4K-limit
stack with ESP wrapped negative. Mechanism: DMX's timer handler fires ~14k
PM OPL traps/s; each trap entry has a short window on HDPMI's small
internal stack before SwitchStackIO switches away, and the 43Hz codec IRQ
eventually lands inside it → nested frame blows the 4K stack → ring-0
GPF. Rate × window ≈ a collision within seconds ("bombs after init").
This is the same pre-existing "DOOM with FM music" death, differently
dressed. DOOM SFX-only (music off) remains the supported daily-driver
config and is verified healthy on this build.

## Telemetry (BIOS IAC 0x4F0, live-read only — 755C clears IAC on WARM boot)

4F0 depth · 4F1 phase (1 claim / 2 getpos / 3 write / 4 exit) · 4F2/3 tick
count u16 · 4F4 reenter count · 4F5 last SR at claim · 4F6 heal count ·
4F8/9 last getpos u16 · 4FA consecutive futile heals · 4FB depth
high-water · 4FC IRQ0 polls/tick (last) · 4FD polls/tick high-water (the
saturation meter). Read over COMrade mid-wedge (mem_read 0x4F0).
Real slave PIC readable live: OUT 0xA0,0x0B then IN 0xA0 = ISR (0x0A = IRR)
— 0xA0 is now trapped in the TP755 build but byte accesses pass through
(VPIC_PassAcc / stub QPI sim), so the probe still reads REAL hardware;
0x20/0x21/0xA1 remain shadowed. A WORD in from 0xA0 gets the VPIC shadow
in the high byte (the decomposer routes it) — real IMR stays unreadable
from V86, infer via guardian behavior. Never issue disk ops over COMrade
while the slave PIC is suspect (IRQ14 dead → agent captive in INT 13h).

## Later / separate projects

- **VSBHDA-as-JLM (ring-0 port handlers)**: THE identified next chapter
  for V86 FM on 486s. Registering port handlers natively with Jemm (like
  QPIEMU does via Install_IO_Handler, but pointing at OUR ring-0 code
  instead of a nested-exec v86 callback) collapses per-trap cost to
  #GP+decode+call. Kills the crescendo saturation the write ring can't.
  Big lift: VSBHDA's trap layer would grow a JLM-resident component.
- **ADLIBEMU: assessed 2026-08-14 pt.3 and CANCELLED.** Float in the hot
  loop AND pow() per key-on — dead on the FPU-less half of the fleet
  (PC110/OB530/OB425), i.e. exactly the boxes the fleet-FM play targets;
  on 486 x87 timings it plausibly LOSES to dbopl anyway. Successor plan:
  **de-FP dbopl** (its render is already all-integer; only InitTables'
  pow/log10/sin, Setup's double scale, and vopl3.cpp's defensive
  fpu_save stand between it and an SX/33) + bench; emu8950 (MIT,
  integer, PicoGUS-proven FPU-less) is the fallback core.
- Jemm Simulate_IO raw-split regression (≥5.84) — candidate UPSTREAM Jemm
  report (pre-5.84 re-trapped split bytes; the raw split is what let the
  word OUT reach the real slave IMR at all).
- FM mix level: Duke setup reported FM "quiet" — default ADLG/midivol
  balance worth a look once FM is stable.

## Bench session pt.4 (2026-08-14, 755C live) — what changed on this branch

- **OPLGEN build knob**: `CARD=TP755 OPLGEN=TABLELOG|HANDLER ./tools/build.sh`
  → vsbpcmtl/vsbpcmth.exe (dbopl.h DBOPL_WAVE now #ifndef-overridable;
  OPLDEF rides CPPFLAGS only). Default build stays byte-identical
  (226488/3342316118 verified after every edit). Fixed an UPSTREAM DOSBox
  bug en route: TABLELOG GetWave's `( wave & 0x7fff ) + vol << shift`
  shifts the SUM (precedence); parenthesised, host-probe-verified (unfixed:
  half RMS, 12.5% samples crushed to 0; fixed: tracks TABLEMUL within
  0.6%). Dead code upstream (DOSBox ships TABLEMUL) — report-worthy.
  **On-box verdict: TABLELOG ≈ TABLEMUL at 11025** (same saturation
  numbers, MI1). Generator choice is a TABLE-SIZE question for the 8KB-L1
  fleet (HANDLER: 1.9KB bss vs 9.1/9.4KB), not a speed question.
- **OPL write ring 256→1024 entries** (word head/tail + mask in stub and
  ptrap.c, DOS home 46→146 paras). The 256 ring MEASURED FULL (occupancy
  HW 255/255, MI1) = peak writes falling back to sync traps. Post-fix
  peaks: MI1 268, MI2 ~108, ring-full events 0 (new stub counter at IAC
  4F7). Also: stub (186B) was 6 bytes from outgrowing its 192B slot and
  corrupting ring entry 0 — ring moved to offset 256 + install-time guard
  (overrun → ring disarmed, sync path, loud printf).
- **🏆 Futile-heal root cause, live-probed on a real MI2 wedge: the CS4248
  IGNORES status-register writes while PEN is on.** The guardian's re-arm
  was a no-op in precisely the stuck-INT state → 8 futile heals → dormant
  → wedge matured into game crash. Fix: **verified ack** (write SR, read
  back, if INT still latched bounce PEN around the ack). Do NOT gate the
  bounce on ISR depth — the real wedge parked 2 SNDISR frames (depth
  stuck at 2) and a depth==0 gate reproduced the death; worst case of
  bouncing under parked frames is IAR left at 0x09 once. **Tier-2** added:
  slave re-ICW (0x11/70/02/01 + boot-stashed IMR) at futile>=4 for the
  ISR=00/IRR=00/codec-begging state — but it's inert WITHOUT the ack
  (edge-triggered PIC can't see a stuck-high line; 5 tier-2 rounds cured
  nothing until the ack landed first). IAC 4FA = futile lo-nibble |
  tier-2 hi-nibble now.
- **MI2 envelope verdict**: with all of the above, MI2's intro plays end to
  end (choppy + slow video = trap-entry saturation, engine ~2-3× slow-mo,
  ~450 first-round-curing heals), then the GUEST dies on its own — Jemm
  exc 06, wild jump into FF bytes in the game arena; iMUSE broken by
  prolonged time distortion. ESC recovers to prompt, no reboot; guardian
  went futile→tier2→dormant cleanly post-crash (ring hit true-full during
  the death throes). Old build: terminal wedge + reboot. MI1 = survives
  with stutter; MI2 = plays-then-crashes; smooth dense FM = the JLM
  chapter, nothing driver-side is left.
- Deployed: VSBPCMTL.EXE 431300/2820968711 + GOTL.BAT / GOTH.BAT
  (optional DACRATE arg). Ops: crashed guest can refuse file_write —
  reboot first; real 0x20/0x21/0xA1 are UNREACHABLE over COMrade (shadow),
  so master-PIC state is only visible to the driver's UntrappedIO.
