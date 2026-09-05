 A few simple test programs

 TEST01: DSP cmd 0x14 (8-bit mono unsigned), zigzag
 TEST02: DSP cmd 0xB6 (16-bit mono unsigned autoinit), zigzag
 TEST03: DSP cmd 0xB6 (16-bit mono signed autoinit), sine
 TEST04: DSP cmd 0x14, uses EMS page frame as sound buffer
 TEST05: FM synthesizer test
 TEST06: MIDI synthesizer test
 TEST07: DSP cmd 0x91 (8-bit mono unsigned, highspeed), zigzag
 TEST08: DSP cmds 0x75/0x74 (4-bit ADPCM single-cycle with/without ref byte)
 TEST09: DSP cmd 0x7D (4-bit ADPCM autoinit)
 CVRATE: to test the resampling part of VSBHDA
 PTBLKS: DSP cmd 0x14, a torrent of small single-cycle blocks -- the Duke
         Nukem II block pattern, for measuring the passthrough tap loop
         (VSBPCMCIA; was TEST05 before the VSBHDA 2.0 merge)
 PTBYPASS: DSP cmd 0x75 (4-bit ADPCM) and cmd 0x10 (direct-DAC) -- the two
         guest formats that BYPASS the tap, so the engine's render tail
         runs instead. Doubles as a reproducer for the unbounded render
         tail on backends that register no ISR depth hook.
         (VSBPCMCIA; was TEST06 before the VSBHDA 2.0 merge)
 
 The assembly binaries can be created with JWasm, using its -mz option.
 
