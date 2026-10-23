/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* setBfree's tone generator (src/tonegen.c, GPL-2.0-or-later, Fredrik Kilander, Robin Gareus, Ken Restivo, Will Panther)
 * built as it is on the host: OrganVave-1's floating-point reference (ORGAN.md). Built by build.sh against a setBfree
 * checkout; nothing of setBfree is copied into this tree.
 *   sbf_ref dump OUT.txt                    the data the firmware's tables are generated from (gen_organ_tables.py)
 *   sbf_ref render OUT.f32 BARS SECS EVENTS  mono float32 at 44.1 kHz; BARS "888000000"; EVENTS "t:+key,t:-key,..."
 *                                            (key 0..60 = the upper manual C..C); env SWELL (default 1.0), TEMP 0/1/2,
 *                                            NOXT (crosstalk off)
 *   sbf_ref adv OUT.txt                     the advanced parameters (ADVANCED EDIT, tests/organ_adv_test.c): the keys'
 *                                            contributions and the click envelopes with the tone generator's settings
 *                                            from the environment: COMP TRANS TSTRIP WIRE FLOOR MIN (crosstalk, 0..1),
 *                                            ATKLEVEL ATKMIN ATKMAX RELLEVEL (0..1), ATKMODEL RELMODEL (0 click, 1 cosine,
 *                                            2 linear, 3 shelf); the unset ones keep setBfree's defaults
 * srand(1) before the init: the click envelopes are the same every run. */
#include "tonegen.c"
#include <stdio.h>
#include <stdlib.h>

double SampleRateD = 44100.0;
int mainConfig (ConfigContext* cfg) { (void)cfg; return 0; }
const ConfigDoc* mainDoc (void) { return NULL; }

static struct b_tonegen* make (int gear, int xtalk)
{
	struct b_tonegen* t;
	void *st, *mc;
	srand (1);
	t = allocTonegen ();
	st = allocRunningConfig ();
	mc = allocMidiCfg (st);
	initControllerTable (mc);
	if (!xtalk)                           /* VOICE CROSSTALK off: no compartment, terminal or wiring bleed */
		t->defaultCompartmentCrosstalk = t->defaultTerminalStripCrosstalk = t->defaultWiringCrosstalk = 0.0;
	t->gearTuning = gear;                 /* 0 equal, 1 gear60, 2 gear50 */
	t->envAttackClickLevel = 1.0;         /* 0.9.5: the key click twice as strong as setBfree's default (0.5), the release click too (0.25) */
	t->envReleaseClickLevel = 0.5;
	initToneGenerator (t, mc);
	if (gear != 1 || !xtalk) {
		/* the click envelopes come from rand() after the wavetables' dither, so they depend on the wavetables:
		 * every temperament takes the 60 Hz ones, as the firmware does (one set of tables) */
		struct b_tonegen* r = make (1, 1);
		memcpy (t->attackEnv, r->attackEnv, sizeof t->attackEnv);
		memcpy (t->releaseEnv, r->releaseEnv, sizeof t->releaseEnv);
	}
	return t;
}

