/* Host-only test harness for mpxplay/emu_sf2.c -- NOT part of any DOS build.
 *
 * Resolves every (preset, key, velocity) in a soundfont with both our parser
 * and the vendored TinySoundFont, and diffs the results. TSF is an independent
 * implementation of the same preset->instrument->sample chain, so agreement
 * across a whole font is strong evidence the record offsets, bag walking and
 * generator merge are right -- none of which can be checked by ear over a
 * serial cable.
 *
 * Build:  cc -O1 -Wall -o /tmp/sf2test tools/sf2test.c mpxplay/emu_sf2.c -lm
 * Run:    /tmp/sf2test font.sf2 [--dump bank prog key vel]
 *
 * Known, deliberate differences from TSF (compensated for below):
 *   - TSF makes loop_end inclusive (-1) and end exclusive-plus-one (+1) to
 *     suit its own interpolator; we keep the SF2 convention, which is what
 *     the EMU10K2's PSST/DSL registers want.
 *   - TSF adds preset-level sample-address offsets. The spec says those
 *     generators are illegal at preset level; we ignore them. Real fonts do
 *     not use them, so this should never show up as a mismatch.
 *   - We remap an originalPitch above 127 (255 = "unpitched") to 60; TSF
 *     passes it through.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../mpxplay/emu_sf2.h"

#define TSF_IMPLEMENTATION
#include "../tsf/tsf.h"

/* ------------------------------------------------- stdio backing for sf2_io */

static int io_read(void *h, void *buf, uint32_t len)
{
	return fread(buf, 1, len, (FILE *)h) == len;
}

static int io_seek(void *h, uint32_t off)
{
	return fseek((FILE *)h, (long)off, SEEK_SET) == 0;
}

static void *xalloc(uint32_t n) { return malloc(n); }
static void  xfree(void *p)     { free(p); }

/* --------------------------------------------------------------- reporting */

static int errors, compared, zones_seen;

static void mismatch(const char *what, int bank, int prog, int key, int vel,
                     long mine, long theirs)
{
	if (errors < 25)
		printf("  MISMATCH %-12s bank=%3d prog=%3d key=%3d vel=%3d  "
		       "ours=%ld tsf=%ld\n", what, bank, prog, key, vel, mine, theirs);
	errors++;
}

