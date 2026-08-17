# Implementation plan: CARD_AUDIGY on interrupt-dead CardBus hosts (560X, X60s)

## A. VERDICT on the TI route

**The recipe exists, is Linux-proven (it was written for exactly this failure on TI ThinkPads), and the 560X bench has NOT actually tried it yet — the single load-bearing register bit was never poked.** But it remains board-wiring-dependent and unverifiable fleet-wide, so it is a bench experiment, not the shipping path. The timer pump is primary (see C).

First, a premise correction from the research: the 560X bridge is a **TI PCI1250/1250A** (PCI ID 104c:ac16), not a 1130/1131. The 113x register map still mostly applies (0x92/0x91/ExCA are common), with two 1250-specific quirks noted below.

### Definitive bit meanings (Linux ti113x.h / i82365.h, cross-checked vs FreeBSD pccbbreg.h + TI PCI4410A manual)

**0x92 Device Control (PCI config, 8-bit)** — interrupt mode = bits 2:1 (mask 0x06):
- `0x00` = parallel PCI interrupts only (no ISA path)
- `0x02` = parallel ISA IRQ pins + parallel PCI
- `0x04` = serialized ISA IRQ (IRQSER pin) + parallel PCI
- `0x06` = everything serialized
(Russell King caveat: encodings vary per TI part; the PCI4410A "1130-compat" table confirms this layout for the 12xx generation.)

**0x91 Card Control (8-bit)**: `0x80` RIENB, `0x40` ZVENABLE, `0x20` PCI_IRQ_ENA (master), `0x10` PCI_IREQ (functional int → PCI), `0x08` PCI_CSC (CSC → PCI), `0x02` SPKROUTEN, `0x01` IFG (write-1-to-clear).

**0x3E Bridge Control (PCI config, 16-bit), bit 7 (0x0080) = CB_BRIDGE_INTR**: SET = CardBus functional interrupt (CINT) routed per the ExCA registers (i.e., to the ISA IRQ named in ExCA 0x03 bits 3:0); CLEAR = routed to PCI INTA#. Generic CardBus-bridge-spec, so it applies to the 1250.

**ExCA 0x03 (INTCTL)** bits 3:0 = ISA IRQ number for the functional interrupt (bit 4 must stay clear — on TI it means CSC→PCI). **ExCA 0x05 (CSCINT)** bits 7:4 = ISA IRQ for CSC.

### Why each blind poke failed

