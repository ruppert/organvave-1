/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments; Guilherme Ruppert (FMini-1, organvave-1) */
/* Regression suite of the DSP on the host (the firmware's sources, through tests/host.h).
 *   build/host/regress [GOLDEN_FILE CPU_FILE]      (run_tests.sh builds and runs it)
 *
 * 1. golden renders: every factory patch of the organ, the FX sends on its first patch, and the sequencer playing a
 *    pattern. Each render plays a fixed phrase (notes, an
 *    overlap, a chord, note-offs, the release tail) and is reduced to a 64-bit FNV-1a hash of its output samples,
 *    compared with GOLDEN_FILE (tests/golden.txt). Every render runs in its own fork()ed child of a process that never
 *    touched the DSP state, so all state starts as at boot. The renders keep the REVERB TYPE at ROOM (the power-on
 *    reverb of the hashes' first days; HALL is the default since, its own test is fx_test.c).
 * 2. health checks on every render: no sample beyond 16 bits, none near full scale, DC offset, the peak level, every
 *    organ free after the release, silence at the end.
 * 3. CPU budget (hosts with a kernel instruction counter: macOS): per patch (8 notes held), the instructions per
 *    sample against CPU_FILE (tests/cpu_baseline.txt) at +-25 %.
 * 4. keys: the key budget under random notes, a held key keeps sounding under pressure, and no key left down after
 *    the note-offs (MIDI on every channel, the keys, the octave buttons, random patches).
 * env: GOLDEN_UPDATE=1 rewrites GOLDEN_FILE, BUDGET_UPDATE=1 rewrites CPU_FILE (on purpose: review the diff),
 * VERBOSE=1 prints every render's numbers, JOBS=n children at once (default 8). */
#define HOST_NO_UI 1
#include "host.h"

/* ------------------------------------------------------------- limits --- */
#define LIM_NEAR 30000            /* |sample| at or above: "near full scale" (the limiter holds ~18000) */
#define LIM_DC 400                /* |mean| of the sounding part, 16-bit units */
#define LIM_PEAK_MIN 250          /* quieter than this (-42 dBFS): something is missing */
#define LIM_TAIL 6                /* AC peak of the last 0.25 s, after the voices are free and the FX rang out */
#define LIM_TAIL_DC 2             /* its constant part */
#define FREE_CAP_S 40             /* the organ must be free this long after the last note-off (its release, the tonewheels'
                                   * decay to silence, is a few seconds at most) */
#define TAIL_S 8                  /* FX tail rendered after the organ is free (the delay and reverb sends ring for seconds) */
#define CPU_TOL 0.25              /* +-25 %, and at least CPU_SLACK instructions */
#define CPU_SLACK 20

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------ results --- */
typedef struct {
    uint64_t hash;
    uint32_t frames, over, near, vmax, ok;     /* ok: a check's own result */
    int32_t peak, tail_peak, tail_dc, tmin, tmax;
    double dc, free_s;                         /* free_s: s from the last note-off until all voices free, -1 never */
    double ipc, ns;                            /* CPU: instructions and ns per sample */
    char msg[512];
} res_t;

static res_t R;
static uint32_t fpos, rel_at, dc_end, tail_from;
static int64_t dcs[2];

static int parts_free(void)                     /* the organ: no tone instance, no queued key, no click left */
{
    return !org.nins && !org.nq && !org.nacl;
}

static int32_t last_out[2 * CTL];
static void blk(void)
{
    int32_t *o = last_out;
    uint32_t i, k;
    mix_block(o, CTL);
    for (i = 0; i < 2u * CTL; i++) {
        int32_t x = o[i], a = x < 0 ? -x : x;
        uint32_t u = (uint32_t)x;
        for (k = 0; k < 4u; k++) {
            R.hash ^= (u >> (8u * k)) & 0xFFu;
            R.hash *= 0x100000001B3ull;
        }
        if (a > R.peak)
            R.peak = a;
        R.over += a > 32767;
        R.near += a >= LIM_NEAR;
        if (fpos < dc_end)
            dcs[i & 1u] += x;
        if (tail_from && fpos >= tail_from) {
            R.tmin = x < R.tmin ? x : R.tmin;
            R.tmax = x > R.tmax ? x : R.tmax;
        }
    }
    k = busy_now();
    if (k > R.vmax)
        R.vmax = k;
    fpos += CTL;
}

