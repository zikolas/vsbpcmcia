@echo off
rem Wavetable stack: Audigy 2 ZS Notebook, SF2 soundfont on the EMU10K2's
rem own hardware voices. Configure games: Music = General MIDI, port 330;
rem Sound = Sound Blaster A220 I7 D1. OPL emulation is OFF (/OPL0): this
rem card has no hardware OPL and the DBOPL render was the big ISR cost --
rem AdLib-only titles need GOAUD /P330 /OPL1 instead.
rem
rem   GOWT [font.sf2]      font in C:\VSBPCM, default TIMGM6MB.SF2
rem
rem One stack per boot: reboot before launching again.
rem MIDI smoke test afterwards:  DOSMID /mpu=330 C:\CANYON.MID
set AUDSF2=C:\VSBPCM\TIMGM6MB.SF2
if not "%1"=="" set AUDSF2=C:\VSBPCM\%1
call C:\VSBPCM\GOAUD.BAT /P330 /OPL0