- `0x92=0x62` (orig): IMODE already = `0x02` parallel ISA — **the BIOS left the chip in the correct mode**. Failure wasn't here.
- `0x92=0x60`: IMODE `0x00` = no ISA path at all. Strictly worse; guaranteed fail.
- `0x92=0x64` + PIIX4 `0x64=0xD0`: serialized mode + PIIX4 SERIRQ decode enabled (continuous mode). Only works if the 1250's IRQSER pin is physically wired to the PIIX4 SERIRQ line — unknown, and Linux notes the 1250 family has no MFUNC3 IRQSER config, so serial support details differ. Inconclusive, likely unwired.
- `0x91` orig `0x83` = RIENB|SPKROUTEN|IFG: **all three PCI-routing bits (0x38) already clear** — the BIOS config expects ISA delivery. Writing `0xBB` (setting 0x20|0x10|0x08) was the *wrong direction* for the goal, and the readback `0xA2` (0x10/0x08 didn't latch, 0x01 W1C-cleared) is evidence the 1250 does not implement the 113x per-class routing bits — on this part, don't trust 113x names beyond what a probe verifies.
- `ExCA 0x03 = 0x0B`: functional nibble = IRQ 11, bit 4 clear — the correct *half* of the recipe.
- **Never poked: Bridge Control 0x3E bit 7.** Without CB_BRIDGE_INTR set, a CardBus card's CINT never consults the ExCA nibble — it goes to PCI INTA# regardless of IMODE. On the 1250, INTA additionally rides GPIO3 (config 0x8B, mode bits 7:6 must be 00), which was never checked — so the CINT very likely went to a GPIO-mode pin, i.e., nowhere. This is exactly the 760ED story ("PCI irq pin not connected... Cardbus IRQ has to be routed to an ISA irq", kernel commit 0d3a940de51c).

### Corrected recipe (exact pokes, in order) — and how to verify without the card

1. `0x92 = (val & ~0x06) | 0x02` (already true).
2. `0x91`: ensure bits 0x38 clear (already true); write `0x01` to clear a stale IFG.
3. Probe phase, per candidate IRQ n in {11, 10, 9, 5, 7} (exclude 3/4 = serial/COMrade link, 8 = our RTC, 12 = TrackPoint, 14/15 = IDE): `ExCA 0x05 = (n<<4)|0x01` (CSC on n, STSCHG enable), then force a fake CSC by writing `0x00000001` (CSTSCHG) to SOCKET_FORCE at MMIO base+0x0C (0xD000C on the bench), then read the PIC IRR (OCW3 0x0A to 0x20/0xA0) for bit n. This is Linux's `ti113x_use_isa_irq` probe verbatim — it answers "is ANY parallel ISA pin wired on this board" in minutes, with no card involvement. If no hit, also set SOCKET_MASK bit 0 and retry once.
4. On a hit at n: `ExCA 0x03 = (val & ~0x1F) | n`; **`0x3E |= 0x0080`**; clear probe state (`ExCA 0x05 = 0`, read ExCA 0x04 to W1C, write the event back to SOCKET_EVENT); unmask n at the PIC; card_irq = n. Card side keeps the normal interrupt path (INTENABLE = INTE_SAMPLERATETRACKER, CLIEL bit 2, LOOPINT dummy voice).

Supporting evidence the pins are wired: 16-bit PC Card IRQs on the 560X in DOS use the same ExCA nibble → same pins. If 16-bit cards ever delivered IRQs on this box, step 3 will find a live line.

## B. Timer-pump design (primary, shipping path)

Vector = **RTC IRQ8 / INT 70h**, because that is what this fork has already hardware-proven on four ES1688/VEW211 boxes: `_SND_InstallISR` is vector-agnostic; guest re-hooks of INT 70h are virtualized (`_Snd_Notify21/31`); VPIC refuses guest masking of IRQ8; `PTRAP_Prepare` keeps the slave-PIC trap for irq >= 8; `PIC_UnmaskIRQ` handles cascade. IRQ0/PIT is disqualified: the PIT is guest hardware, games reprogram ch0 and hook vector 8 constantly (the documented VEWPLAY failure).

The Audigy engine is uniquely suited: pacing is **position-based** (`AU_cardbuf_space` reads the real DMA read pointer `CCCA_CURRADDR` vs `card_dmalastput`), not IRQ-count-based — it runs correctly from any tick source at any rate where tick period < buffer depth.

All changes in `mpxplay/sc_sbliv.c` under `#ifdef CARD_AUDIGY`, plus one call-site divider in `src/sndisr.c`. Copy the RTC helpers locally (~60 lines, modeled on sc_es1688.c:630-663 + 716-734 — they are static there; do not de-static).

1. **`SBALL_adetect`** (sc_sbliv.c:1725): if `AUDTIMER=1` → `aud_timer=1; aui->card_irq = 8;` parse `SBERTC=3..15` for the RTC rate select (default 6 = 1024 Hz; 7 = 512 Hz for CPU headroom). This alone re-points the vector hook (sndisr.c:913), VPIC protection (main.c:553), slave trap (ptrap.c:581), and IRQ8+cascade unmask (main.c:637).
2. **`SBALL_start`** (1821-1842): in timer mode write `EMU10K_INTENABLE = 0` (skip line 1836), never set `CLIEL` and don't start dummy voice 2 (1289-1293) — with both clear, INTA# never asserts and there is no level-triggered storm risk on the dead PCI line. Then `rtc_enable()` (reg A rate | reg B PIE) and install the **chained** IRQ0 heartbeat (`DPMI_InstallISR(0x08, ..., TRUE)`, kill-switch env like ESNOI8).
3. **`SBALL_IRQRoutine`** (1914-1920): timer path = stamp own tick counter, run watchdog, cli-guarded RTC ack (`outportb(0x70,0x0C); inportb(0x71)`), optional belt-and-braces IPR drain (read, write back if nonzero, skip the CLIPL branch), **return 1 unconditionally** — a 0 return chains SNDISR into the BIOS INT70 handler (sndisr.c:336). `PIC_SendEOI(8)` at sndisr.c:853 already EOIs both PICs.
4. **stop/close**: keep the RTC armed across `SBALL_stop` (stop fires on every rate change — killing the pump there deadlocked the ES backend); `rtc_disable()` + IRQ0 unhook only in close.
5. **Watchdog**: the VEW211 **verify-before-revive** design (sc_vew211.c:427-464), never the blind reg-C re-arm (a blind reg-C read steals the pending tick). Hosts = the IRQ0 chain + the irq_routine; RTC seconds register as guest-proof wall clock; all timing from the own tick counter, never 0x46C. Not optional — guests silently disarm the RTC periodic (Theme Hospital class).
6. **`EMUWT_Poll` cadence** (sndisr.c:836-839): envelope steps hard-assume ~83 Hz (~12 ms/tick, emu_wt.c:355) — at 1024 Hz fades run ~12x too fast. Divide: run the body every N ticks, N = RTC_HZ*12/1000 (N=12 at 1024 Hz, N=6 at 512 Hz). `VMPU_Process_Messages` stays every tick (finer MIDI timing — strictly better).
7. **PIT-reprogramming games**: no interaction by construction — the timebase is RTC reg A (ours, crystal-exact); the IRQ0 heartbeat is pass-through-chained at whatever rate the game sets and derives no timing from it.

## C. What to implement FIRST for the 560X bench

**Implement the timer pump first** (B), gated on `AUDTIMER=1`. Rationale: it is the guaranteed path — independent of board wiring, it exercises the entire Audigy render/wavetable stack on the 560X immediately, and it is a transplant of a recipe already proven on four boxes. It also covers the X60s (Ricoh RL5c476 II + ICH7 — no parallel-ISA option exists there at all; its alternatives are ICH7 PIRQ-router pokes or the pump).

**In parallel, time-boxed: run the corrected ISA probe from A as a standalone tool** (small DOS .EXE run via run_command — one command per run, no `&&`), no driver changes required. It is ~10 minutes of scripted CSC-force pokes. If it finds a live IRQ, add an `AUDIRQ=n` env path later (bridge reroute from A step 4 + `card_irq = n`, normal interrupt-driven engine unchanged). Benefit of the real IRQ if it works: ISR runs at LOOPINT rate (~83 Hz) instead of 512-1024 Hz — meaningful CPU savings on a P-MMX 233.

**235 stays untouched by construction**: without `AUDTIMER`/`AUDIRQ` the code path is byte-identical to today's working build. Do NOT ship auto-detect tomorrow — a mis-probing autodetect could regress the one working box. Auto-detect is phase 3, after both paths are individually bench-proven, and should run **before `SNDISR_Init`** (synchronous, no vector hook): force a CSC routed at the BIOS PCI IRQ, poll PIC IRR with interrupts masked ~50 ms; hit → interrupt mode; miss → ISA-probe sweep; miss → timer pump. (Probing at adetect/card_init time avoids the mess of hot-switching a hooked vector.)

## D. Open risks

- **1250 register semantics**: the 0xBB→0xA2 readback proves the part diverges from the 113x header; also King's warning that IMODE encodings vary per part. Mitigation: the CSC-force probe *measures* delivery instead of trusting bit tables.
- **Reentrancy at 1024 Hz with SETIF=1**: every tick runs the full mixer+writedata tail (no `pt_took` early-exit). The `es_in_render` guard is compiled into the AUDIGY build and is card-agnostic; STACKCHECK backstops at 16 levels; fallback is `SBERTC=7/8` (position pacing tolerates it by design). Measure CPU headroom on the 560X.
- **EMUWT divider correctness**: wrong N = audibly wrong envelope fades. Verify against the active Audigy-wavetable handoff bench (coordinate the sndisr.c call-site change with that work to avoid a diff collision).
- **RTC death by guests**: covered by watchdog, but the verify-before-revive logic is the newest code in the fleet — VEW211-proven, Audigy-unproven.
- **IRQ pool hygiene on the ISA route**: never probe/claim IRQ 3/4 (COMrade link — same class of self-inflicted wound as the COM1 I/O-window rule), 8 (pump), 12 (TrackPoint), 14/15 (IDE).
- **Ordering with the Audigy wake-up quirk**: I/O reads hard-hang until the BAR+0x38 wake-up writes run — ensure `INTENABLE=0` is written after wake-up but before any unmask in timer mode.
- **Dual-socket 1250**: second socket is a separate PCI function with its own ExCA set — probe and program the socket the card is actually in.
- **X60s specifics**: if the pump also misbehaves there, next poke is the ICH7 LPC PIRQ route registers (D31:F0 config 0x60-0x63, bit 7 = disable) — but the pump should make that moot.
- **Probe side effects**: forced CSC events must be fully cleaned up (ExCA 0x05 = 0, ExCA 0x04 read-to-clear, SOCKET_EVENT write-back) or a stale STSCHG enable will fire spurious IRQs during audio.

Reference sources: local copies of Linux ti113x.h, yenta_socket.c/.h, i82365.h, cardbus.c.
