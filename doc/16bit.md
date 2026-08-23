# VSBPCM16 — the 16-bit protected-mode build

**Status (2026-08-23, evening): WORKS ON HARDWARE.** Bench-verified on the
ThinkPad 235 + KXL-C101: **Tyrian** (Borland RTM, 16-bit PM — the reference
title) plays with digital SFX and AdLib FM both working and gameplay at full
speed; **Jazz Jackrabbit** runs clean throughout. Three port bugs were found
and fixed on the bench — all in this fork's deltas, none in upstream's
machinery — and are written up below. Known cosmetic wart: Tyrian's shell
(menus only, not the jukebox or gameplay) runs slightly slow; see the
open-items list.

---

## Why a second binary exists

DPMI 0.9 gives 16-bit and 32-bit clients **separate protected-mode interrupt
tables**, so a 32-bit client cannot provide services to a 16-bit one
(`vsbhda.txt:51-57`). `VSBPCM.EXE` therefore serves real-mode and 32-bit PM
games; 16-bit PM games — Tyrian and the rest of the Borland RTM / Phar Lap 286
/ DOS16M catalogue — need a client that is itself 16-bit. That is
`VSBPCM16.EXE`. Both serve real-mode games. **It is not a size optimisation.**

Concretely, the blocker is in `src/int31.asm`. VSBPCM hooks the client's
INT 31h and intercepts AX=0204h/0205h so it can *shadow* the SB IRQ vector the
guest sets (`currSBvec` in `sbisr.asm`) rather than let the guest install it.
It then delivers the virtual SB IRQ by **far-calling `cs:[currSBvec]`
synchronously** from inside its own sound ISR (`SBIsrCall`). A 32-bit
resident's INT 31h hook lives in the 32-bit table, so a 16-bit game's DPMI
calls never reach it — VSBPCM never learns the vector, and could not build the
right (16:16) call frame for it if it did.

On the box the difference is two words:

```
32-bit PM            16-bit PM
-----------------------------------
HDPMI32I -r -x -v    HDPMI16I -r -x -v
VSBPCM               VSBPCM16
```

`deploy/go16es.bat` and `deploy/go16vew.bat` are the launchers.
**Run `UNINST.EXE` before switching between the two** — neither reliably
detects the other (`vsbhda.txt:76`).

## "16-bit" is about the client type, not the code

The generated code is still 32-bit: `.386`, USE32 segments,
`startup/cstrt16x.asm` is literally headed *"DOS 32-bit startup code for
16-bit client"*. What is 16-bit is the **DPMI client type** — the host is
entered through its 16:16 API entry, so interrupt frames are words, not
dwords. `src/config.inc` switches `insIRET`/`insPUSHF`/`SEGOFS`/`PFAR` on
`NOTFLAT` to match.

Two things people assume and get wrong:

* **DS is not limited to 64K.** `startup/init1632.asm` re-bases DGROUP and
  sets its limit to `0xFFFFFFFF` — *"for linear access, we need full 4GB
  descriptor"*. A near pointer wraps round to any linear address here exactly
  as it does under DJGPP, so `linear.h`'s `NearPtr` is the whole low-memory
  mechanism in both builds. No extra selector, no far pointers.
* **`_TEXT` is not limited to 64K either.** `init1632.asm` already sets
  `CSGT64K equ 1`. (Measured anyway: 48K code, 41K data — both fit.)

## One module, not two

Upstream ships `vsbhda16.exe` + `sndcard.drv` because its driver half is six
PCI drivers plus `ac97mix` and `pcibios`, and because that half only ever has
to answer the 14-entry `AUEXP` table (`src/auexp16.asm`) — a strictly one-way
interface with a stack switch and pointer translation on every call.

**Our backends are not shaped like that.** They reach *back* into the engine:
`PTOPS_Register`, `PTOPS_CardIs`, seven `SNDISR_*` entries,
`PTRAP_SetOplRing`, and the `FOpts` option block. Three of those —
`SNDISR_HasTsc`, `SNDISR_ReviveSquelch`, `FOpts` — are **data**, which no call
thunk can bridge. Splitting would mean inventing a reverse import table with
pointer translation for engine globals.

So `ow16.mak` links one module with the same object list as `djgpp.mak`, plus
the OW startup. `ldmod16` / `libmain` / `dstrt16x` / `auimp16` / `auexp16` are
unused. Keeping the object lists the same is also what will make the two
binaries comparable when the bench comes back.

## What the port actually consisted of

The engine was already dual-build — upstream maintains both makefiles. Every
DJGPP-only line was **ours**: the three PCMCIA backends and the telemetry
added to `sndisr.c`. All of it now goes through `src/hostsvc.h`:

| was | now |
|---|---|
| `_farpokeb(_dos_ds, 0x4F2, v)` | `LOW_PokeB(0x4F2, v)` — same absolute addresses |
| `_farpeekl(_dos_ds, 0x46C)` | `LOW_PeekD(0x46C)` |
| `inportb` / `outportb` | same names; `inp`/`outp` aliases under OW |
| GCC `__asm__` IF save/restore ×3 copies | `HOST_DisableInterrupt`/`HOST_CliSave` |
| GCC `__asm__` CPUID/TSC probe | `HOST_HasTsc()` |
| GCC `__asm__` DPMI 0100h | `HOST_DosAlloc()` |
| `_go32_dpmi_chain_protected_mode_interrupt_vector` | `src/pmisr.asm` |

`-za99` was needed for `main.c`, which declares after statements.

## The one piece with no portable ancestor

`_go32_dpmi_chain_protected_mode_interrupt_vector` is a DJGPP **libc**
facility, not a DPMI call: go32 allocates a trampoline that saves registers,
loads the app's selectors, calls the C function, and then jumps to the handler
that was there before. Open Watcom has nothing like it. The backends use it
for the **IRQ0 watchdog heartbeat** — the guest-independent host that
resurrects a killed RTC within ~110 ms (Theme Hospital kills it for 6.5 s at
a stretch).

`src/pmisr.asm` replaces it with a fixed pool of four trampolines, each
assembled with its slot index as a literal (CS is not writable in protected
mode, so a trampoline cannot patch itself and one shared trampoline cannot
tell which vector it was entered for). Its structure is deliberately
`stackisr.asm`'s `SwitchStackISR`, which is the proven dual-build PM interrupt
handler in this tree: private ISR stack via `__djgpp_stack_top`, our DS
fetched from a `cs:`-relative word, `insIRET` or a far `jmp` to the saved
`PFAR`. The `0204h` store order (dword then word at `+SEGOFS`) is
`sbisr.asm`'s, verbatim.

**Statically verified from the JWasm listing** — both modes, 0 warnings:

```
NOTFLAT   66 CF                iret       (16-bit IRET — correct for a 16-bit client frame)
          66 2E FF 2D ....     jmp far [cs:pm_old]   (16:16)
flat      CF                   iretd
          2E FF 2D ....        jmp far [cs:pm_old]   (16:32)
```

That is as far as static checking goes. What it does **not** prove is whether
the 16-bit host's entry stack, the shared `__djgpp_stack_top` switch, and the
chain hand-off behave on hardware.

## The 32-bit binary was not disturbed

With no bench, the only safety property available was "the shipping binary
still generates the same code", so it was measured rather than assumed. A
pre-port tree was reconstructed (it rebuilds to the shipping
`sha256 6ce34ce3…` exactly) and every object compared instruction by
instruction with symbols, literals and branch targets masked — `tools/cmp32.sh`.

* `sc_es1688.o`, `sc_vew211.o`, `sndisr.o` — **identical instruction
  streams**; only a static's *name* changed (`DPMI_DisableInterrupt` →
  `HOST_DisableInterrupt`).
* `sc_tp755.o` — same function set; gcc allocated differently around the
  now-inlined DPMI 0100h helper. `TP755_adetect` gains 4 instructions and
  `TP755_close` pushes from memory instead of an immediate. **Init and
  teardown only** — checked function by function, the ISR and render paths are
  instruction-for-instruction identical. (`TP755_getbufpos` and
  `tp755_pt_ops` show up in the tool's attribution only because the padding
  after them disassembles as junk and shifts with the object.)
* Everything else — unchanged.

The default 32-bit build does not compile `hostsvc.c` or `pmisr.asm` at all;
under DJGPP every service in `hostsvc.h` is a per-file static, exactly as the
three private copies were.

---

# BENCH RESULTS (2026-08-23) — the plan below was run; keep for the next port

The ladder was executed in order and each step earned its place.

### Step 1: PMISR on the proven 32-bit stack — PASSED

`PMISR=1 ./tools/build.sh` → `vsbpcmp.exe` survived a full DOOM timedemo
(653 gametics / 496 realtics) with the trampolines chained on IRQ0 and the
iret slot on IRQ5 — including **one genuine RTC death healed through the
trampoline** (`0x4F4` revive count ticked mid-demo). Run this A/B again after
any change to `pmisr.asm`.

### Step 2/3: GO16 + Tyrian — PASSED, after three real bugs

Each crash below was in OUR port code; upstream's machinery never faulted.

