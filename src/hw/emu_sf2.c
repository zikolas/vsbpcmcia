/* SoundFont 2.x reader -- see emu_sf2.h.
 *
 * Records are read byte at a time on purpose. Overlaying a packed struct is
 * the classic way to break this: gcc pads sfPresetHeader to 40 and sfSample to
 * 48, after which every subsequent record reads garbage that still looks like
 * plausible sample offsets, and the font just sounds subtly wrong.
 */

#include "emu_sf2.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ---------------------------------------------------------------- readers */

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
	     | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int fourcc_eq(const uint8_t *p, const char *s)
{
	return p[0] == (uint8_t)s[0] && p[1] == (uint8_t)s[1]
	    && p[2] == (uint8_t)s[2] && p[3] == (uint8_t)s[3];
}

static void copy_name(const uint8_t *src, char dst[21])
{
	int i;
	for (i = 0; i < 20; i++)
		dst[i] = (char)src[i];
	dst[20] = '\0';
	/* names need not be NUL-terminated in the file; trim trailing blanks */
	for (i = 19; i >= 0 && (dst[i] == ' ' || dst[i] == '\0'); i--)
		dst[i] = '\0';
}

/* record sizes */
#define SZ_PHDR 38
#define SZ_PBAG  4
#define SZ_PGEN  4
#define SZ_INST 22
#define SZ_IBAG  4
#define SZ_IGEN  4
#define SZ_SHDR 46

/* field accessors -- offsets are from the SF2.04 spec, section 7 */
#define PHDR_BANK(p)    rd16((p) + 22)
#define PHDR_PROG(p)    rd16((p) + 20)
#define PHDR_BAGNDX(p)  rd16((p) + 24)
#define INST_BAGNDX(p)  rd16((p) + 20)
#define BAG_GENNDX(p)   rd16((p) + 0)
#define GEN_OPER(p)     rd16((p) + 0)
#define GEN_AMOUNT(p)   rd16((p) + 2)

/* ------------------------------------------------------------ generators */

/* Everything not listed is 0. Note keyRange/velRange default to the full
 * 0..127 span encoded as lo | (hi<<8). */
static void gen_defaults(int16_t *g)
{
	int i;
	for (i = 0; i < SF2_GEN_MAX; i++)
		g[i] = 0;
	g[SF2_GEN_INITIALFILTERFC] = 13500;
	g[21] = -12000;                 /* delayModLFO   */
	g[23] = -12000;                 /* delayVibLFO   */
	g[25] = -12000;                 /* delayModEnv   */
	g[26] = -12000;                 /* attackModEnv  */
	g[27] = -12000;                 /* holdModEnv    */
	g[28] = -12000;                 /* decayModEnv   */
	g[30] = -12000;                 /* releaseModEnv */
	g[SF2_GEN_DELAYVOLENV]   = -12000;
	g[SF2_GEN_ATTACKVOLENV]  = -12000;
	g[SF2_GEN_HOLDVOLENV]    = -12000;
	g[SF2_GEN_DECAYVOLENV]   = -12000;
	g[SF2_GEN_RELEASEVOLENV] = -12000;
	g[SF2_GEN_KEYRANGE] = (int16_t)0x7f00;
	g[SF2_GEN_VELRANGE] = (int16_t)0x7f00;
	g[SF2_GEN_KEYNUM]   = -1;
	g[SF2_GEN_VELOCITY] = -1;
	g[SF2_GEN_SCALETUNING]    = 100;
	g[SF2_GEN_OVERRIDEROOTKEY] = -1;
}

/* Generators that are illegal (and so ignored) at preset level: the sample
 * address offsets, the two range selectors, the terminators, and the handful
 * that make no sense as an offset. Everything else is added. */
static int preset_additive(int g)
{
	switch (g) {
	case SF2_GEN_STARTADDRO:      case SF2_GEN_ENDADDRO:
	case SF2_GEN_STARTLOOPADDRO:  case SF2_GEN_ENDLOOPADDRO:
	case SF2_GEN_STARTADDRCOARSE: case SF2_GEN_ENDADDRCOARSE:
	case SF2_GEN_STARTLOOPCOARSE: case SF2_GEN_ENDLOOPCOARSE:
	case SF2_GEN_KEYRANGE:        case SF2_GEN_VELRANGE:
	case SF2_GEN_KEYNUM:          case SF2_GEN_VELOCITY:
	case SF2_GEN_SAMPLEMODES:     case SF2_GEN_EXCLUSIVECLASS:
	case SF2_GEN_OVERRIDEROOTKEY:
	case SF2_GEN_INSTRUMENT:      case SF2_GEN_SAMPLEID:
	case SF2_GEN_ENDOPER:
		return 0;
	}
	return 1;
}