int main(int argc, char **argv)
{
	FILE *fp;
	sf2_io io;
	sf2_file sf;
	tsf *t;
	int pidx, np;
	int dump_mode = 0, d_bank = 0, d_prog = 0, d_key = 60, d_vel = 100;

	if (argc < 2) {
		fprintf(stderr, "usage: %s font.sf2 [--dump bank prog key vel]\n", argv[0]);
		return 2;
	}
	if (argc >= 7 && !strcmp(argv[2], "--dump")) {
		dump_mode = 1;
		d_bank = atoi(argv[3]); d_prog = atoi(argv[4]);
		d_key  = atoi(argv[5]); d_vel  = atoi(argv[6]);
	}

	fp = fopen(argv[1], "rb");
	if (!fp) { perror(argv[1]); return 2; }
	io.h = fp; io.read = io_read; io.seek = io_seek;

	if (!sf2_open(&sf, &io, xalloc, xfree)) {
		fprintf(stderr, "sf2_open failed\n");
		return 2;
	}
	printf("ours: SF2 v%d.%02d  presets=%d inst=%d samples=%d  "
	       "smpl @%lu %lu bytes (%lu sample points)\n",
	       sf.ver_major, sf.ver_minor, sf2_preset_count(&sf),
	       sf.n_inst - 1, sf.n_shdr - 1,
	       (unsigned long)sf.smpl_off, (unsigned long)sf.smpl_bytes,
	       (unsigned long)(sf.smpl_bytes / 2));

	t = tsf_load_filename(argv[1]);
	if (!t) { fprintf(stderr, "tsf_load_filename failed\n"); return 2; }
	printf("tsf:  presets=%d\n\n", tsf_get_presetcount(t));

	if (dump_mode) {
		sf2_zone z[64];
		char nm[21];
		int i, n;
		pidx = sf2_find_preset(&sf, d_bank, d_prog);
		if (pidx < 0) { printf("no such preset\n"); return 1; }
		sf2_preset_info(&sf, pidx, nm, NULL, NULL);
		printf("preset[%d] '%s' bank=%d prog=%d  key=%d vel=%d\n",
		       pidx, nm, d_bank, d_prog, d_key, d_vel);
		n = sf2_resolve(&sf, pidx, d_key, d_vel, z, 64);
		printf("%d zone(s)\n", n);
		for (i = 0; i < n; i++) {
			char sn[21], in[21];
			sf2_sample_name(&sf, z[i].sample_index, sn);
			sf2_inst_name(&sf, z[i].inst_index, in);
			printf("  [%d] inst='%s' sample='%s'\n", i, in, sn);
			printf("      start=%lu end=%lu loop=%lu..%lu rate=%lu root=%d corr=%d\n",
			       (unsigned long)z[i].start, (unsigned long)z[i].end,
			       (unsigned long)z[i].loopstart, (unsigned long)z[i].loopend,
			       (unsigned long)z[i].sample_rate, z[i].root_key,
			       z[i].pitch_correction);
			printf("      atten=%dcB pan=%d coarse=%d fine=%d scale=%d mode=%d excl=%d\n",
			       z[i].gen[SF2_GEN_INITIALATTEN], z[i].gen[SF2_GEN_PAN],
			       z[i].gen[SF2_GEN_COARSETUNE], z[i].gen[SF2_GEN_FINETUNE],
			       z[i].gen[SF2_GEN_SCALETUNING], z[i].gen[SF2_GEN_SAMPLEMODES],
			       z[i].gen[SF2_GEN_EXCLUSIVECLASS]);
			printf("      env tc: delay=%d attack=%d hold=%d decay=%d sustain=%dcB release=%d  fc=%d Q=%d\n",
			       z[i].gen[SF2_GEN_DELAYVOLENV], z[i].gen[SF2_GEN_ATTACKVOLENV],
			       z[i].gen[SF2_GEN_HOLDVOLENV], z[i].gen[SF2_GEN_DECAYVOLENV],
			       z[i].gen[SF2_GEN_SUSTAINVOLENV], z[i].gen[SF2_GEN_RELEASEVOLENV],
			       z[i].gen[SF2_GEN_INITIALFILTERFC], z[i].gen[SF2_GEN_INITIALFILTERQ]);
		}
		return 0;
	}

	/* ---- full sweep ---- */
	np = sf2_preset_count(&sf);
	for (pidx = 0; pidx < np; pidx++) {
		int bank, prog, tp, key, vel;
		char nm[21];
		sf2_preset_info(&sf, pidx, nm, &bank, &prog);

		/* find the matching TSF preset */
		tp = -1;
		{
			int i;
			for (i = 0; i < t->presetNum; i++)
				if (t->presets[i].bank == bank && t->presets[i].preset == prog) {
					tp = i;
					break;
				}
		}
		if (tp < 0) {
			printf("  preset bank=%d prog=%d '%s' missing from TSF\n",
			       bank, prog, nm);
			errors++;
			continue;
		}

		for (key = 0; key < 128; key++) {
			static const int vels[3] = { 1, 64, 127 };
			int vi;
			for (vi = 0; vi < 3; vi++) {
				sf2_zone z[128];
				int n, i, tn = 0, ti;
				vel = vels[vi];
				n = sf2_resolve(&sf, pidx, key, vel, z, 128);
				compared++;

				/* TSF's matching regions for the same key/vel */
				for (ti = 0; ti < t->presets[tp].regionNum; ti++) {
					struct tsf_region *r = &t->presets[tp].regions[ti];
					if (key >= r->lokey && key <= r->hikey
					    && vel >= r->lovel && vel <= r->hivel)
						tn++;
				}
				if (n != tn) {
					mismatch("zone-count", bank, prog, key, vel, n, tn);
					continue;
				}
				zones_seen += n;

				/* regions come out in the same walk order in both */
				i = 0;
				for (ti = 0; ti < t->presets[tp].regionNum; ti++) {
					struct tsf_region *r = &t->presets[tp].regions[ti];
					long ours, theirs;
					if (!(key >= r->lokey && key <= r->hikey
					      && vel >= r->lovel && vel <= r->hivel))
						continue;
					if (i >= n) break;

					if ((long)z[i].start != (long)r->offset)
						mismatch("start", bank, prog, key, vel,
						         z[i].start, r->offset);
					if ((long)z[i].sample_rate != (long)r->sample_rate)
						mismatch("rate", bank, prog, key, vel,
						         z[i].sample_rate, r->sample_rate);
					/* TSF stores loop_end inclusive */
					if (z[i].loopend > 0) {
						ours = (long)z[i].loopend - 1;
						theirs = (long)r->loop_end;
						if (ours != theirs && theirs != 0)
							mismatch("loopend", bank, prog, key, vel, ours, theirs);
					}
					if ((long)z[i].loopstart != (long)r->loop_start)
						mismatch("loopstart", bank, prog, key, vel,
						         z[i].loopstart, r->loop_start);
					/* TSF bumps end by one unless it clamped to the pool */
					ours = (long)z[i].end + 1;
					theirs = (long)r->end;
					if (ours != theirs
					    && theirs != (long)(sf.smpl_bytes / 2))
						mismatch("end", bank, prog, key, vel, ours, theirs);

					if (z[i].root_key != r->pitch_keycenter
					    && !(z[i].root_key == 60 && r->pitch_keycenter > 127))
						mismatch("rootkey", bank, prog, key, vel,
						         z[i].root_key, r->pitch_keycenter);

					ours = (long)z[i].gen[SF2_GEN_COARSETUNE] * 100
					     + z[i].gen[SF2_GEN_FINETUNE]
					     + z[i].pitch_correction;
					theirs = (long)r->transpose * 100 + r->tune;
					if (ours != theirs)
						mismatch("tune", bank, prog, key, vel, ours, theirs);

					if (z[i].gen[SF2_GEN_SCALETUNING] != r->pitch_keytrack)
						mismatch("scaletune", bank, prog, key, vel,
						         z[i].gen[SF2_GEN_SCALETUNING], r->pitch_keytrack);

					{
						int lm = z[i].gen[SF2_GEN_SAMPLEMODES] & 3;
						int expect = (lm == 3) ? TSF_LOOPMODE_SUSTAIN
						           : (lm == 1) ? TSF_LOOPMODE_CONTINUOUS
						                       : TSF_LOOPMODE_NONE;
						if (expect != r->loop_mode)
							mismatch("loopmode", bank, prog, key, vel,
							         expect, r->loop_mode);
					}
					if ((unsigned)z[i].gen[SF2_GEN_EXCLUSIVECLASS] != r->group)
						mismatch("exclclass", bank, prog, key, vel,
						         z[i].gen[SF2_GEN_EXCLUSIVECLASS], r->group);
					i++;
				}
			}
		}
	}

	printf("\n%d resolutions, %d zones compared, %d mismatch(es)\n",
	       compared, zones_seen, errors);
	sf2_close(&sf, xfree);
	tsf_close(t);
	return errors ? 1 : 0;
}