static uint32_t at(double s) { return (uint32_t)(s * FS) / CTL * CTL; }
static void run_to(uint32_t f) { while (fpos < f) blk(); }

/* after the last note-off (rel_at): until the voices are free, then the FX tail */
static void finish(void)
{
    uint32_t cap = rel_at + FREE_CAP_S * FS;
    dc_end = dc_end ? dc_end : fpos;
    while (!parts_free() && fpos < cap)
        blk();
    R.free_s = parts_free() ? (double)(fpos - rel_at) / FS : -1;
    tail_from = fpos + TAIL_S * FS - FS / 4u;
    R.tmin = 0x7FFFFFFF;
    R.tmax = -0x7FFFFFFF;
    run_to(fpos + TAIL_S * FS);
    R.tail_peak = (R.tmax - R.tmin + 1) / 2;
    R.tail_dc = (R.tmax + R.tmin) / 2;
    R.frames = fpos;
    R.dc = (double)(llabs(dcs[0]) > llabs(dcs[1]) ? dcs[0] : dcs[1]) / (double)(dc_end ? dc_end : 1);
}

/* notes (with an overlap: legato / steal), a 5-note chord, note-offs, the tail. Through input_on / input_off (the
 * keys' and MIDI's way in: a patch's ARP plays) */
static void phrase(track_t *t, uint32_t base)
{
    static const uint8_t CH[5] = {0, 7, 12, 16, 19};
    uint32_t i;
    input_on(t, base, 100);
    run_to(at(0.30));
    input_off(t, base);
    run_to(at(0.35));
    input_on(t, base + 4u, 80);
    run_to(at(0.60));
    input_on(t, base + 7u, 120);               /* overlaps: MONO / LEGATO hand over, POLY two voices */
    run_to(at(0.70));
    input_off(t, base + 4u);
    run_to(at(0.95));
    input_off(t, base + 7u);
    run_to(at(1.00));
    for (i = 0; i < 5u; i++)
        input_on(t, base - 12u + CH[i], 90 + 5 * i);
    run_to(at(1.80));
    for (i = 0; i < 5u; i++)
        input_off(t, base - 12u + CH[i]);
    rel_at = fpos;
    dc_end = fpos + FS / 2u;
    finish();
}

/* ------------------------------------------------------------- jobs --- */
enum { J_PRESET, J_SENDS, J_SEQ, J_CPU, J_CHECK };
typedef struct {
    char name[64];
    uint8_t kind, e, pi, arg;
    int (*check)(char *msg, uint32_t n);       /* J_CHECK */
    res_t r;
    int crashed;
} job_t;

static void power_on(void)
{
    host_tracks_init();
    org_init();                                /* the tone generator at rest (main.c's boot) */
    rot_load_tables();                         /* the effects at rest, as main.c's felucca_init leaves them */
    rot_init(&rot_st);
    dr_init(&dr_st);
    eq_init(&eq_st);
    song.g[G_RTYPE] = 0;                       /* ROOM: the renders' reverb (see the top) */
}

static void job_preset(const job_t *j)
{
    track_t *t = &trk[0];
    power_on();
    host_preset(t, j->e, j->pi);
    phrase(t, 60);
}

static void job_sends(const job_t *j)           /* arg: 0 dry, 1 chorus, 2 delay, 3 reverb, 4 all, 5 DIST */
{
    static const int16_t S[6][4] = {{0, 0, 0, 0}, {0, 110, 0, 0}, {0, 0, 110, 0}, {0, 0, 0, 110},
                                    {40, 80, 80, 80}, {110, 0, 0, 30}};
    track_t *t = &trk[0];
    uint32_t i;
    power_on();
    host_preset(t, j->e, j->pi);
    t->p[P_AMODE] = 0;
    for (i = 0; i < 4u; i++)
        t->p[P_DIST + i] = S[j->arg][i];
    phrase(t, 60);
}