static int16_t clamp16(int32_t v)
{
	if (v > 32767)  return (int16_t)32767;
	if (v < -32768) return (int16_t)-32768;
	return (int16_t)v;
}

/* --------------------------------------------------------------- opening */

/* Walk a run of RIFF chunks looking for one id. Returns 1 and fills
 * off/len with the chunk's BODY when found. */
static int find_chunk(sf2_file *sf, uint32_t from, uint32_t to,
                      const char *id, uint32_t *off, uint32_t *len)
{
	uint8_t hdr[8];
	uint32_t cur = from;

	while (cur + 8 <= to) {
		if (!sf->io.seek(sf->io.h, cur))          return 0;
		if (!sf->io.read(sf->io.h, hdr, 8))       return 0;
		{
			uint32_t sz = rd32(hdr + 4);
			if (fourcc_eq(hdr, id)) {
				*off = cur + 8;
				*len = sz;
				return 1;
			}
			/* odd-sized chunks are padded to even */
			cur += 8 + sz + (sz & 1);
		}
	}
	return 0;
}

/* Locate a LIST of the given form type. Returns the body span AFTER the
 * 4-byte form id. */
static int find_list(sf2_file *sf, uint32_t from, uint32_t to,
                     const char *form, uint32_t *off, uint32_t *len)
{
	uint8_t hdr[12];
	uint32_t cur = from;

	while (cur + 12 <= to) {
		if (!sf->io.seek(sf->io.h, cur))           return 0;
		if (!sf->io.read(sf->io.h, hdr, 12))       return 0;
		{
			uint32_t sz = rd32(hdr + 4);
			if (fourcc_eq(hdr, "LIST") && fourcc_eq(hdr + 8, form)) {
				*off = cur + 12;
				*len = (sz >= 4) ? sz - 4 : 0;
				return 1;
			}
			cur += 8 + sz + (sz & 1);
		}
	}
	return 0;
}

/* Bind one pdta sub-chunk to a view inside the loaded blob. */
static int bind(sf2_file *sf, const char *id, int recsize,
                const uint8_t **ptr, int *count)
{
	uint32_t cur = 4;       /* skip the "pdta" form id */

	while (cur + 8 <= sf->pdta_len) {
		uint32_t sz = rd32(sf->pdta + cur + 4);
		if (fourcc_eq(sf->pdta + cur, id)) {
			if (cur + 8 + sz > sf->pdta_len)
				return 0;
			*ptr   = sf->pdta + cur + 8;
			*count = (int)(sz / (uint32_t)recsize);
			/* every list carries a terminal record; at minimum we need
			 * that one plus something to point at it */
			return (*count >= 1);
		}
		cur += 8 + sz + (sz & 1);
	}
	return 0;
}

