# Should VSBPCMCIA use VDPMI too?

Asked alongside the 16-bit protected-mode build, 2026-08-23. Short answer:
**not for the 16-bit goal — it cannot serve it. There is a real but modest
case for it as a third trap backend for the existing 32-bit binary on
Pentium-class boxes, and that case is worth deferring.**

Everything below is either read out of source we have (`~/sbemu-vdpmi`'s
`vdpmi.c`, this tree, the HDPMI-era MPUSHIM work) or is bench evidence from
the MPUSHIM 0.4 session on the ThinkPad 235. Where it is neither, it says so.

---

## What VDPMI actually is

crazii's DPMI host with its own V86 monitor and virtual PIC. Three things
matter here, and only the first two are documented anywhere — the vendor API
is not in `VDPMI.TXT`; its shape is read off SBEMU's own client driver
(`vdpmi.c`, GPL v2, (C) crazii), which is what MPUSHIM 0.4 did.

1. **One I/O-trap table for V86 *and* ring-3 protected mode.** In `vdpmi.c`
   the table is `VDPMI_IOTrapTable[port]` — indexed by port, with no
   client-type dimension and no range limit. One registration, whole
   catalogue, one resident host instead of three (JEMM386 + QPIEMU +
   HDPMI32i).
2. **A virtual PIC**: `VDPMI_RaiseIRQ(irq)` (vendor fn 7) injects an IRQ
   *asynchronously*; VDPMI delivers it to whichever client is running, framed
   correctly for that client's type.
3. `VDPMI_InstallISR` for the real card IRQ, `VDPMI_GetPhysicalAddr` for DMA.

**Bench fact (235, MPUSHIM 0.4):** one VDPMI registration covered all three
trap worlds — V86, 16-bit PM and 32-bit PM — with DOOM, Duke3D, MI1 and
DOSMID all playing. So the trap really is client-agnostic. That is the
strongest thing in VDPMI's favour and it is not in doubt.

## Why it still cannot deliver the 16-bit build

Three independent reasons. Any one of them is fatal on its own.

### 1. VSBPCM's IRQ delivery is synchronous, and SBEMU's is not

This is the structural one, and it is easy to miss because MPUSHIM *did* work
under VDPMI. MPUSHIM is a facade over a UART — it has no IRQ to deliver.
VSBPCM does.

VSBPCM does **not** ask a host to raise the guest's SB IRQ. It hooks the
client's INT 31h (`src/int31.asm`), intercepts AX=0204h/0205h to *shadow* the
vector the guest sets (`currSBvec`), and then **far-calls that vector
synchronously** from inside its own sound ISR (`SBIsrCall` in `sbisr.asm`).
The tap loop in `sndisr.c` depends on this: it calls `VIRQ_Invoke()` and then
looks at `VSB_Running()` to see whether the guest re-armed *inside its own
ISR*. That contract is the entire subject of the current VEW211 short-SFX
campaign (the `PTDIAG` counters in `sndisr.c`).

SBEMU is built the other way round: `MAIN_InvokeIRQ` → `VDPMI_RaiseIRQ`, and
its own notes call that "the whole reason for this port". Asynchronous
injection is what makes VDPMI client-agnostic — and it is precisely what
VSBPCM's pacing cannot use without being rewritten around it.

So "add VDPMI and the two binaries collapse into one" is not available. The
part that forces two binaries is the INT 31h shadow and the synchronous call,
not the port trap, and VDPMI does not change either.

### 2. Pentium-only, and this project exists for the 486 fleet

VDPMI executes RDTSC and RDMSR unguarded. The boxes this driver was written
for — PC110, OmniBook 530/425, T2130CT, ThinkPad 755C, the VEW211 machines —
are 386SX/486. They can never run it. Whatever VDPMI gained would have to be
built *alongside* the HDPMI16i route, not instead of it, so the 16-bit work
in this session is needed either way.

### 3. VDPMI is at its *worst* on exactly the catalogue vsbpcm16 exists for

Bench-proven on the 235, one-variable control, no shim and no enabler
involved: under VDPMI alone, **Tyrian's setup and menus crawl** and only
moving gameplay is smooth. `/PVI=0` made no difference. 32-bit DOS/4GW static
menus (Duke3D SETUP) are fine, so it looks specific to 16-bit clients. That
is an upstream VDPMI bug, still unreported to crazii, and Tyrian is the
reference title for the whole 16-bit catalogue.

Building the 16-bit answer on the host that runs 16-bit clients worst would
be backwards.

## Where VDPMI *would* genuinely earn its place

Not for 16-bit. For the **existing 32-bit binary, on a Pentium**, as a third
provider inside `PTRAP.C` next to QPI and HDPMI:

* **One resident instead of three.** `JEMM386 + JLOAD QPIEMU + HDPMI32I`
  becomes `VDPMI /X=…`. Compare MPUSHIM's `GOVDP.BAT` with its `GOALL.BAT`.
* **The 8-range limit goes away.** `PTRAP.C` hard-codes
  `HDPMI_MAXRANGE 8` — *"hdpmi32i's support for port trapping is limited to 8
  ranges"* — and VSBPCM's `PortTable` uses **exactly 8**. The code already
  carries a fallback for ports that are trapped but unhandled because a whole
  range had to be over-trapped. There is no headroom: any new trapped port
  outside the current groups needs a ninth range today. VDPMI's per-port table
  has no such ceiling.
* Crucially, on a Pentium the client is still 32-bit, so **the synchronous
  `SBIsrCall` design keeps working unchanged**. Nothing about the IRQ core has
  to move. That is what makes this version of the idea cheap.

Sizing, if it is ever wanted: `PTRAP.C` already abstracts two providers with
the same shape (detect via a vendor entry, install per range, a trap handler
that decodes an error code). A third would follow `mpushim`'s `vdpmi_install`
almost line for line, and the handler contract is already written down in
`MPUSHIM.C`. Call it a contained change to one file plus a `/VDPMI` switch —
but it buys convenience and headroom, not capability.

## Costs to weigh against that

* **A second host path to keep alive forever.** The 486 fleet keeps QPI +
  HDPMI regardless, so this is strictly additive maintenance.
* **Bench friction:** under VDPMI the COMrade link dies whenever any DPMI
  client runs; typing `comrade` again revives it (no reboot). Under HDPMI the
  link survives games. That is a real tax on how this project is actually
  debugged.
* **Attribution.** VDPMI's vendor API is undocumented; any implementation is
  derived from `vdpmi.c` ((C) crazii, GPL v2). VSBPCMCIA is already GPL v2, so
  there is no licence problem — but it must be credited in the README exactly
  as MPUSHIM credits it, and that is a real obligation, not a footnote.

## Recommendation

1. **Ship the HDPMI16i route as the 16-bit answer.** It is the only one that
   works on the fleet, and `vsbpcm16.exe` now builds (`doc/16bit.md`).
2. **Report the VDPMI 16-bit-client slowdown upstream to crazii.** MPUSHIM
   0.4 has an excellent one-variable repro (bare Tyrian fine; VDPMI alone,
   nothing else loaded, menus crawl). This is owed regardless of what we do
   next, and if it gets fixed the picture changes.
3. **Park VDPMI-in-VSBPCM behind a `/VDPMI` switch as future work**, and
   revisit only when either (a) the 8-range ceiling actually blocks a new
   trapped port, or (b) someone wants a one-resident stack on a Pentium
   laptop. Do not start it as part of the 16-bit campaign — it shares no code
   with it and answers a different question.
