/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* The drive's floating-point reference: setBfree's own overdrive.c (b_overdrive/overdrive.c, GPL-2.0-or-later) at its
 * default constants, run on a mono input file.
 *
 *   drive_render IN.f32 OUT.f32 [INPUT_GAIN OUTPUT_GAIN FB FB2 GFB BIAS ZGB]
 *   drive_render --fir          prints the effective input FIR (the sum of the four polyphase rows) and the output FIR
 *
 * IN.f32: mono float32. OUT.f32: mono float32, the same length. Defaults are setBfree's (3.5675, 0.8795, 0.5821, 0.999,
 * -0.6214 as GFB 0.6214, 0.5347, 0.0094). Note what setBfree's loop really does: for every input sample it sums all four
 * polyphase rows of the interpolation filter (so the input FIR is 9 taps at the input rate), runs the transfer function
 * once, and filters the result with the 33-tap "decimation" FIR: there is no oversampling in the signal path.
 * Built with tools/setbfree_ref/build.sh (the same stubs as whirl_render.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "overdrive.c"

int getConfigParameter_d (const char *par, ConfigContext *cfg, double *dp) { (void)par; (void)cfg; (void)dp; return 0; }
int getConfigParameter_i (const char *par, ConfigContext *cfg, int *dp) { (void)par; (void)cfg; (void)dp; return 0; }
int getConfigParameter_f (const char *par, ConfigContext *cfg, float *dp) { (void)par; (void)cfg; (void)dp; return 0; }
int getConfigParameter_fr (const char *par, ConfigContext *cfg, float *dp, float lo, float hi) { (void)par; (void)cfg; (void)dp; (void)lo; (void)hi; return 0; }
void useMIDIControlFunction (void *m, const char *cfname, void (*f) (void *, unsigned char), void *d) { (void)m; (void)cfname; (void)f; (void)d; }

int main (int argc, char **argv)
{
	struct b_preamp *pp = (struct b_preamp *) allocPreamp ();
	initPreamp (pp, NULL);
	if (argc >= 2 && !strcmp (argv[1], "--fir")) {
		int k;
		for (k = 0; k < 9; k++) {
			float h = 0;
			int i;
			for (i = 0; i < 4; i++) if (k < wiLen[i]) h += pp->wi[i][k];
			printf ("in  %d %.9f\n", k, h);
		}
		for (k = 0; k < 33; k++) printf ("out %d %.9f\n", k, pp->aal[k]);
		return 0;
	}
	if (argc < 3) { fprintf (stderr, "usage: drive_render IN.f32 OUT.f32 [IG OG FB FB2 GFB BIAS ZGB]\n"); return 1; }
	if (argc > 3) pp->inputGain = atof (argv[3]);
	if (argc > 4) pp->outputGain = atof (argv[4]);
	if (argc > 5) pp->adwFb = atof (argv[5]);
	if (argc > 6) pp->adwFb2 = atof (argv[6]);
	if (argc > 7) pp->adwGfb = -atof (argv[7]);
	if (argc > 8) cfg_biased (pp, atof (argv[8]));
	if (argc > 9) pp->sagZgb = atof (argv[9]);
	FILE *f = fopen (argv[1], "rb");
	if (!f) { perror (argv[1]); return 1; }
	fseek (f, 0, SEEK_END);
	long n = ftell (f) / 4;
	fseek (f, 0, SEEK_SET);
	float *in = malloc (n * 4), *out = malloc (n * 4);
	if (fread (in, 4, n, f) != (size_t) n) return 1;
	fclose (f);
	overdrive (pp, in, out, n);
	f = fopen (argv[2], "wb");
	fwrite (out, 4, n, f);
	fclose (f);
	return 0;
}