int sf2_open(sf2_file *sf, const sf2_io *io,
             void *(*xalloc)(uint32_t), void (*xfree)(void *))
{
	uint8_t hdr[12];
	uint32_t end, off, len;

	sf->io = *io;
	sf->pdta = NULL;
	sf->pdta_len = 0;
	sf->smpl_off = sf->smpl_bytes = 0;
	sf->ver_major = sf->ver_minor = 0;

	if (!sf->io.seek(sf->io.h, 0))       return 0;
	if (!sf->io.read(sf->io.h, hdr, 12)) return 0;
	if (!fourcc_eq(hdr, "RIFF") || !fourcc_eq(hdr + 8, "sfbk"))
		return 0;
	end = 8 + rd32(hdr + 4);

	/* INFO/ifil -- version, informational only */
	if (find_list(sf, 12, end, "INFO", &off, &len)
	    && find_chunk(sf, off, off + len, "ifil", &off, &len) && len >= 4) {
		uint8_t v[4];
		if (sf->io.seek(sf->io.h, off) && sf->io.read(sf->io.h, v, 4)) {
			sf->ver_major = rd16(v);
			sf->ver_minor = rd16(v + 2);
		}
	}

	/* sdta/smpl -- the 16-bit sample pool. sm24 (the low bytes of a 24-bit
	 * font) is deliberately ignored: the EMU fetches 16-bit. */
	if (!find_list(sf, 12, end, "sdta", &off, &len))
		return 0;
	if (!find_chunk(sf, off, off + len, "smpl", &sf->smpl_off, &sf->smpl_bytes))
		return 0;

	/* pdta -- pull the whole thing into memory, it is small (a few hundred
	 * kB even for a large font) and every lookup indexes into it */
	if (!find_list(sf, 12, end, "pdta", &off, &len))
		return 0;
	/* re-include the form id so bind() can walk from a known base */
	sf->pdta_len = len + 4;
	sf->pdta = (uint8_t *)xalloc(sf->pdta_len);
	if (!sf->pdta)
		return 0;
	sf->pdta[0] = 'p'; sf->pdta[1] = 'd'; sf->pdta[2] = 't'; sf->pdta[3] = 'a';
	if (!sf->io.seek(sf->io.h, off)
	    || !sf->io.read(sf->io.h, sf->pdta + 4, len)) {
		xfree(sf->pdta);
		sf->pdta = NULL;
		return 0;
	}

	if (!bind(sf, "phdr", SZ_PHDR, &sf->phdr, &sf->n_phdr)) goto fail;
	if (!bind(sf, "pbag", SZ_PBAG, &sf->pbag, &sf->n_pbag)) goto fail;
	if (!bind(sf, "pgen", SZ_PGEN, &sf->pgen, &sf->n_pgen)) goto fail;
	if (!bind(sf, "inst", SZ_INST, &sf->inst, &sf->n_inst)) goto fail;
	if (!bind(sf, "ibag", SZ_IBAG, &sf->ibag, &sf->n_ibag)) goto fail;
	if (!bind(sf, "igen", SZ_IGEN, &sf->igen, &sf->n_igen)) goto fail;
	if (!bind(sf, "shdr", SZ_SHDR, &sf->shdr, &sf->n_shdr)) goto fail;

	/* every list is terminated by a sentinel record, so a usable font needs
	 * at least two of each of the ones we walk */
	if (sf->n_phdr < 2 || sf->n_inst < 2 || sf->n_shdr < 2)
		goto fail;

	return 1;
fail:
	xfree(sf->pdta);
	sf->pdta = NULL;
	return 0;
}

void sf2_close(sf2_file *sf, void (*xfree)(void *))
{
	if (sf->pdta) {
		xfree(sf->pdta);
		sf->pdta = NULL;
	}
}

/* ------------------------------------------------------------- accessors */

int sf2_preset_count(const sf2_file *sf)
{
	return sf->n_phdr - 1;          /* drop the terminal EOP record */
}

int sf2_find_preset(const sf2_file *sf, int bank, int prog)
{
	int i;
	for (i = 0; i < sf->n_phdr - 1; i++) {
		const uint8_t *p = sf->phdr + (long)i * SZ_PHDR;
		if (PHDR_BANK(p) == (uint16_t)bank && PHDR_PROG(p) == (uint16_t)prog)
			return i;
	}
	return -1;
}

void sf2_preset_info(const sf2_file *sf, int pidx,
                     char name[21], int *bank, int *prog)
{
	const uint8_t *p;
	if (pidx < 0 || pidx >= sf->n_phdr) {
		if (name) name[0] = '\0';
		if (bank) *bank = -1;
		if (prog) *prog = -1;
		return;
	}
	p = sf->phdr + (long)pidx * SZ_PHDR;
	if (name) copy_name(p, name);
	if (bank) *bank = PHDR_BANK(p);
	if (prog) *prog = PHDR_PROG(p);
}

void sf2_sample_name(const sf2_file *sf, int sidx, char name[21])
{
	if (sidx < 0 || sidx >= sf->n_shdr) { name[0] = '\0'; return; }
	copy_name(sf->shdr + (long)sidx * SZ_SHDR, name);
}

void sf2_inst_name(const sf2_file *sf, int iidx, char name[21])
{
	if (iidx < 0 || iidx >= sf->n_inst) { name[0] = '\0'; return; }
	copy_name(sf->inst + (long)iidx * SZ_INST, name);
}

/* ------------------------------------------------------------ resolution */

/* Look one generator up inside a zone's generator run. Returns 1 if present,
 * and the raw 16-bit amount. */
