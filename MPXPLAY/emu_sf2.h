/* SoundFont 2.x reader -- container walk, zone resolution, generator merge.
 *
 * Deliberately knows nothing about the EMU10K2, DOS or the driver: it takes a
 * pair of read/seek callbacks and hands back merged generator sets. That way
 * the fiddly half (the preset->instrument->sample chain, where a wrong offset
 * produces plausible-sounding noise rather than an error) can be built and
 * diffed against a reference implementation on a host machine, and only the
 * register translation has to be debugged over a serial cable.
 *
 * C89, integer only -- no libm, and none of this may run in FPU context.
 */
#ifndef EMU_SF2_H
#define EMU_SF2_H

#include <stdint.h>

#define SF2_GEN_MAX     61      /* generators 0..60, 60 = endOper */

/* the generators we actually act on */
#define SF2_GEN_STARTADDRO       0
#define SF2_GEN_ENDADDRO         1
#define SF2_GEN_STARTLOOPADDRO   2
#define SF2_GEN_ENDLOOPADDRO     3
#define SF2_GEN_STARTADDRCOARSE  4
#define SF2_GEN_MODLFOTOPITCH    5
#define SF2_GEN_VIBLFOTOPITCH    6
#define SF2_GEN_MODENVTOPITCH    7
#define SF2_GEN_INITIALFILTERFC  8
#define SF2_GEN_INITIALFILTERQ   9
#define SF2_GEN_ENDADDRCOARSE    12
#define SF2_GEN_PAN              17
#define SF2_GEN_DELAYVOLENV      33
#define SF2_GEN_ATTACKVOLENV     34
#define SF2_GEN_HOLDVOLENV       35
#define SF2_GEN_DECAYVOLENV      36
#define SF2_GEN_SUSTAINVOLENV    37
#define SF2_GEN_RELEASEVOLENV    38
#define SF2_GEN_KEYNUMTOVOLHOLD  39
#define SF2_GEN_KEYNUMTOVOLDECAY 40
#define SF2_GEN_INSTRUMENT       41
#define SF2_GEN_KEYRANGE         43
#define SF2_GEN_VELRANGE         44
#define SF2_GEN_STARTLOOPCOARSE  45
#define SF2_GEN_KEYNUM           46
#define SF2_GEN_VELOCITY         47
#define SF2_GEN_INITIALATTEN     48
#define SF2_GEN_ENDLOOPCOARSE    50
#define SF2_GEN_COARSETUNE       51
#define SF2_GEN_FINETUNE         52
#define SF2_GEN_SAMPLEID         53
#define SF2_GEN_SAMPLEMODES      54
#define SF2_GEN_SCALETUNING      56
#define SF2_GEN_EXCLUSIVECLASS   57
#define SF2_GEN_OVERRIDEROOTKEY  58
#define SF2_GEN_ENDOPER          60

/* sfSampleType bits */
#define SF2_SMPL_MONO       1
#define SF2_SMPL_RIGHT      2
#define SF2_SMPL_LEFT       4
#define SF2_SMPL_LINKED     8
#define SF2_SMPL_ROM        0x8000

/* sampleModes */
#define SF2_LOOP_NONE       0
#define SF2_LOOP_CONT       1
#define SF2_LOOP_UNUSED     2
#define SF2_LOOP_SUSTAIN    3   /* loop while held, then play through to end */

typedef struct sf2_io_s {
	void *h;
	/* both return 1 on success, 0 on failure */
	int (*read)(void *h, void *buf, uint32_t len);
	int (*seek)(void *h, uint32_t off);
} sf2_io;

typedef struct {
	sf2_io    io;
	uint8_t  *pdta;         /* whole pdta LIST body, allocated by sf2_open */
	uint32_t  pdta_len;
	/* views into pdta -- counts INCLUDE the terminal record */
	const uint8_t *phdr; int n_phdr;
	const uint8_t *pbag; int n_pbag;
	const uint8_t *pgen; int n_pgen;
	const uint8_t *inst; int n_inst;
	const uint8_t *ibag; int n_ibag;
	const uint8_t *igen; int n_igen;
	const uint8_t *shdr; int n_shdr;
	uint32_t  smpl_off;     /* file offset of the raw 16-bit sample pool */
	uint32_t  smpl_bytes;
	int       ver_major, ver_minor;
} sf2_file;

/* One articulated zone = one hardware voice. Generator values are merged
 * (instrument absolute, preset additive); sample addresses are already
 * offset-corrected and absolute in the smpl pool, counted in SAMPLES. */
typedef struct {
	int16_t  gen[SF2_GEN_MAX];
	uint32_t start, end, loopstart, loopend;
	uint32_t sample_rate;
	int      root_key;          /* overridingRootKey, else shdr originalPitch */
	int      pitch_correction;  /* cents, from shdr */
	uint16_t sample_type;
	uint16_t sample_link;
	int      sample_index;
	int      inst_index;
} sf2_zone;

/* xalloc/xfree let the caller pick the heap: plain malloc on a host, the DPMI
 * block allocator inside the driver. Returns 1 on success. */
int  sf2_open(sf2_file *sf, const sf2_io *io,
              void *(*xalloc)(uint32_t), void (*xfree)(void *));
void sf2_close(sf2_file *sf, void (*xfree)(void *));

int  sf2_preset_count(const sf2_file *sf);          /* excludes terminal EOP */
int  sf2_find_preset(const sf2_file *sf, int bank, int prog);   /* -1 if none */
void sf2_preset_info(const sf2_file *sf, int pidx,
                     char name[21], int *bank, int *prog);
void sf2_sample_name(const sf2_file *sf, int sidx, char name[21]);
void sf2_inst_name(const sf2_file *sf, int iidx, char name[21]);

/* Resolve (preset index, key, velocity) into up to maxout voices.
 * Returns the number written. */
int  sf2_resolve(const sf2_file *sf, int pidx, int key, int vel,
                 sf2_zone *out, int maxout);

#endif /* EMU_SF2_H */
