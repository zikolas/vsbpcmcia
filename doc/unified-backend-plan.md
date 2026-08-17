# Unifying the backends into one VSBPCM.EXE — assessment (2026-08-15)

Goal: one binary, runtime card selection, retiring the per-card
builds/branches. Assessed against the actual trees, not memory.

## What actually has to merge

Only TWO lines, not four: `audigy-cardbus` already CONTAINS `vew211-backend`
in full (vew is its direct ancestor, 15 commits behind). So the merge is:

    main ─┬─ vew211-backend ── audigy-cardbus     (line A: vew + audigy)
          └─ tp755-backend                        (line B: tp755 + FM stack)

Caveat on line A: its base is 9 commits behind main, and main took the
stereo fix (caba345) as a CHERRY-PICK of vew-line work -- expect duplicate
hunks in sc_es1688.c / vsb.c when line A merges up. One careful conflict
session, not a redesign.

Mechanical conflict surface between the two lines is small: AU_CARDS.C's
card table (#elif chain, both sides edit it), CONFIG.H card blocks, small
additive hooks in VSB.C / SNDISR.C / MAIN.C. PTRAP.C was rewritten by the
tp755 line (word decompose, OPL ring, 0xA0 trap) but line A only touched 14
lines of it. djgpp.mak / build.sh trivially.

## The three real architectural jobs

1. **De-singleton the passthrough ABI.** sndisr.c calls `ES1688_PT_Space` /
   `ES1688_PT_Feed` (+Watchdog/FullReset) as bare externs; sc_es1688,
   sc_vew211 and sc_tp755 EACH define them ("compile-time exclusive" by
   design -- sc_tp755's are stubs). In one binary they collide at link.
   Fix: promote the PT entry points into per-card ops (function pointers in
   or beside `sndcard_info_s`), sndisr dispatches through the active card.
   Same treatment for the TP755-only hooks (vsb.c DSP-reset heal, virq
   revive-squelch gate, sndisr depth limiter) -- runtime `if card` instead
   of `#ifdef CARD_TP755`. This is THE core refactor; everything else hangs
   off it. The ifdef inventory to convert: ptrap.c 13 sites, sndisr.c 3,
   vsb.c 2, au_cards.c 2, virq/rmcode1/ptrap.h 1 each.

2. **Probe safety + ordering.** Today each card is "tried first" because
   it's the only one compiled. Unified probe order must be safe on every
   box: sc_tp755's detect POKES the ThinkPad control port (0x15E8 idx 0x1C)
   blind -- on a non-ThinkPad that port could be anything (the fleet lesson:
   I/O windows are landmines). Needs a machine gate before the poke (755
   planar ID / BIOS model check), conservative ordering (PCMCIA passthrough
   backends detect via already-enabled cards at BLASTER/SBEBASE ports =
   safe; planar pokes = gated; PCI/CardBus scan = flagged), plus a user
   override (/CARD=xxx or reuse /DEV). Small code, real design care.

3. **FM becomes runtime -- and first gets 90% cheaper.** NOFM is compile
   time today (ptrap skips the 0x388 traps entirely). Measured tonight: the
   FM build costs ~200KB of EXE, and the synthesis is almost NONE of it --
   dbopl.o is 19KB; the rest is the DJGPP C++ runtime + libm dragged in by
   ONE `new DBOPL::Chip` and init-time pow/log10/sin. The already-planned
   de-FP dbopl work (precomputed tables, fixed-point Setup, static Chip
   instead of new) kills libm AND libstdc++ -> FM-in-every-build lands
   around +30KB text, floor-viable even for the PC110/OB425 tenancy wall.
   The SX-floor plan and the unification plan are the same work item.
   Runtime side: upstream already has the /OPL flag machinery; default it
   per-card (TP755 on, ES1688-class off -> real ESFM stays king), make the
   0x388 trap install conditional at runtime.
   The rmcode1 stub is already mostly runtime-armed (rseg=0 = ring off,
   wPICp=0xFFFF = PIC trap off) -- one universal stub with the 0x388 fast
   path assembled in costs a few compares per trap on non-FM cards.

## What stays OUT of the first unified binary

- **Audigy/CardBus + emu_wt hardware wavetable**: keep build-flagged.
  Only the 235 has CardBus, it needs PCIBIOS + bridge init (cbinit), the
  wavetable campaign is still an active handoff, and VMPU must flip on for
  it. Fold in last, or keep as the one remaining special build.
- The **16-bit OW build**: untouched, as now.

## Debts to land or freeze BEFORE merging (they conflict textually)

- VEW211 R-first frame swap (pending on that backend since the stereo fix).
- FMVOL set: on vew line only; decide if /FMVOL generalizes (it pokes real
  OPL3 through the traps -- meaningless for TP755's emulated OPL; runtime-
  gate per card).
- tp755 open bugs that touch ptrap/rmcode1 (Duke-setup wedge) -- land
  fixes on the branch first or accept re-porting them.

## Sequencing (each step shippable)

1. Integration branch off main; merge line A (eat the cherry-pick dups),
   then line B. Still CARD-flag builds -- ONE TREE, drift stops. [1 session]
2. PT-ABI ops table + de-ifdef the shared files. Builds still per-card;
   byte-identity invariant replaced by per-box bench regression. [1-2]
3. Probe table + machine gates + /CARD override -> first true unified
   binary (ES1688 + VEW211 + TP755). [1-2 + a bench pass on 3 box families]
4. dbopl slim (de-FP + static chip) -> FM compiled in everywhere, /OPL
   runtime; universal rmcode1 stub. [1-2 + SX-floor bench]
5. v0.7 release, retire vew211-backend/tp755-backend branches; audigy
   remains the one special build until its campaign concludes.

Bench cost is the honest bulk: every step 3+ needs SBDIAG + game spot
checks per box family (235, PC110, T2130CT, 755C, OB425/530), since the
"default build byte-identical to v0.6" safety net dies by definition.