/* the sequencer: 16 steps (notes, a rest, a TIE, an accent, a slide) at 120 BPM, two passes, with a swing */
static void job_seq(const job_t *j)
{
    static const uint8_t N[16] = {48, 48, 60, 0, 51, 51, 55, 0, 48, 0, 63, 60, 58, 0, 55, 53};
    track_t *t = &trk[0];
    uint32_t i;
    power_on();
    host_preset(t, j->e, j->pi);
    t->p[P_AMODE] = 0;
    t->p[P_SLEN] = 16;
    t->p[P_SSWING] = 20;
    for (i = 0; i < 16u; i++) {
        uint8_t n = N[i];
        put_step(t, i, n ? 1u : 0u, &n, n ? ST_NOTE : i == 3u ? ST_TIE : ST_REST,
                 i == 2u ? SF_ACCENT : i == 10u ? SF_SLIDE : 0u);
    }
    transport_req = 1;
    run_to(at(4.0));
    transport_req = 2;
    blk();
    rel_at = fpos;
    dc_end = fpos;
    finish();
}

static uint64_t instr_now(void)
{
#ifdef __APPLE__
    struct rusage_info_v4 ri;
    if (!proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t *)&ri))
        return ri.ri_instructions;
#endif
    return 0;
}
static void job_cpu(const job_t *j)             /* e 0xFF: idle (nothing held) */
{
    static const uint8_t NOTES[8] = {48, 52, 55, 59, 60, 64, 67, 71};
    uint32_t i, k, nb = FS / CTL;
    uint64_t i0 = 0, t0 = 0;
    power_on();
    if (j->e != 0xFF) {
        host_preset(&trk[0], j->e, j->pi);
        trk[0].p[P_AMODE] = 0;
        for (i = 0; i < 8u; i++)
            trk_note_on(&trk[0], NOTES[i], 100);
    }
    for (k = 0; k < nb / 2u + nb; k++) {
        if (k == nb / 2u) {
            i0 = instr_now();
            t0 = now_ns();
        }
        mix_block(last_out, CTL);
    }
    R.ns = (double)(now_ns() - t0) / (nb * CTL);
    R.ipc = i0 ? (double)(instr_now() - i0) / (nb * CTL) : 0;
}

static void run_job_body(job_t *j)
{
    memset(&R, 0, sizeof R);
    switch (j->kind) {
    case J_PRESET: job_preset(j); break;
    case J_SENDS: job_sends(j); break;
    case J_SEQ: job_seq(j); break;
    case J_CPU: job_cpu(j); break;
    case J_CHECK: R.ok = (uint32_t)j->check(R.msg, sizeof R.msg); break;
    }
}

/* run jobs [0, n) in children, up to nj at once; results come back through pipes */
static void run_jobs(job_t *jobs, uint32_t n, uint32_t nj)
{
    uint32_t i, k;
    for (i = 0; i < n; i += nj) {
        int fd[64];
        pid_t pid[64];
        uint32_t m = n - i < nj ? n - i : nj;
        for (k = 0; k < m; k++) {
            int p[2];
            if (pipe(p)) {
                perror("pipe");
                exit(2);
            }
            fflush(stdout);
            if (!(pid[k] = fork())) {
                close(p[0]);
                run_job_body(&jobs[i + k]);
                if (write(p[1], &R, sizeof R) != sizeof R)
                    _exit(1);
                _exit(0);
            }
            close(p[1]);
            fd[k] = p[0];
        }
        for (k = 0; k < m; k++) {
            size_t got = 0;
            ssize_t r;
            int st = 0;
            while (got < sizeof(res_t) && (r = read(fd[k], (char *)&jobs[i + k].r + got, sizeof(res_t) - got)) > 0)
                got += (size_t)r;
            close(fd[k]);
            waitpid(pid[k], &st, 0);
            jobs[i + k].crashed = got != sizeof(res_t) || !WIFEXITED(st) || WEXITSTATUS(st);
        }
    }
}

/* ------------------------------------------------- key / routing checks --- */
static uint32_t lcg = 12345u;
static uint32_t rnd(uint32_t n)
{
    lcg = lcg * 1103515245u + 12345u;
    return (lcg >> 8) % n;
}
static void midi_pkt(uint32_t st, uint32_t d1, uint32_t d2)   /* as usb.c: the queue holds MQ packets */
{
    if (mi_w - mi_r >= MQ)
        blk();
    midi_in_q[mi_w++ % MQ] = (st >> 4) | st << 8 | d1 << 16 | d2 << 24;
}

/* the key budget: random notes on and off for 6 s, up to 48 held (the upper manual's 61 keys: MIDI notes 36..96, the
 * keys 0..60); after every block at most ORG_UPPER keys down; then all up: the organ free */