1. **`AU_init` retf #GP** (first boot). `au_cards.h` declares the `AU_*`
   entries `_far` for upstream's exe+drv split; the one-module build calls
   them near, so the far return popped a bogus CS (the TSS selector, err
   code 0030). Fix: the `ONEMODULE` define — near entries, and the
   sndcard-side `bOMode` fixups in `au_cards.c` compile out (`main.c` owns
   them in a one-module build).

2. **Hard hang at the first RTC tick** (second boot). The compat layer's
   `DPMI_DisableInterrupt` went through platform.h's `_disable_ints`, which
   far-calls `cs:[oldint31]` — a pointer `_InstallInt31` only fills at
   main.c:747. The RTC pump and IRQ0 heartbeat tick from `AU_init`
   (main.c:616), and `irq_routine` disables interrupts on EVERY tick, so the
   first tick in that window far-called through NULL inside an ISR: hard
   hang, no exception report. Fix: raw `int 31h` 0900h/0901h pragmas in
   `hostsvc.h` — the DJGPP original's exact semantics, no ordering
   dependency. (The engine's `oldint31` route is correct FOR THE ENGINE:
   upstream installs every ISR before anything can tick.)

3. **"VSBPCMCIA: fatal error 3"** (third boot). Our own STACKCHECK — which
   upstream ships `equ 0` — computes `bottom = top - 64K`. The OW startup's
   STACK segment is 16K inside a ~41K DGROUP, so the subtraction UNDERFLOWS,
   the bound wraps to ~0xFFFFxxxx, and the check trips on the FIRST
   SwitchStackISR entry. Telemetry proved SNDISR had never run once — a
   false alarm. Fix: under NOTFLAT the bottom is `_STACKLOW`, the startup's
   real stack floor. Sub-lesson: an `externdef` declared INSIDE `.code`
   associates with CS and the reference gets a CS prefix (#GP past the CS
   limit) — declare at file top.

The STKDIAG rig built for bug 3 (`DIAG=1 ./tools/build16.sh`) stays: it
converts a STACKCHECK trip into a survivable, telemetry-readable event
(mask IRQ8, EOI both PICs, marker `0x4FE=F3`, unwind, iret) and adds PMISR
depth probes at `0x4FC/0x4FD` (which collide with the engine's TSC duration
probe — trust them only on no-TSC boxes).

### Tuning found on the bench

* **`SET SBEPTLAT=60`** (in the GO16 launchers): the engine's 250ms
  passthrough default put an audible gap between keypress and SFX; 60ms
  fixes it. The 32-bit ES1688 launchers still run the default — fold in at
  the next fleet touch.
* Under load: PT feeds 16k+, ring ~2.7KB, ISR depth pinned at 1, no
  re-entries, no stack trips, guest reconfigs tracked. Clean exits.

### Open items

* **Tyrian shell slowdown** (menus only; jukebox + gameplay full speed;
  Jazz unaffected): a per-wait-event cost, not CPU starvation — the busiest
  shell scene is the *correct*-speed one. Mirrors the VDPMI bug's shape
  (also Tyrian-shell-specific), much milder. Staged one-variable
  experiments, env-only: `SET SBERTC=8` (pin the pump, no adaptive ramp)
  and `SET SBENOI8=1` (take our trampoline out of Tyrian's INT 8 music
  chain — prime suspect). `SBENOI5=1` exists too.
* **Idle-only RTC revive churn**: the watchdog "revives" the RTC every
  ~1.5s when no audio flows (froze during gameplay; 32-bit GOP: 1-2 per
  DOOM demo). Heals cleanly; find the liveness-compare misfire someday.
* Real-mode games under GO16 unchecked (both binaries serve RM).
* VEW211 (`deploy/go16vew.bat`) and TP755 16-bit paths built, unbenched.
  TP755's raw cli under an IOPL-0 16-bit host is untested theory.
* **Next box: the 486SX fleet** (PC110 + the same KXL-C101 first). Expect
  `SBERTC` pinning to become mandatory and `SBEPTLAT` to need loosening
  (~100-120ms); nothing in the port assumes a Pentium (CPUID probe fails
  safe, PMISR is 386+, NOFM keeps the FPU out).
* Under GO16, 32-bit titles get no emulation (DOS/4GW falls back to Jemm's
  VCPI, outside the PM traps) — expected, switching worlds = reboot +
  the 32-bit launcher. `UNINST.EXE` between worlds remains untested.

### Known unknowns that resolved

* `main.c`'s NOTFLAT TSR path: ran fine.
* The `mov ss/esp` switch under a 16-bit host stack: fine (as
  upstream-proven form suggested).
* `PMISR_SLOTS=4`: two in use (IRQ5 iret, IRQ0 chain), headroom stands.
