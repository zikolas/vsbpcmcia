 A few simple test programs

 TEST01: DSP cmd 0x14 (8-bit mono unsigned), zigzag
 TEST02: DSP cmd 0xB6 (16-bit mono unsigned autoinit), zigzag
 TEST03: DSP cmd 0xB6 (16-bit mono signed autoinit), sine
 TEST04: DSP cmd 0x14, uses EMS page frame as sound buffer
 TEST05: DSP cmd 0x14, a torrent of small single-cycle blocks -- the Duke
         Nukem II block pattern, for measuring the passthrough tap loop
 TEST06: DSP cmd 0x75 (4-bit ADPCM) and cmd 0x10 (direct-DAC) -- the two
         guest formats that BYPASS the tap, so the engine's render tail
         runs instead. Doubles as a reproducer for the unbounded render
         tail on backends that register no ISR depth hook.
 
 The assembly binaries can be created with JWasm, using its -mz option.
 