#define ORG_UPPER 61u                  /* the upper manual: keys 0..60 (org_note) */
static int chk_keys(char *msg, uint32_t n)
{
    uint8_t held[128] = {0};
    uint32_t k, worst = 0;
    power_on();
    host_preset(&trk[0], ENGI_ORGAN, 2);
    for (k = 0; k < 6u * FS / CTL; k++) {
        uint32_t i, a = 0;
        if (k % 4u == 0u) {
            uint32_t note = 36u + rnd(48);
            if (held[note])
                trk_note_off(&trk[0], note);
            else
                trk_note_on(&trk[0], note, 60u + rnd(60));
            held[note] ^= 1u;
        }
        blk();
        for (i = 0; i < ORG_UPPER; i++)
            a += org.keys[i] != 0;
        worst = a > worst ? a : worst;
    }
    for (k = 0; k < 128u; k++)
        if (held[k])
            trk_note_off(&trk[0], k);
    rel_at = fpos;
    finish();
    snprintf(msg, n, "random notes for 6 s: at most %u of the upper manual's %u keys down at once, the organ free "
             "%.2f s after the note-offs", worst, ORG_UPPER, R.free_s);
    return worst <= ORG_UPPER && R.free_s >= 0;
}

/* a held key keeps sounding while higher keys come and go for 2 s */
static int chk_keep_key(char *msg, uint32_t n)
{
    uint32_t k, lost = 0;
    power_on();
    host_preset(&trk[0], ENGI_ORGAN, 0);
    trk_note_on(&trk[0], 40, 100);
    for (k = 0; k < 2u * FS / CTL; k++) {
        if (k % 3u == 0u) {                    /* (at most 2 others held: the key queue keeps them all) */
            uint32_t m = 50u + k / 3u % 40u;
            trk_note_off(&trk[0], m - 2u);
            trk_note_on(&trk[0], m, 100);
        }
        blk();
        lost += !org.keys[40u - 36u];
    }
    trk_note_off(&trk[0], 40);
    snprintf(msg, n, "a held key keeps sounding through 2 s of other keys: lost in %u blocks", lost);
    return !lost;
}

/* no hanging notes: 6 s of random MIDI note-ons / offs on several channels and the keys, the octave buttons, three
 * random patches; then every held note off: no key may stay down, the organ must be free */
static int chk_hang(char *msg, uint32_t n)
{
    static const uint8_t CHS[7] = {0, 1, 2, 3, 4, 9, 15};
    uint8_t on[7][128] = {{0}};
    uint32_t keys = 0, k, c, ev = 0, round;
    char who[96] = "";
    int bad = 0;
    for (round = 0; round < 3u && !bad; round++) {
        power_on();
        host_preset(&trk[0], ENGI_ORGAN, rnd(ORG_NFACTORY));
        for (k = 0; k < 6u * FS / CTL; k++) {
            uint32_t r = rnd(16);
            if (r < 6u) {                          /* MIDI */
                uint32_t ci = rnd(7), note = 36u + rnd(36);
                if (on[ci][note]) {
                    midi_pkt(rnd(2) ? 0x80u | CHS[ci] : 0x90u | CHS[ci], note, 0);
                    on[ci][note] = 0;
                } else {
                    midi_pkt(0x90u | CHS[ci], note, 1u + rnd(127));
                    on[ci][note] = 1;
                }
                ev++;
            } else if (r < 9u) {                   /* a key down or up */
                keys ^= 1u << rnd(27);
                fm1_in.notes = keys;
                ev++;
            } else if (r == 9u) {                  /* an octave button */
                song.octave = (int8_t)((int)rnd(5) - 2);
            }
            blk();
        }
        fm1_in.notes = keys = 0;                   /* everything off */
        for (c = 0; c < 7u; c++)
            for (k = 0; k < 128u; k++)
                if (on[c][k]) {
                    midi_pkt(0x80u | CHS[c], k, 64);
                    on[c][k] = 0;
                }
        run_to(fpos + FS / 2u);
        for (k = 0; k < ORG_NKEY && !bad; k++)
            if (org.keys[k]) {
                bad = 1;
                snprintf(who, sizeof who, "key %u still down", k);
            }
        rel_at = fpos;
        {
            uint32_t cap = fpos + FREE_CAP_S * FS;
            while (!parts_free() && fpos < cap)
                blk();
            if (!parts_free() && !bad) {
                bad = 2;
                snprintf(who, sizeof who, "the organ still sounds %d s after the note-offs", FREE_CAP_S);
            }
        }
    }
    snprintf(msg, n, "%u random MIDI / key events on ch 1-5, 10, 16 and the keys, octave changing, 3 random patches: %s",
             ev, bad ? who : "no key left down, the organ free");
    return !bad;
}