static int zone_gen(const uint8_t *genlist, int from, int to, int oper,
                    uint16_t *amount)
{
	int i;
	for (i = from; i < to; i++) {
		const uint8_t *g = genlist + (long)i * 4;
		if (GEN_OPER(g) == (uint16_t)oper) {
			*amount = GEN_AMOUNT(g);
			return 1;
		}
	}
	return 0;
}

static int in_range(const uint8_t *genlist, int from, int to,
                    int glob_from, int glob_to, int oper, int v)
{
	uint16_t a;
	if (!zone_gen(genlist, from, to, oper, &a)) {
		if (glob_to <= glob_from
		    || !zone_gen(genlist, glob_from, glob_to, oper, &a))
			return 1;                       /* unrestricted */
	}
	return v >= (int)(a & 0xff) && v <= (int)((a >> 8) & 0xff);
}

static void apply_absolute(int16_t *g, const uint8_t *genlist, int from, int to)
{
	int i;
	for (i = from; i < to; i++) {
		const uint8_t *p = genlist + (long)i * 4;
		uint16_t oper = GEN_OPER(p);
		if (oper < SF2_GEN_MAX)
			g[oper] = (int16_t)GEN_AMOUNT(p);
	}
}

/* Resolve a bag range into (global zone span, first non-global zone index).
 * A zone is global when it carries no terminator generator; only the first
 * zone of a list may be one. */
static void split_global(const uint8_t *bag, const uint8_t *genlist,
                         int bag_from, int bag_to, int term_oper,
                         int *glob_from, int *glob_to, int *first)
{
	uint16_t dummy;
	int gf = 0, gt = 0;

	*first = bag_from;
	if (bag_from < bag_to) {
		int from = BAG_GENNDX(bag + (long)bag_from * 4);
		int to   = BAG_GENNDX(bag + (long)(bag_from + 1) * 4);
		if (!zone_gen(genlist, from, to, term_oper, &dummy)) {
			gf = from;
			gt = to;
			*first = bag_from + 1;
		}
	}
	*glob_from = gf;
	*glob_to   = gt;
}

