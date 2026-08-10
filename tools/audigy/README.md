# Audigy 2 ZS Notebook bench tools (DOS, real mode)

Companions to the `CARD=AUDIGY` driver build. Their chip knowledge derives
from the Linux ALSA `snd-emu10k1` driver, so they live here under the same
GPL-2.0 as the rest of this tree (see `copying`).

| tool | what it does |
| --- | --- |
| `cbinit` | the `BAR+0x38` chip wake-up, standalone (driver does this itself) |
| `fxvol` | FX-engine master volume (GPR 8/9), live |
| `dacvol` | Wolfson WM8768 DAC volume over SPI, live |
| `audmix` | interactive mixer: MAIN (DAC) / MASTER / WAVE / MIDI sliders + mute |

`audmix`'s WAVE/MIDI faders need this driver's DSP program (group GPRs 10/11,
wavetable on FX buses 4/5). All are live-tuning tools: a driver reload rewrites
its own defaults.

Build on the box with OpenWatcom: `C:\WATCOM\BLD.BAT <name>` (16-bit real
mode, C89).