/* ------------------------------------------------------------ files --- */
typedef struct { char name[64]; char val[64]; } kv_t;
static uint32_t load_kv(const char *path, kv_t *kv, uint32_t max)
{
    FILE *f = fopen(path, "r");
    char line[256];
    uint32_t n = 0;
    if (!f)
        return 0;
    while (fgets(line, sizeof line, f) && n < max) {
        if (line[0] == '#' || line[0] == '\n')
            continue;
        if (sscanf(line, "%63s %63s", kv[n].name, kv[n].val) == 2)
            n++;
    }
    fclose(f);
    return n;
}
static const char *kv_get(const kv_t *kv, uint32_t n, const char *name)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (!strcmp(kv[i].name, name))
            return kv[i].val;
    return 0;
}

static void slug(char *d, const char *s, size_t n)
{
    size_t i = 0;
    for (; *s && i + 1 < n; s++)
        d[i++] = (*s == ' ' || *s == '/') ? '_' : *s;
    d[i] = 0;
}

/* ------------------------------------------------------------- main --- */
#define MAXJ 512
static job_t J[MAXJ];
static uint32_t nj;
static job_t *add(uint32_t kind, const char *name)
{
    job_t *j = &J[nj++];
    memset(j, 0, sizeof *j);
    j->kind = (uint8_t)kind;
    snprintf(j->name, sizeof j->name, "%s", name);
    return j;
}

