/* EMU10K2 hardware wavetable -- SF2 soundfonts played on the card's own
 * sample-playback voices, so the CPU does no synthesis at all.
 *
 * The card has no onboard sample memory: the DSP fetches from host RAM through
 * the page table at PTB. So "loading" a soundfont means putting its sample pool
 * in DMA-able memory and pointing page-table entries at it, after which each
 * voice is just a set of registers describing where to read, how fast, and how
 * loud.
 *
 * The driver plays PCM on voices 0-2; we use 3..31. Not 3..63 -- the sound ISR
 * only ever services CLIPL, so a voice above 31 that raised a loop interrupt
 * would latch a pending bit nothing clears.
 */
#ifndef EMU_WT_H
#define EMU_WT_H

struct emu10k1_card;

/* foreground only (driver init): loads the font and maps it into the card's
 * page table. Returns 1 on success. */
int  EMUWT_Init(struct emu10k1_card *card, const char *sf2path);
void EMUWT_Exit(void);
int  EMUWT_Active(void);

/* MIDI-side entry points. Safe to call from a port trap / ISR: no printf, no
 * allocation, no FPU. */
void EMUWT_NoteOn(int ch, int key, int vel);       /* vel 0 == note off */
void EMUWT_NoteOff(int ch, int key);
void EMUWT_ProgramChange(int ch, int prog);
void EMUWT_ControlChange(int ch, int cc, int val);
void EMUWT_PitchBend(int ch, int bend14);          /* 0..16383, 8192 centre */
void EMUWT_ChannelPressure(int ch, int val);
void EMUWT_SetBankPreset(int ch, int bank, int prog);
void EMUWT_SetMasterVolume(unsigned int q15);
void EMUWT_Reset(void);
void EMUWT_AllNotesOff(void);

/* once per sound interrupt: steps release fades, reaps finished voices */
void EMUWT_Poll(void);

/* Milestone-1 probe: a generated tone on one voice, no soundfont involved.
 * Kept because it isolates "is the sample path alive" from "is the soundfont
 * parsed right". */
void EMUWT_Selftest(struct emu10k1_card *card);

/* Audible bring-up test: plays a scale and a chord from the loaded font. */
void EMUWT_Demo(int bank, int prog);

#endif /* EMU_WT_H */
