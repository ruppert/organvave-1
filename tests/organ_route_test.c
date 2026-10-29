/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Guilherme Ruppert (OrganVave-1) */
/* ORGAN's scanner routing (0.10.2, setBfree's RT_UPPRVIB / RT_LOWRVIB in tonegen.c): the upper and the lower manual
 * go through the one scanner by their own switch, the pedals never, the sum of the manuals that are not routed stays
 * dry. The checks are exact (the same events in each run, a fresh engine each time):
 *  1. no manual routed: the scanner's output is the dry one (equal to the scanner off, sample for sample);
 *  2. a manual's keys alone, routed or not: routed only through the scanner, so the other manual's switch changes nothing;
 *  3. the pedals: never through the scanner (a pedal key sounds the same with both manuals routed and the scanner off);
 *  4. both manuals routed and the scanner on: it differs from the dry sound (the wet is there). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "felucca_tables.h"
#define NVOICE 1
#define NPART 1
static inline int32_t clamp_(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
#include "../firmware/engines/src/felucca_engine.h"
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
enum { P_E0, P_E1, P_E2, P_E3 };
struct track { int dummy; };
#include "../firmware/engines/src/dsp.c"
#include "../firmware/src/eng_organ.c"

static int bad;
static void check(const char *what, int ok)
{
    printf("organ route: %-100s %s\n", what, ok ? "ok" : "FAIL");
    bad += !ok;
}

#define NS (CTL * 96u)
static float A[NS], B[NS];

/* a fresh engine: every drawbar at 8 (upper, lower, pedals), the scanner VIB 3, the routing as given; the keys held */
static void play(uint32_t vib, uint32_t up, uint32_t lo, int upk, int lok, int pedk, float *y)
{
    uint32_t i, j, n;
    int32_t b[CTL];
    org_init();
    for (i = 0; i < ORG_NBUS; i++)
        org.bars[i] = 8;
    org.vibm = (uint8_t)vib;
    org.vup = (uint8_t)up;
    org.vlo = (uint8_t)lo;
    if (upk)
        org_key(24, 1);
    if (lok)
        org_key(64 + 24, 1);
    if (pedk)
        org_key(128 + 12, 1);
    for (n = 0; n < NS; n += CTL) {
        memset(b, 0, sizeof b);
        org_render(b, CTL);
        for (j = 0; j < CTL; j++)
            y[n + j] = (float)b[j] / 32768.0f;
    }
    (void)i;
}

static int same(const float *a, const float *b)
{
    return memcmp(a, b, sizeof(float) * NS) == 0;
}

static int differs(const float *a, const float *b)
{
    uint32_t i;
    for (i = 0; i < NS; i++)
        if (a[i] != b[i])
            return 1;
    return 0;
}

static double rms(const float *y)
{
    double s = 0;
    uint32_t i;
    for (i = 0; i < NS; i++)
        s += (double)y[i] * y[i];
    return sqrt(s / NS);
}

int main(void)
{
    static float R[NS];
    /* the dry reference: the scanner off, both manuals and the pedal */
    play(0, 0, 0, 1, 1, 1, A);
    /* 1. the scanner on, nothing routed: the same as off */
    play(3, 0, 0, 1, 1, 1, B);
    check("1. the scanner on, no manual routed: sample for sample the dry sound", same(A, B));
    /* 2. one manual's key alone: the switch of the other manual changes nothing */
    play(0, 0, 0, 1, 0, 0, A);
    play(3, 1, 0, 1, 0, 0, B);
    play(3, 1, 1, 1, 0, 0, R);
    check("2a. the upper key, the upper routed: the same as with both routed (the lower silent)", same(B, R));
    play(3, 0, 1, 1, 0, 0, B);
    check("2b. the upper key, only the lower routed: dry (the upper is not through the scanner)", same(A, B));
    play(0, 0, 0, 0, 1, 0, A);
    play(3, 0, 1, 0, 1, 0, B);
    play(3, 1, 1, 0, 1, 0, R);
    check("2c. the lower key, the lower routed: the same as with both routed (the upper silent)", same(B, R));
    play(3, 1, 0, 0, 1, 0, B);
    play(0, 0, 0, 0, 1, 0, A);
    check("2d. the lower key, only the upper routed: dry", same(A, B));
    /* 3. the pedals: never through the scanner */
    play(0, 0, 0, 0, 0, 1, A);
    play(3, 1, 1, 0, 0, 1, B);
    check("3. a pedal key with both manuals routed: dry (the pedals are not on the scanner)", same(A, B));
    /* 4. both manuals routed: the wet is there, and the level stays sane */
    play(0, 0, 0, 1, 1, 0, A);
    play(3, 1, 1, 1, 1, 0, B);
    check("4. both manuals routed: the scanner's sound differs from the dry one", differs(A, B));
    check("4. ... and it is no louder than 2x the dry sound (the wet replaces, the chorus is not used here)", rms(B) < 2.0 * rms(A) + 1e-6);
    printf("organ route: %s\n", bad ? "FAILED" : "all ok");
    return bad ? 1 : 0;
}