int main(int argc, char **argv)
{
    const char *gpath = argc > 1 ? argv[1] : "tests/golden.txt";
    const char *cpath = argc > 2 ? argv[2] : "tests/cpu_baseline.txt";
    int gupd = getenv("GOLDEN_UPDATE") != 0, cupd = getenv("BUDGET_UPDATE") != 0, verbose = getenv("VERBOSE") != 0;
    uint32_t jobs_at_once = getenv("JOBS") ? (uint32_t)atoi(getenv("JOBS")) : 8u;
    static const char *const SN[6] = {"dry", "chorus", "delay", "reverb", "all", "dist"};
    static kv_t gold[MAXJ], cpu[MAXJ];
    uint32_t ng, nc, e, pi, i, g0, g1, c0, c1, k0, k;
    uint32_t g_changed = 0, g_new = 0, g_gone = 0, h_fail = 0, c_fail = 0, c_warn = 0, k_fail = 0, crash = 0;
    uint64_t t_start = now_ns();
    char name[64], s[64];
    if (jobs_at_once < 1u || jobs_at_once > 64u)
        jobs_at_once = 8;

    /* 1 + 2: the golden renders */
    g0 = nj;
    for (e = 0; e < NENGINES; e++)
        for (pi = 0; pi < ENGINES[e]->npresets; pi++) {
            job_t *j;
            slug(s, ENGINES[e]->presets[pi].name, sizeof s);
            snprintf(name, sizeof name, "preset/%s/%02u_%s", ENGINES[e]->name, pi, s);
            j = add(J_PRESET, name);
            j->e = (uint8_t)e;
            j->pi = (uint8_t)pi;
        }
    for (k = 0; k < 6u; k++) {
        job_t *j;
        snprintf(name, sizeof name, "sends/ORGAN/%s", SN[k]);
        j = add(J_SENDS, name);
        j->e = ENGI_ORGAN;
        j->arg = (uint8_t)k;
    }
    add(J_SEQ, "seq/ORGAN/16_steps")->e = ENGI_ORGAN;
    g1 = nj;
    {   /* determinism: the first render once more */
        job_t *j = add(J_PRESET, "repeat");
        *j = J[g0];
        snprintf(j->name, sizeof j->name, "repeat");
    }

    /* 4: the key checks */
    k0 = nj;
    add(J_CHECK, "keys: the budget")->check = chk_keys;
    add(J_CHECK, "keys: a held key keeps sounding")->check = chk_keep_key;
    add(J_CHECK, "routing: no hanging notes")->check = chk_hang;
    run_jobs(J, nj, jobs_at_once);

    /* 3: CPU, one child at a time */
    c0 = nj;
    for (e = 0; e < NENGINES; e++)
        for (pi = 0; pi < ENGINES[e]->npresets; pi++) {
            job_t *j;
            slug(s, ENGINES[e]->presets[pi].name, sizeof s);
            snprintf(name, sizeof name, "cpu/%s/%02u_%s", ENGINES[e]->name, pi, s);
            j = add(J_CPU, name);
            j->e = (uint8_t)e;
            j->pi = (uint8_t)pi;
        }
    add(J_CPU, "cpu/mix/idle")->e = 0xFF;
    c1 = nj;
    run_jobs(J + c0, c1 - c0, 1);

    /* ---- report: goldens and health ---- */
    ng = load_kv(gpath, gold, MAXJ);
    for (i = g0; i < g1; i++) {
        job_t *j = &J[i];
        const res_t *r = &j->r;
        const char *want = kv_get(gold, ng, j->name);
        char hs[24], why[256] = "";
        snprintf(hs, sizeof hs, "%016llx", (unsigned long long)r->hash);
        if (j->crashed) {
            printf("regress: CRASH  %s\n", j->name);
            crash++;
            continue;
        }
        if (!want)
            g_new++;
        else if (strcmp(want, hs)) {
            if (!gupd)
                printf("regress: GOLDEN CHANGED  %s  (%s -> %s)\n", j->name, want, hs);
            g_changed++;
        }
        if (r->over)
            snprintf(why + strlen(why), sizeof why - strlen(why), " %u samples beyond 16 bits;", r->over);
        if (r->near)
            snprintf(why + strlen(why), sizeof why - strlen(why), " %u samples >= %d;", r->near, LIM_NEAR);
        if (fabs(r->dc) > LIM_DC)
            snprintf(why + strlen(why), sizeof why - strlen(why), " DC %.1f (limit %d);", r->dc, LIM_DC);
        if (r->peak < LIM_PEAK_MIN)
            snprintf(why + strlen(why), sizeof why - strlen(why), " peak %d (min %d);", r->peak, LIM_PEAK_MIN);
        if (r->free_s < 0)
            snprintf(why + strlen(why), sizeof why - strlen(why), " organ not free %d s after the note-offs;", FREE_CAP_S);
        if (abs(r->tail_dc) > LIM_TAIL_DC)
            snprintf(why + strlen(why), sizeof why - strlen(why), " DC %d left at the end (limit %d);", r->tail_dc,
                     LIM_TAIL_DC);
        if (r->tail_peak > LIM_TAIL)
            snprintf(why + strlen(why), sizeof why - strlen(why), " not silent at the end: peak %d (limit %d);",
                     r->tail_peak, LIM_TAIL);
        if (r->vmax > ORG_NKEY)
            snprintf(why + strlen(why), sizeof why - strlen(why), " %u keys (organ %u);", r->vmax, ORG_NKEY);
        if (why[0]) {
            printf("regress: HEALTH FAIL  %s:%s\n", j->name, why);
            h_fail++;
        }
        if (verbose)
            printf("  %-44s %s  %5.1f s  peak %5d  dc %6.1f  free %5.2f s  tail %3d dc %3d  voices %u\n", j->name,
                   hs, (double)r->frames / FS, r->peak, r->dc, r->free_s, r->tail_peak, r->tail_dc, r->vmax);
    }
    for (i = 0; i < ng; i++) {                      /* in the file, not rendered any more */
        uint32_t found = 0;
        for (k = g0; k < g1 && !found; k++)
            found = !strcmp(J[k].name, gold[i].name);
        if (!found) {
            if (!gupd)
                printf("regress: GOLDEN GONE  %s (no such render now)\n", gold[i].name);
            g_gone++;
        }
    }
    if (J[g1].r.hash != J[g0].r.hash || J[g1].crashed) {
        printf("regress: NOT DETERMINISTIC  %s rendered twice: %016llx, %016llx\n", J[g0].name,
               (unsigned long long)J[g0].r.hash, (unsigned long long)J[g1].r.hash);
        h_fail++;
    }
    if (gupd) {
        FILE *f = fopen(gpath, "w");
        if (!f) {
            perror(gpath);
            return 2;
        }
        fprintf(f, "# Golden renders (tests/regress.c): name, FNV-1a 64 of the output samples.\n"
                   "# Rewritten by GOLDEN_UPDATE=1 -- only for an intended change of the sound.\n");
        for (i = g0; i < g1; i++)
            fprintf(f, "%s %016llx\n", J[i].name, (unsigned long long)J[i].r.hash);
        fclose(f);
        printf("regress: golden file %s rewritten (%u renders; %u had changed, %u new, %u gone)\n", gpath, g1 - g0,
               g_changed, g_new, g_gone);
    } else if (g_new) {
        printf("regress: %u renders not in %s (new: GOLDEN_UPDATE=1 adds them)\n", g_new, gpath);
    }

    /* ---- keys and routing ---- */
    for (i = k0; i < c0; i++) {
        int ok = !J[i].crashed && J[i].r.ok;
        printf("regress: %s: %s %s\n", J[i].name, J[i].crashed ? "crashed" : J[i].r.msg, ok ? "ok" : "FAIL");
        k_fail += !ok;
    }

    /* ---- CPU (a patch's own cost: its count less the idle mix's) ---- */
    nc = load_kv(cpath, cpu, MAXJ);
    {
        double idle_now = J[c1 - 1u].r.ipc, idle_base = kv_get(cpu, nc, J[c1 - 1u].name) ? atof(kv_get(cpu, nc, J[c1 - 1u].name)) : 0;
        for (i = c0; i < c1; i++) {
            const job_t *j = &J[i];
            const char *want = kv_get(cpu, nc, j->name);
            int own = j->e < NENGINES && idle_base > 0;
            double b = want ? atof(want) - (own ? idle_base : 0) : 0, ipc = j->r.ipc - (own ? idle_now : 0);
            if (j->crashed) {
                printf("regress: CRASH  %s\n", j->name);
                crash++;
                continue;
            }
            if (!j->r.ipc || cupd)
                continue;
            if (!want)
                printf("regress: CPU %s: %.0f instructions / sample, no baseline (BUDGET_UPDATE=1 adds it)\n", j->name, ipc);
            else if (ipc > b * (1 + CPU_TOL) && ipc > b + CPU_SLACK) {
                printf("regress: CPU OVER BUDGET  %s: %.0f instructions / sample, baseline %.0f (+%.0f %%, limit +%.0f %%)\n",
                       j->name, ipc, b, (ipc / b - 1) * 100, CPU_TOL * 100);
                c_fail++;
            } else if (ipc < b * (1 - CPU_TOL) && ipc < b - CPU_SLACK) {
                printf("regress: CPU note  %s: %.0f instructions / sample, baseline %.0f (%.0f %%): faster? "
                       "BUDGET_UPDATE=1 to keep it\n", j->name, ipc, b, (ipc / b - 1) * 100);
                c_warn++;
            }
        }
    }
    if (!J[c0].r.ipc)
        printf("regress: CPU: no instruction counter on this host (proc_pid_rusage); the budget is not checked\n");
    else if (cupd) {
        FILE *f = fopen(cpath, "w");
        if (!f) {
            perror(cpath);
            return 2;
        }
        fprintf(f, "# Host CPU baseline (tests/regress.c): instructions per 44.1 kHz sample, cc -O2 on the Mac\n"
                   "# (kernel-counted, ~1 %% run to run). The check allows +%.0f %%. Rewritten by BUDGET_UPDATE=1.\n",
                CPU_TOL * 100);
        for (i = c0; i < c1; i++)
            fprintf(f, "%s %.0f\n", J[i].name, J[i].r.ipc);
        fclose(f);
        printf("regress: CPU baseline %s rewritten (%u entries)\n", cpath, c1 - c0);
    }

    printf("regress: %u golden renders (%u changed, %u gone), %u health failures, %u key / routing checks failed, "
           "%u CPU entries over budget (%u notes), %u crashes; %.1f s\n", g1 - g0, gupd ? 0 : g_changed,
           gupd ? 0 : g_gone, h_fail, k_fail, cupd ? 0 : c_fail, c_warn, crash, (double)(now_ns() - t_start) / 1e9);
    if (!gupd && (g_changed || g_gone))
        printf("regress: the sound changed. If that is intended: GOLDEN_UPDATE=1, then review and commit tests/golden.txt\n");
    return (!gupd && (g_changed || g_gone)) || h_fail || k_fail || (!cupd && c_fail) || crash;
}
