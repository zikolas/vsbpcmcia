# Game compatibility

Results below are from real hardware: IBM PC110 (486SX/33), Toshiba T2130CT
(486DX4), IBM ThinkPad 755C (486DX4), Toshiba Libretto 20 (486), IBM
ThinkPad 235 (Pentium MMX), with Panasonic KXL-C101 and Ratoc REX-5571/5572
cards enabled by ES1688GO. As of v0.6, SBPro stereo output is supported
with authentic SBPro channel polarity -- games' Reverse Stereo settings
behave as they would on a real SB Pro.
"Works" means digital + FM audio in normal play, not exhaustive testing.
Game version can matter. Reports welcome -- open a compatibility-report
issue with your game, machine, card and symptoms.

| Game | Status | Tested on | Notes |
|---|---|---|---|
| DOOM | ✅ | 486SX/33, 486DX4, P-MMX | instant SB detect |
| DOOM II | ✅ | 486DX4 | |
| Duke Nukem 3D | ✅ | 486DX4 | |
| Wolfenstein 3D | ✅ | 486DX4 | |
| Epic Pinball | ✅ | 486SX/33, 486DX4 | |
| Prince of Persia | ✅ | 486 | |
| Heretic | ✅ | 486 | |
| Rise of the Triad | ✅ | 486 | |
| The Lion King | ✅ | 486DX4 | |
| SimCity 2000 | ✅ | 486DX4 | |
| Dune II | ✅ | P-MMX | |
| Quake | ✅ | 486 (Libretto 20) | |
| Raptor: Call of the Shadows | ✅ | P-MMX | |
| Impulse Tracker | ✅ | P-MMX | tracker/editor, SB digital out |
| Theme Hospital | ✅ | 486DX4, P-MMX | native ESFM music (stereo) with ES1688GO 1.4+ |
| Warcraft II | ✅ | P-MMX | native ESFM ("ESFM Enhanced") with ES1688GO 1.4+ |
| Tomb Raider | ✅ | P-MMX | |
| Sam & Max Hit the Road (talkie) | ❌ | 486SX/33 | crashes in the HDPMI DPMI host itself (also without VSBPCMCIA loaded); runs on a bare boot |
| Pinball Fantasies | ⚠️ | 486SX/33, 486 | game freezes (on load on the SX/33); not yet triaged |
| Duke Nukem II | ❌ | 486 | game's SB detection fails; not yet triaged |
| Earthworm Jim 2 | ⚠️ | 486SX/33 | audio distortion; suspected CPU limit, untested on faster machines |
| Cannon Fodder | ❌ | P-MMX | not yet triaged |
| Worms | ❌ | P-MMX | not yet triaged |

FM-only ESS-native players (e.g. ESFMPLAY) probe the BLASTER variable:
point it at the REAL chip base (`SET BLASTER=A220 I5 D1`) for such tools --
the emulated base carries no FM.
