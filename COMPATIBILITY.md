# Game compatibility

Results below are from real hardware: IBM PC110 (486SX/33), Toshiba T2130CT
(486DX4), IBM ThinkPad 755C (486DX4), Toshiba Libretto 20 (Am5x86/75),
HP OmniBook 425 (486SLC/25, via HP CardBIOS Socket Services), HP OmniBook 530
(486SX/33, via SystemSoft Card Services), and IBM ThinkPad 235 (166MHz Pentium
MMX), with Panasonic KXL-C101 and Ratoc REX-5571/5572 cards enabled by ES1688GO. As of v0.6, SBPro stereo output is supported
with authentic SBPro channel polarity -- games' Reverse Stereo settings
behave as they would on a real SB Pro.
"Works" means digital + FM audio in normal play, not exhaustive testing.
Game version can matter. Reports welcome -- open a compatibility-report
issue with your game, machine, card and symptoms.

| Game | Status | Tested on | Notes |
|---|---|---|---|
| DOOM | ✅ | 486SX/33, 486DX4, P-MMX, 486SLC/25 | instant SB detect; runs but slow on the 486SLC/25 — use mono (`/T3`), see performance notes below |
| DOOM II | ✅ | 486DX4 | |
| Duke Nukem 3D | ✅ | 486DX4 | |
| Wolfenstein 3D | ✅ | 486DX4, 486SLC/25 | |
| Epic Pinball | ✅ | 486SX/33, 486DX4 | |
| Prince of Persia | ✅ | 486 | |
| Heretic | ✅ | 486 | |
| Rise of the Triad | ✅ | 486 | |
| The Lion King | ✅ | 486DX4 | |
| SimCity 2000 | ✅ | 486DX4 | |
| Dune II | ⚠️ | 486DX4, P-MMX | fine on the P-MMX. On the DX4 it loads and plays, but moving the mouse halts it with a DOS internal stack overflow — the same signature as Cannon Fodder's setup, and it survives `STACKS=32,512`; untriaged |
| Quake | ✅ | Am5x86/75 (Libretto 20) | |
| Raptor: Call of the Shadows | ✅ | P-MMX | |
| Impulse Tracker | ✅ | P-MMX | tracker/editor, SB digital out |
| Open Cubic Player 2.6.0pre6 | ✅ | P-MMX | keep the UI in 80x25 (`screentype=0` in BOTH `[screen]` and `[fileselector]` of cp.ini) -- dense 80x50 text modes redraw slowly enough to starve CP's own mixer (warbly, slightly slow audio); put `devpSB` first in `playerdevices` so it uses the emulated SB via BLASTER |
| Theme Hospital | ✅ | 486DX4, P-MMX | native ESFM music (stereo) with ES1688GO 1.4+ |
| Warcraft II | ✅ | P-MMX | native ESFM ("ESFM Enhanced") with ES1688GO 1.4+ |
| Tomb Raider | ✅ | P-MMX | |
| Sam & Max Hit the Road (talkie) | ❌ | 486SX/33 | crashes in the HDPMI DPMI host itself (also without VSBPCMCIA loaded); runs on a bare boot |
| Pinball Fantasies | ❌ | 486SX/33, 486DX4 | graphics corruption, then a wedge, on the DX4 (freeze on load on the SX/33). Isolated to VSBPCMCIA: with ES1688GO alone and the game set to AdLib — no VSBPCMCIA loaded — it runs clean with no corruption |
| Duke Nukem II | ✅ | 486DX4 | needs the SB-base FM alias fix (v0.6.1-pre1+): its SB probe runs an AdLib timer test at base+8 and never touches the DSP unless it passes. Also put the emulated SB **below** the real chip (`ES1688GO /SB=240` + `VSBPCM /A220`) — it scans 0x220 first, and with the real card there it selects real silicon, which has no DMA on a PCMCIA socket |
| Earthworm Jim 2 | ⚠️ | 486SX/33 | audio distortion; suspected CPU limit, untested on faster machines |
| Flashback | ❌ | 486DX4 | wedges to a flashing cursor in-game; untriaged, not yet retested without VSBPCMCIA loaded |
| Cannon Fodder | ❌ | 486DX4, P-MMX | the setup's sound test dies in a repeating ring-0 exception 0Dh inside the DPMI host (faults on `mov gs, ss:[esi]`, byte-identical registers every run). Not the FM alias fix — the pre-fix binary faults identically. `/CF4` stops the fault repeating but not the fault itself; untriaged |
| Worms | ❌ | P-MMX | not yet triaged |

## Measured performance (DOOM v1.2 `-timedemo demo1`, 1667 gametics, VSBPCM v0.6)

HP OmniBook 425 (486SLC/25, KXL-C101, 6MB RAM):

| configuration | realtics | fps | audio share of CPU |
|---|---|---|---|
| no sound | 9,966 | 5.9 | — |
| SB 2.0 mono (`/T3`) | 17,480 | 3.3 | 43% |
| SB Pro stereo (default) | 26,520 | 2.2 | 62% |

IBM ThinkPad (486DX2-50):

| configuration | realtics | fps | audio share of CPU |
|---|---|---|---|
| no sound | 3,140 | 18.6 | — |
| SB 2.0 mono (`/T3`) | 4,171 | 14.0 | 25% |

Audio cost scales almost linearly with the output byte rate, so on very slow
CPUs the levers that matter are the emulated card type and the rate: running
the emulation as a mono SB 2.0 (`VSBPCM /T3` plus `BLASTER=... T3`) halves
the byte rate and bought ~50% more frames on the SLC, at the cost of stereo
panning. `DACRATE=11025` is the other half of the same lever. The resident
is already size-optimized (`-Os`) and the passthrough adds no per-sample
processing — below this class of CPU, the remaining costs are the game's own
mixing and the port-trap round trips, which no configuration removes.
Across machines the absolute audio cost falls faster than clock speed alone
suggests (the DX2-50's mono audio bill is ~7x smaller than the SLC/25's):
the cost is I/O- and memory-shaped, so bus width and cache matter as much
as MHz.

FM-only ESS-native players (e.g. ESFMPLAY) probe the BLASTER variable:
point it at the REAL chip base (`SET BLASTER=A220 I5 D1`) for such tools --
the emulated base carries no FM.