int main (int argc, char** argv)
{
	if (argc >= 3 && !strcmp (argv[1], "dump")) {
		FILE* f = fopen (argv[2], "w");
		struct b_tonegen *t1 = make (1, 1), *t2 = make (2, 1), *t0 = make (0, 1), *tx = make (1, 0);
		int i, b, k;
		fprintf (f, "# setBfree tonegen dump (sbf_ref.c): SampleRate %.1f BUFFER_SIZE_SAMPLES %d\n", SampleRateD, BUFFER_SIZE_SAMPLES);
		for (i = 1; i <= 91; i++)
			fprintf (f, "wheel %d %.9f %.9f %.9f %.9f\n", i, t1->oscillators[i].frequency, t2->oscillators[i].frequency,
			         t0->oscillators[i].frequency, t1->oscillators[i].attenuation);
		for (i = 0; i < MAX_PARTIALS; i++)
			if (t1->wheel_Harmonics[i] != (i == 0 ? 1.0 : 0.0))
				fprintf (f, "harmonic %d %.9f\n", i + 1, t1->wheel_Harmonics[i]);
		for (i = 0; i <= 91; i++)
			if (t1->wheelHarmonics[i])
				fprintf (f, "wheelharmonics %d (present)\n", i);
		/* the lower manual (keys 64..127, buses 9..17) is the upper one moved by 9 buses: checked, then left out */
		for (k = 0; k < 64; k++) {
			ListElement *a = t1->keyContrib[k], *b = t1->keyContrib[k + 64], *c = tx->keyContrib[k], *d = tx->keyContrib[k + 64];
			for (; a && b; a = a->next, b = b->next)
				if (LE_WHEEL_NUMBER_OF (a) != LE_WHEEL_NUMBER_OF (b) || LE_BUSNUMBER_OF (a) + 9 != LE_BUSNUMBER_OF (b) || LE_LEVEL_OF (a) != LE_LEVEL_OF (b))
					break;
			for (; c && d; c = c->next, d = d->next)
				if (LE_WHEEL_NUMBER_OF (c) != LE_WHEEL_NUMBER_OF (d) || LE_BUSNUMBER_OF (c) + 9 != LE_BUSNUMBER_OF (d) || LE_LEVEL_OF (c) != LE_LEVEL_OF (d))
					break;
			if (a || b || c || d) {
				fprintf (stderr, "sbf_ref: lower key %d differs from the upper one\n", k);
				return 1;
			}
		}
		fprintf (f, "lower = upper + 9 buses\n");
		/* the terminals' neighbours (the crosstalk model's wiring: compartment pair, transformers, terminal strip) */
		for (i = 1; i <= NOF_WHEELS; i++) {
			int te = 0, tw = 0, se = 0, sw = 0;
			findEastWestNeighbours (terminalStrip, i, &se, &sw);
			if (44 <= i)          /* (a wheel in neither row has no neighbours: setBfree's assert is off in a release build) */
				if (!findEastWestNeighbours (northTransformers, i, &te, &tw))
					findEastWestNeighbours (southTransformers, i, &te, &tw);
			fprintf (f, "topo %d %d %d %d %d %d\n", i, wheelPairs[i], te, tw, se, sw);
		}
		for (k = 0; k < MAX_KEYS; k++) {
			ListElement* lep;
			if (64 <= k && k < 128)
				continue;
			for (lep = t1->keyContrib[k]; lep; lep = lep->next)
				fprintf (f, "contrib %d %d %d %.9g\n", k, LE_WHEEL_NUMBER_OF (lep), LE_BUSNUMBER_OF (lep), LE_LEVEL_OF (lep));
		}
		for (k = 0; k < MAX_KEYS; k++) {
			ListElement* lep;
			if (64 <= k && k < 128)
				continue;
			for (lep = tx->keyContrib[k]; lep; lep = lep->next)
				fprintf (f, "contribnx %d %d %d %.9g\n", k, LE_WHEEL_NUMBER_OF (lep), LE_BUSNUMBER_OF (lep), LE_LEVEL_OF (lep));
		}
		for (b = 0; b < 9; b++) {
			fprintf (f, "attack %d", b);
			for (i = 0; i < BUFFER_SIZE_SAMPLES; i++) fprintf (f, " %.7f", t1->attackEnv[b][i]);
			fprintf (f, "\nrelease %d", b);
			for (i = 0; i < BUFFER_SIZE_SAMPLES; i++) fprintf (f, " %.7f", t1->releaseEnv[b][i]);
			fprintf (f, "\n");
		}
		for (i = 0; i < MAXKEYS; i++)
			fprintf (f, "keycomp %d %.9f\n", i, t1->keyCompTable[i]);
		fprintf (f, "gaintc %.9f\n", t1->gainTimeConstant);
		fclose (f);
		return 0;
	}
	if (argc >= 6 && !strcmp (argv[1], "render")) {
		struct b_tonegen* t = make (getenv ("TEMP") ? atoi (getenv ("TEMP")) : 1, getenv ("NOXT") ? 0 : 1);
		unsigned int bars[9], n = (unsigned int)(atof (argv[4]) * SampleRateD) / 128 * 128, p;
		float buf[128];
		char* ev = strdup (argv[5]);
		double et[256]; int ek[256], eon[256], ne = 0, ei = 0, i;
		FILE* f = fopen (argv[2], "wb");
		for (i = 0; i < 9; i++) bars[i] = (unsigned int)(argv[3][i] - '0');
		for (i = 0; i < 9; i++) setDrawBar (t, UPPER_BUS_LO + i, bars[i]);
		t->swellPedalGain = t->targetGain = t->currentGain = getenv ("SWELL") ? atof (getenv ("SWELL")) : 1.0;
		for (char* s = strtok (ev, ","); s && ne < 256; s = strtok (NULL, ",")) {
			char c; int key;
			if (sscanf (s, "%lf:%c%d", &et[ne], &c, &key) == 3) { ek[ne] = key; eon[ne] = c == '+'; ne++; }
		}
		for (p = 0; p < n; p += 128) {
			while (ei < ne && et[ei] * SampleRateD <= p) {
				if (eon[ei]) oscKeyOn (t, (unsigned char)ek[ei], 255); else oscKeyOff (t, (unsigned char)ek[ei], 255);
				ei++;
			}
			oscGenerateFragment (t, buf, 128);
			fwrite (buf, sizeof (float), 128, f);
		}
		fclose (f);
		return 0;
	}
	if (argc >= 3 && !strcmp (argv[1], "adv")) {
		FILE* f = fopen (argv[2], "w");
		struct b_tonegen* t;
		void *st, *mc;
		int b, i, k;
#define ENVD(name, var) if (getenv (name)) var = atof (getenv (name))
		srand (1);
		t = allocTonegen ();
		st = allocRunningConfig ();
		mc = allocMidiCfg (st);
		initControllerTable (mc);
		t->gearTuning = 1;
		ENVD ("COMP", t->defaultCompartmentCrosstalk);
		ENVD ("TRANS", t->defaultTransformerCrosstalk);
		ENVD ("TSTRIP", t->defaultTerminalStripCrosstalk);
		ENVD ("WIRE", t->defaultWiringCrosstalk);
		ENVD ("FLOOR", t->contributionFloorLevel);
		ENVD ("MIN", t->contributionMinLevel);
		ENVD ("ATKLEVEL", t->envAttackClickLevel);
		ENVD ("RELLEVEL", t->envReleaseClickLevel);
		if (getenv ("ATKMIN")) setEnvAtkClkMinLength (t, atof (getenv ("ATKMIN")));
		if (getenv ("ATKMAX")) setEnvAtkClkMaxLength (t, atof (getenv ("ATKMAX")));
		if (getenv ("ATKMODEL")) setEnvAttackModel (t, atoi (getenv ("ATKMODEL")));
		if (getenv ("RELMODEL")) setEnvReleaseModel (t, atoi (getenv ("RELMODEL")));
		initToneGenerator (t, mc);
		fprintf (f, "# sbf_ref adv\n");
		for (k = 0; k < MAX_KEYS; k++) {
			ListElement* lep;
			if (64 <= k && k < 128)
				continue;
			for (lep = t->keyContrib[k]; lep; lep = lep->next)
				fprintf (f, "contrib %d %d %d %.9g\n", k, LE_WHEEL_NUMBER_OF (lep), LE_BUSNUMBER_OF (lep), LE_LEVEL_OF (lep));
		}
		for (b = 0; b < 8; b++) {
			fprintf (f, "attack %d", b);
			for (i = 0; i < BUFFER_SIZE_SAMPLES; i++) fprintf (f, " %.7f", t->attackEnv[b][i]);
			fprintf (f, "\nrelease %d", b);
			for (i = 0; i < BUFFER_SIZE_SAMPLES; i++) fprintf (f, " %.7f", t->releaseEnv[b][i]);
			fprintf (f, "\n");
		}
		fclose (f);
		return 0;
	}
	fprintf (stderr, "usage: sbf_ref dump OUT.txt | adv OUT.txt | render OUT.f32 BARS SECS EVENTS\n");
	return 2;
}