int sf2_resolve(const sf2_file *sf, int pidx, int key, int vel,
                sf2_zone *out, int maxout)
{
	int pbag_from, pbag_to, pz, n = 0;
	int pglob_from, pglob_to, pfirst;

	if (pidx < 0 || pidx >= sf->n_phdr - 1 || maxout <= 0)
		return 0;

	pbag_from = PHDR_BAGNDX(sf->phdr + (long)pidx * SZ_PHDR);
	pbag_to   = PHDR_BAGNDX(sf->phdr + (long)(pidx + 1) * SZ_PHDR);
	if (pbag_to > sf->n_pbag - 1) pbag_to = sf->n_pbag - 1;

	split_global(sf->pbag, sf->pgen, pbag_from, pbag_to,
	             SF2_GEN_INSTRUMENT, &pglob_from, &pglob_to, &pfirst);

	for (pz = pfirst; pz < pbag_to && n < maxout; pz++) {
		int pg_from = BAG_GENNDX(sf->pbag + (long)pz * 4);
		int pg_to   = BAG_GENNDX(sf->pbag + (long)(pz + 1) * 4);
		uint16_t instid;
		int ibag_from, ibag_to, iz;
		int iglob_from, iglob_to, ifirst;
		int16_t pacc[SF2_GEN_MAX];
		uint8_t pset[SF2_GEN_MAX];
		int i;

		if (!zone_gen(sf->pgen, pg_from, pg_to, SF2_GEN_INSTRUMENT, &instid))
			continue;                       /* not an instrument zone */
		if (!in_range(sf->pgen, pg_from, pg_to, pglob_from, pglob_to,
		              SF2_GEN_KEYRANGE, key))
			continue;
		if (!in_range(sf->pgen, pg_from, pg_to, pglob_from, pglob_to,
		              SF2_GEN_VELRANGE, vel))
			continue;
		if ((int)instid >= sf->n_inst - 1)
			continue;

		/* preset-level contribution: global first, local overrides */
		for (i = 0; i < SF2_GEN_MAX; i++) { pacc[i] = 0; pset[i] = 0; }
		for (i = pglob_from; i < pglob_to; i++) {
			const uint8_t *g = sf->pgen + (long)i * 4;
			uint16_t o = GEN_OPER(g);
			if (o < SF2_GEN_MAX) { pacc[o] = (int16_t)GEN_AMOUNT(g); pset[o] = 1; }
		}
		for (i = pg_from; i < pg_to; i++) {
			const uint8_t *g = sf->pgen + (long)i * 4;
			uint16_t o = GEN_OPER(g);
			if (o < SF2_GEN_MAX) { pacc[o] = (int16_t)GEN_AMOUNT(g); pset[o] = 1; }
		}

		ibag_from = INST_BAGNDX(sf->inst + (long)instid * SZ_INST);
		ibag_to   = INST_BAGNDX(sf->inst + (long)(instid + 1) * SZ_INST);
		if (ibag_to > sf->n_ibag - 1) ibag_to = sf->n_ibag - 1;

		split_global(sf->ibag, sf->igen, ibag_from, ibag_to,
		             SF2_GEN_SAMPLEID, &iglob_from, &iglob_to, &ifirst);

		for (iz = ifirst; iz < ibag_to && n < maxout; iz++) {
			int ig_from = BAG_GENNDX(sf->ibag + (long)iz * 4);
			int ig_to   = BAG_GENNDX(sf->ibag + (long)(iz + 1) * 4);
			uint16_t sid;
			const uint8_t *sh;
			sf2_zone *z;
			int32_t v;

			if (!zone_gen(sf->igen, ig_from, ig_to, SF2_GEN_SAMPLEID, &sid))
				continue;
			if (!in_range(sf->igen, ig_from, ig_to, iglob_from, iglob_to,
			              SF2_GEN_KEYRANGE, key))
				continue;
			if (!in_range(sf->igen, ig_from, ig_to, iglob_from, iglob_to,
			              SF2_GEN_VELRANGE, vel))
				continue;
			if ((int)sid >= sf->n_shdr - 1)
				continue;

			sh = sf->shdr + (long)sid * SZ_SHDR;
			if (rd16(sh + 44) & SF2_SMPL_ROM)
				continue;                   /* we have no ROM to read */

			z = &out[n];
			gen_defaults(z->gen);
			apply_absolute(z->gen, sf->igen, iglob_from, iglob_to);
			apply_absolute(z->gen, sf->igen, ig_from, ig_to);
			for (i = 0; i < SF2_GEN_MAX; i++)
				if (pset[i] && preset_additive(i))
					z->gen[i] = clamp16((int32_t)z->gen[i] + (int32_t)pacc[i]);

			/* sample addresses: coarse offsets are in units of 32768 */
			v = (int32_t)rd32(sh + 20)
			  + z->gen[SF2_GEN_STARTADDRO]
			  + 32768L * z->gen[SF2_GEN_STARTADDRCOARSE];
			z->start = (v < 0) ? 0 : (uint32_t)v;
			v = (int32_t)rd32(sh + 24)
			  + z->gen[SF2_GEN_ENDADDRO]
			  + 32768L * z->gen[SF2_GEN_ENDADDRCOARSE];
			z->end = (v < 0) ? 0 : (uint32_t)v;
			v = (int32_t)rd32(sh + 28)
			  + z->gen[SF2_GEN_STARTLOOPADDRO]
			  + 32768L * z->gen[SF2_GEN_STARTLOOPCOARSE];
			z->loopstart = (v < 0) ? 0 : (uint32_t)v;
			v = (int32_t)rd32(sh + 32)
			  + z->gen[SF2_GEN_ENDLOOPADDRO]
			  + 32768L * z->gen[SF2_GEN_ENDLOOPCOARSE];
			z->loopend = (v < 0) ? 0 : (uint32_t)v;

			z->sample_rate     = rd32(sh + 36);
			z->pitch_correction = (int)(int8_t)sh[41];
			z->sample_type     = rd16(sh + 44);
			z->sample_link     = rd16(sh + 42);
			z->sample_index    = (int)sid;
			z->inst_index      = (int)instid;

			if (z->gen[SF2_GEN_OVERRIDEROOTKEY] >= 0)
				z->root_key = z->gen[SF2_GEN_OVERRIDEROOTKEY];
			else {
				int rk = (int)sh[40];
				z->root_key = (rk > 127) ? 60 : rk;   /* 255 = unpitched */
			}

			if (z->sample_rate == 0)
				z->sample_rate = 22050;     /* malformed; keep it audible */

			n++;
		}
	}
	return n;
}
