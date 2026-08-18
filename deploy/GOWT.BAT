@echo off
rem General MIDI on the EMU10K2 hardware wavetable (see goaudigy.bat).
rem Configure games: Music = General MIDI port 330, Sound = SB A220 I7 D1.
rem
rem   gowt [font.sf2] [extra VSBPCMA args]
rem
rem Font lives in C:\VSBPCM, default TIMGM6MB.SF2. OPL emulation is OFF here;
rem AdLib-only titles want goaudigy.bat instead. One stack per boot.
rem Smoke test afterwards:  DOSMID /mpu=330 C:\CANYON.MID
set AUDSF2=C:\VSBPCM\TIMGM6MB.SF2
if not "%1"=="" set AUDSF2=C:\VSBPCM\%1
call C:\VSBPCM\GOAUDIGY.BAT /P330 /OPL0 %2 %3 %4
