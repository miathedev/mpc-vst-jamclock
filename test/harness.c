/* harness.c - offline functional harness for the JamClock engine, real-time paced.
 *
 * Build the engine with -DJAMCLOCK_HARNESS: its alsa_send() then calls jclk_log_event() (below)
 * instead of opening ALSA, so every emitted MIDI event (START/CONTINUE/STOP/CLOCK/SPP) lands in
 * a log with a CLOCK_MONOTONIC timestamp. The harness feeds synthetic signals through
 * process() in 128-frame blocks paced on the wall clock, polls the bpm/status readouts, and
 * prints one CSV line of metrics (see run_ab.sh for the A/B table).
 *
 * Usage: harness <scenario>
 *   click60 click90 click120 click174   synthetic click track, lock < 10 s, +-0.5 BPM
 *   drums drums_lp120 drums_lp80        kick+hats drumloop, optionally through a biquad LP
 *   sweep                               drums 90 -> 130 BPM over the run, no octave jumps
 *   steady120                           45 s at 120 BPM: drift ppm + tick-to-tick jitter
 *   steady120_5min                      300 s at 120 BPM: long-run drift (not in the default
 *                                       A/B - run explicitly, ~5 min)
 *   silence                             10 s drums, 6 s silence, 10 s drums: STOP then CONTINUE
 *   volllast                            feed flat-out (ring fills faster than the worker
 *                                       drains), log max process() duration + ring-full
 *                                       events, then a realtime drums tail to prove the engine
 *                                       still locks. Extra CSV columns (volllast lines only):
 *                                       max_process_us,ring_full_calls,ring_full_frames
 *
 * CSV columns (header in run_ab.sh): scenario,dur_s,t_lock_s,bpm_readout,bpm_ticks,bpm_err,
 * octave_ratio,rate_ppm,drift_ppm,drift_ppm_stallfree,jitter_pct,jitter_p95_pct,jitter_rms_pct,
 * sweep_max_err_pct,octave_jumps,n_start,n_cont,n_stop,n_spp,pass_bad,final_status - drift_ppm
 * and drift_ppm_stallfree stay empty for sweep/volllast (meaningless there).
 * drift_ppm_stallfree accumulates only deltas <= 1.1x median of the steady segment and shows
 * the drift without host stalls; jitter_p95_pct is symmetric over |d-med|.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>
#include <dirent.h>
#include <sched.h>
#include <sys/syscall.h>
#include "engine.h"

extern const mpc_engine_t *mpc_engine(void);

#define FS 44100.0

/* ---- event log (written by the engine's worker thread under JAMCLOCK_HARNESS) ---- */
typedef struct { int type; uint64_t ns; int spp; } ev_t;
#define EV_CAP (1 << 20)
static ev_t evlog[EV_CAP];
static atomic_uint ev_head;
void jclk_log_event(int type, uint64_t ns) {
    unsigned h = atomic_load_explicit(&ev_head, memory_order_relaxed);
    if (h >= EV_CAP) return;
    evlog[h].type = type; evlog[h].ns = ns; evlog[h].spp = -1;
    atomic_store_explicit(&ev_head, h + 1, memory_order_release);
}
void jclk_log_spp(int spp, uint64_t ns) {
    unsigned h = atomic_load_explicit(&ev_head, memory_order_relaxed);
    if (h >= EV_CAP) return;
    evlog[h].type = 20; evlog[h].ns = ns; evlog[h].spp = spp;
    atomic_store_explicit(&ev_head, h + 1, memory_order_release);
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
static void sleep_until(uint64_t deadline) {
    struct timespec ts = { (time_t)(deadline / 1000000000ULL), (long)(deadline % 1000000000ULL) };
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
}

/* ---- signal generators ---- */

typedef struct { double b0, b1, b2, a1, a2, x1, x2, y1, y2; } biquad_t;
static void bq_lp(biquad_t *b, double fc) {
    double w0 = 2.0 * M_PI * fc / FS, cw = cos(w0), sw = sin(w0), alpha = sw / (2.0 * 0.7071);
    double a0 = 1.0 + alpha;
    b->b0 = (1.0 - cw) / 2.0 / a0; b->b1 = (1.0 - cw) / a0; b->b2 = b->b0;
    b->a1 = -2.0 * cw / a0;        b->a2 = (1.0 - alpha) / a0;
    b->x1 = b->x2 = b->y1 = b->y2 = 0.0;
}
static double bq_run(biquad_t *b, double x) {
    double y = b->b0 * x + b->b1 * b->x1 + b->b2 * b->x2 - b->a1 * b->y1 - b->a2 * b->y2;
    b->x2 = b->x1; b->x1 = x; b->y2 = b->y1; b->y1 = y;
    return y;
}

enum { K_CLICK, K_DRUMS, K_SWEEP, K_VOLL };
typedef struct {
    const char *name; double dur; int kind; int bias;
    double bpm0, bpm1;        /* sweep: bpm0 -> bpm1 over dur */
    double lp_fc;             /* 0 = no LP */
    double sil_from, sil_to;  /* silence window in seconds (signal zero there) */
    double burst_s;           /* K_VOLL: seconds of flat-out feeding before the realtime tail */
    biquad_t lp;
} scen_t;

static const scen_t SCEN[] = {
    /* click60 needs bias=Half: with a plain click every 1 s, BTT's octave-enhanced
     * autocorrelation also peaks at 120, and only the prior disambiguates - Half anchors
     * it at 60 (bpm 0->120->/2; geo-mid of the 40..240 range is sqrt(40*240)=97.98) */
    { "click60",    12.0, K_CLICK, 1,  60.0,   0.0,   0.0, -1.0, -1.0,  0.0, .lp = {0} },
    { "click90",    12.0, K_CLICK, 0,  90.0,   0.0,   0.0, -1.0, -1.0,  0.0, .lp = {0} },
    { "click120",   12.0, K_CLICK, 0, 120.0,   0.0,   0.0, -1.0, -1.0,  0.0, .lp = {0} },
    { "click174",   12.0, K_CLICK, 0, 174.0,   0.0,   0.0, -1.0, -1.0,  0.0, .lp = {0} },
    { "drums",      12.0, K_DRUMS, 0, 120.0,   0.0,   0.0, -1.0, -1.0,  0.0, .lp = {0} },
    { "drums_lp120",12.0, K_DRUMS, 0, 120.0,   0.0, 120.0, -1.0, -1.0,  0.0, .lp = {0} },
    { "drums_lp80", 12.0, K_DRUMS, 0, 120.0,   0.0,  80.0, -1.0, -1.0,  0.0, .lp = {0} },
    { "sweep",      18.0, K_SWEEP, 0,  90.0, 130.0,   0.0, -1.0, -1.0,  0.0, .lp = {0} },
    { "steady120",  45.0, K_DRUMS, 0, 120.0,   0.0,   0.0, -1.0, -1.0,  0.0, .lp = {0} },
    { "steady120_5min", 300.0, K_DRUMS, 0, 120.0, 0.0, 0.0, -1.0, -1.0,  0.0, .lp = {0} },
    { "silence",    26.0, K_DRUMS, 0, 120.0,   0.0,   0.0, 10.0, 16.0,  0.0, .lp = {0} },
    /* volllast: 8 s flat-out feed (ring fills faster than the worker drains under
     * load/ASan), then the remaining dur is fed realtime-paced to prove recovery+lock */
    { "volllast",   20.0, K_VOLL,  0, 120.0,   0.0,   0.0, -1.0, -1.0,  8.0, .lp = {0} },
};

static double bpm_at(const scen_t *sc, double t) {
    if (sc->kind == K_SWEEP) {
        double f = t / sc->dur; if (f > 1.0) f = 1.0;
        return sc->bpm0 + (sc->bpm1 - sc->bpm0) * f;
    }
    return sc->bpm0;
}

/* Beat phase as the INTEGRAL of the instantaneous frequency: at constant sweep rate
 * slope = (bpm1-bpm0)/dur [BPM/s], beat(t) = (bpm0*t + 0.5*slope*t^2)/60 must hold,
 * because only then d(beat)/dt*60 = bpm0 + slope*t = bpm_at(t) is the true instantaneous
 * tempo of the signal. (Previously t*bpm_at(t)/60 stood here - not an antiderivative; the
 * signal therefore really swept 90->~170 BPM instead of the labeled 90->130, and the
 * sweep metrics were computed against a phantom ground truth.) The truth label at segment
 * end T is bpm0 + slope*T = bpm_at(T) - bpm_at() thus remains the correct truth. */
static double beat_phase(const scen_t *sc, double t) {
    if (sc->kind == K_SWEEP) {
        double slope = (sc->bpm1 - sc->bpm0) / sc->dur;
        return (sc->bpm0 * t + 0.5 * slope * t * t) / 60.0;
    }
    return t * sc->bpm0 / 60.0;
}

static double gen_sample(const scen_t *sc, uint64_t i) {
    double t = (double)i / FS;
    if (t >= sc->sil_from && t < sc->sil_to && sc->sil_from >= 0.0) return 0.0;
    double bpm = bpm_at(sc, t);
    double beat = beat_phase(sc, t);
    double s = 0.0;
    if (sc->kind == K_CLICK) {
        double ph = beat - floor(beat);            /* 0..1 within the beat */
        if (ph < 0.02) s = 0.9 * sin(2.0 * M_PI * 1800.0 * t) * exp(-ph * 250.0);
    } else {
        /* Time since beat/8th from the beat PHASE, not t - floor(beat)*60/bpm: that only
         * holds at constant tempo. During the sweep (beat = integral of the instantaneous
         * frequency) floor(beat)*60/bpm is computed against the *running* bpm and tt lags
         * the real beat (at 105 BPM tt was 0.75 s > one period -> the kick no longer fires,
         * the signal goes silent, the engine drops to STOPPED). ph*60/bpm is exactly the
         * old tt at constant tempo, and the locally correct approximation during the sweep. */
        double ph  = beat - floor(beat);
        double ph8 = beat * 2.0 - floor(beat * 2.0);
        double tt  = ph  * 60.0 / bpm;                           /* s since quarter */
        double tt8 = ph8 * 30.0 / bpm;                           /* s since 8th */
        if (tt < 0.30) s += exp(-tt * 25.0) * sin(2.0 * M_PI * 55.0 * tt);      /* kick */
        if (tt8 < 0.04) s += sin(2.0 * M_PI * 8000.0 * t) * exp(-tt8 * 200.0) * 0.12; /* hat */
    }
    if (sc->lp_fc > 0.0) s = bq_run((biquad_t *)&sc->lp, s);
    return s;
}

/* ---- metrics ---- */
static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}
static double median(uint64_t *v, int n) {
    qsort(v, n, sizeof *v, cmp_u64);
    return n ? (double)v[n / 2] : 0.0;
}

/* Verify the engine's worker thread really runs SCHED_OTHER and off CPU 3.
 * Finds the thread named "jamclock" in /proc/self/task and reads its policy + affinity.
 * Returns 1 if the thread was found AND is in the target state (SCHED_OTHER, cpu3 excluded);
 * the last observation lands in sched_last (for the final CHECK after the retries).
 * A single scan races with the worker (pthread_setname_np("jamclock") and
 * worker_demote() run only shortly after pthread_create) - see log_worker_sched(). */
static char sched_last[256];
static int log_worker_sched_once(void) {
    DIR *d = opendir("/proc/self/task");
    if (!d) { snprintf(sched_last, sizeof sched_last, "  worker-sched: no /proc/self/task"); return 0; }
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char p[512], comm[64] = "";   /* d_name is up to 255 bytes -> buffer sized for -Wformat-truncation */
        snprintf(p, sizeof p, "/proc/self/task/%s/comm", de->d_name);
        FILE *f = fopen(p, "r");
        if (f) { if (fgets(comm, sizeof comm, f)) { comm[strcspn(comm, "\n")] = 0; } fclose(f); }
        if (strcmp(comm, "jamclock")) continue;
        long tid = atol(de->d_name);
        int pol = (int)syscall(SYS_sched_getscheduler, tid);
        const char *pn = pol == SCHED_OTHER ? "SCHED_OTHER" : pol == SCHED_FIFO ? "SCHED_FIFO"
                       : pol == SCHED_RR ? "SCHED_RR" : pol == SCHED_BATCH ? "SCHED_BATCH"
                       : pol == SCHED_IDLE ? "SCHED_IDLE" : "?";
        cpu_set_t cs;
        CPU_ZERO(&cs);
        char cpus[128] = "";
        if (sched_getaffinity(tid, sizeof cs, &cs) == 0) {
            int off = 0;
            for (int c = 0; c < CPU_SETSIZE; c++)
                if (CPU_ISSET(c, &cs)) off += snprintf(cpus + off, sizeof cpus - off, "%s%d", off ? "," : "", c);
        }
        int on3 = sched_getaffinity(tid, sizeof cs, &cs) == 0 && CPU_ISSET(3, &cs);
        int ok = (pol == SCHED_OTHER && !on3);
        snprintf(sched_last, sizeof sched_last,
                 "  worker-sched: tid=%ld policy=%d(%s) cpu3_excluded=%s cpus=%s -> %s",
                 tid, pol, pn, on3 ? "NO" : "yes", cpus, ok ? "OK" : "CHECK");
        closedir(d);
        if (ok) { fprintf(stderr, "%s\n", sched_last); return 1; }
        return 0;   /* found, but not demoted yet - retry */
    }
    closedir(d);
    return 0;       /* thread not yet named "jamclock" - retry */
}

static void log_worker_sched(void) {
    /* Retry up to 500 ms in 50 ms steps until comm=="jamclock" is found AND the worker is
     * SCHED_OTHER/off-cpu3 - otherwise a false CHECK or a missing line. Still raises an
     * alarm after 500 ms if the worker is not in the target state then (a real problem,
     * e.g. a v0.1 baseline without the affinity fix). */
    sched_last[0] = 0;
    for (int tries = 0; tries < 10; tries++) {
        if (log_worker_sched_once()) return;
        struct timespec ts = { 0, 50L * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    if (sched_last[0])
        fprintf(stderr, "%s (after 500 ms retry)\n", sched_last);
    else
        fprintf(stderr, "  worker-sched: thread \"jamclock\" not found in /proc/self/task after 500 ms -> CHECK\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <scenario>\n", argv[0]); return 2; }
    const scen_t *sc = NULL;
    for (unsigned i = 0; i < sizeof SCEN / sizeof SCEN[0]; i++)
        if (!strcmp(SCEN[i].name, argv[1])) sc = &SCEN[i];
    if (!sc) { fprintf(stderr, "unknown scenario %s\n", argv[1]); return 2; }
    scen_t g = *sc;
    if (g.lp_fc > 0.0) bq_lp(&g.lp, g.lp_fc);

    const mpc_engine_t *eng = mpc_engine();
    void *inst = eng->create(NULL);
    if (!inst) { fprintf(stderr, "create failed\n"); return 1; }
    eng->set_param(inst, "enable", "1");
    {
        char b[8];
        snprintf(b, sizeof b, "%d", g.bias);
        eng->set_param(inst, "bias", b);
    }
    eng->set_param(inst, "minbpm", "40");
    eng->set_param(inst, "maxbpm", "240");
    eng->set_param(inst, "silence", "3");
    if (getenv("HARNESS_PRIOR")) eng->set_param(inst, "prior", getenv("HARNESS_PRIOR"));
    if (getenv("HARNESS_BIAS")) eng->set_param(inst, "bias", getenv("HARNESS_BIAS"));

    log_worker_sched();   /* worker must be SCHED_OTHER and off CPU 3 */

    uint64_t t0 = now_ns();
    double burst_audio_s = (g.kind == K_VOLL) ? g.burst_s : 0.0;  /* fed FLAT-OUT first */
    uint64_t total = (uint64_t)((g.dur + burst_audio_s) * FS);
    uint64_t burst_end_i = (uint64_t)(burst_audio_s * FS);         /* sample index where pacing starts */
    uint64_t next_poll = t0 + 50000000ULL;
    double t_lock = -1.0, last_bpm_readout = 0.0, sweep_max_err_pct = 0.0;
    int octave_jumps = 0, pass_bad = 0;
    char status[64] = "", bpmbuf[64] = "", drops[64] = "";
    uint64_t max_proc_ns = 0;
    unsigned drop_calls0 = 0, drop_frames0 = 0;   /* drops before the volllast burst */
    int pacing = 0;
    uint64_t paced_base = 0, paced_samples = 0;
    uint64_t i = 0;
    for (uint64_t blk = 0; blk * 128 < total; blk++) {
        int16_t in[256], out[256];
        for (int k = 0; k < 128; k++, i++) {
            double s = gen_sample(&g, i);
            if (s > 1.0) s = 1.0;
            if (s < -1.0) s = -1.0;
            int16_t v = (int16_t)(s * 32767.0);
            in[2 * k] = v; in[2 * k + 1] = v;
        }
        uint64_t p0 = now_ns();
        eng->process(inst, in, out, 128);
        uint64_t pd = now_ns() - p0;                 /* volllast: process() must stay bounded */
        if (pd > max_proc_ns) max_proc_ns = pd;
        if (memcmp(in, out, sizeof in)) pass_bad++;
        if (i <= burst_end_i) {                     /* volllast burst: flat-out, no pacing/poll */
            if (i == 128) { drops[0] = 0; eng->get_param(inst, "drops", drops, sizeof drops);
                            sscanf(drops, "%u %u", &drop_calls0, &drop_frames0); }
            continue;
        }
        if (!pacing) {                              /* burst over: restart realtime pacing + metrics window */
            pacing = 1; paced_base = now_ns(); paced_samples = 0; t0 = paced_base;
            /* t0 reset: the volllast line measures t_lock from HERE (~0.2 s) - an artifact of
             * the window restart; the lock already happened during the burst. Not comparable
             * with the ~4 s t_lock values of the other scenarios. */
            next_poll = t0 + 50000000ULL;
            /* print only for K_VOLL - otherwise burst_end_i=0 and the block fires once for
             * every scenario with meaningless values. */
            if (g.kind == K_VOLL && getenv("HARNESS_DEBUG")) {
                drops[0] = 0; eng->get_param(inst, "drops", drops, sizeof drops);
                unsigned dc = 0, df = 0; sscanf(drops, "%u %u", &dc, &df);
                fprintf(stderr, "  volllast burst done: drops during burst: calls=%u frames=%u (of %u before), max_process=%.1fus\n",
                        dc - drop_calls0, df - drop_frames0, drop_calls0, max_proc_ns / 1e3);
            }
        }
        paced_samples += 128;
        uint64_t deadline = paced_base + (uint64_t)(paced_samples * 1e9 / FS);
        for (;;) {
            uint64_t now = now_ns();
            if (now >= next_poll) {
                next_poll = now + 50000000ULL;
                status[0] = 0; eng->get_param(inst, "status", status, sizeof status);
                bpmbuf[0] = 0; eng->get_param(inst, "bpm", bpmbuf, sizeof bpmbuf);
                if (!strcmp(status, "LOCKED") && t_lock < 0.0)
                    t_lock = (double)(now - t0) / 1e9;
                if (bpmbuf[0] >= '0' && bpmbuf[0] <= '9') {
                    double rb = atof(bpmbuf);
                    double truth = bpm_at(&g, (double)i / FS);
                    if (t_lock >= 0.0 && truth > 0.0 && rb > 0.0 &&
                        fabs(log2(rb / truth)) > 0.9) octave_jumps++;
                    if (g.kind == K_SWEEP && t_lock >= 0.0 &&
                        (double)(now - t0) / 1e9 > t_lock + 2.0 && rb > 0.0) {
                        double e = 100.0 * fabs(rb - truth) / truth;
                        if (e > sweep_max_err_pct) sweep_max_err_pct = e;
                    }
                    if (getenv("HARNESS_DEBUG"))
                        fprintf(stderr, "POLL t=%.2f status=%s bpm=%.1f (truth %.1f)\n",
                                (double)(now - t0) / 1e9, status, rb, truth);
                    last_bpm_readout = rb;
                }
            }
            if (now >= deadline) break;
            sleep_until(deadline > now + 2000000ULL ? deadline - 1000000ULL : now + 500000ULL);
        }
    }
    sleep_until(now_ns() + 300000000ULL);   /* let the worker flush */
    eng->get_param(inst, "status", status, sizeof status);
    eng->get_param(inst, "bpm", bpmbuf, sizeof bpmbuf);
    drops[0] = 0; eng->get_param(inst, "drops", drops, sizeof drops);   /* before destroy */
    eng->destroy(inst);

    /* metrics from the event log */
    unsigned n = atomic_load(&ev_head);
    if (getenv("HARNESS_DEBUG")) {
        for (unsigned k = 0; k < n && k < EV_CAP; k++)
            fprintf(stderr, "EV %2d t=%8.3f ms spp=%d\n", evlog[k].type,
                    (double)(evlog[k].ns - t0) / 1e6, evlog[k].spp);
    }
    int n_start = 0, n_cont = 0, n_stop = 0, n_spp = 0;
    uint64_t first_start = 0, last_ev = 0;
    static uint64_t clk[EV_CAP / 4];
    int n_clk = 0;
    for (unsigned k = 0; k < n && k < EV_CAP; k++) {
        ev_t *e = &evlog[k];
        if (e->ns < t0) continue;
        if (e->type == 30) { n_start++; if (!first_start) first_start = e->ns; }
        else if (e->type == 31) { n_cont++; if (!first_start) first_start = e->ns; }
        else if (e->type == 32) n_stop++;
        else if (e->type == 20) n_spp++;
        else if (e->type == 36) { if (n_clk < (int)(EV_CAP / 4)) clk[n_clk++] = e->ns; }
        if (e->ns > last_ev) last_ev = e->ns;
    }
    /* steady metrics over the longest uninterrupted run of the clock stream after lock+2 s
     * (a STOP/CONTINUE gap must not pollute drift/jitter - segment on gaps > 3x median) */
    double bpm_ticks = 0.0, drift_ppm = 0.0, drift_sf_ppm = 0.0, jitter_pct = 0.0, jitter_p95_pct = 0.0, jitter_rms_pct = 0.0;
    uint64_t win_lo = first_start ? first_start + 2000000000ULL : 0;
    uint64_t win_hi = last_ev > 500000000ULL ? last_ev - 500000000ULL : 0;
    int m = 0;
    static uint64_t d[EV_CAP / 4];
    for (int k = 0; k < n_clk; k++)
        if (clk[k] >= win_lo && clk[k] <= win_hi) d[m++] = clk[k];
    if (m >= 10) {
        static uint64_t dd[EV_CAP / 4];   /* tick deltas in time order */
        static uint64_t ds[EV_CAP / 4];   /* scratch copy for median() - it sorts in place */
        int nd = 0;
        for (int k = 1; k < m; k++) dd[nd++] = d[k] - d[k - 1];
        memcpy(ds, dd, (size_t)nd * sizeof *dd);
        double med = median(ds, nd);
        /* longest run of deltas within [0.5, 2]x median (the steady segment) - the
         * segmentation runs on the time-ordered deltas in dd, not on the sorted copy ds */
        int best_lo = 0, best_len = 0, run_lo = 0;
        for (int k = 0; k < nd; k++) {
            if (dd[k] < med * 0.5 || dd[k] > med * 2.0) {
                if (k - run_lo > best_len) { best_len = k - run_lo; best_lo = run_lo; }
                run_lo = k + 1;
            }
        }
        if (nd - run_lo > best_len) { best_len = nd - run_lo; best_lo = run_lo; }
        static uint64_t sd[EV_CAP / 4];
        int ns_ = 0;
        double sum = 0.0;
        for (int k = best_lo; k < best_lo + best_len; k++) { sd[ns_++] = dd[k]; sum += (double)dd[k]; }
        med = median(sd, ns_);
        bpm_ticks = 60.0e9 / (24.0 * med);   /* tick_ns = 60e9/(bpm*24) */
        double maxdev = 0.0;
        for (int k = 0; k < ns_; k++) { double dev = fabs((double)sd[k] - med); if (dev > maxdev) maxdev = dev; }
        /* p95 + RMS in addition to max: in the sandbox, single ms stalls of the VM produce
         * max jitter of 46-68 %; p95/RMS over the same steady segment show the real engine
         * jitter (the <5 % target is provable on-device via aseqdump).
         * p95 is SYMMETRIC over |d-med| - like max, RMS and the on-device awk (max|d-mean|);
         * the old version took the 95% percentile of the sorted deltas and thus only saw
         * the upper tail. */
        {
            static uint64_t srt[EV_CAP / 4];
            for (int k = 0; k < ns_; k++) {
                double dv = fabs((double)sd[k] - med);
                srt[k] = (uint64_t)dv;             /* med is integral (median() returns v[n/2]) */
            }
            qsort(srt, ns_, sizeof *srt, cmp_u64);
            double p95dev = (double)srt[(int)(0.95 * (ns_ - 1))];
            double sq = 0.0;
            for (int k = 0; k < ns_; k++) { double dv = (double)sd[k] - med; sq += dv * dv; }
            double rmsdev = sqrt(sq / ns_);
            jitter_pct = med > 0.0 ? 100.0 * maxdev / med : 0.0;
            jitter_p95_pct = med > 0.0 ? 100.0 * p95dev / med : 0.0;
            jitter_rms_pct = med > 0.0 ? 100.0 * rmsdev / med : 0.0;
            if (getenv("HARNESS_DEBUG"))
                fprintf(stderr, "  metrics: m=%d steady_run=%d med=%.3fms maxdev=%.3fms (%.2f%%) p95=%.2f%% rms=%.2f%%\n",
                        m, ns_, med / 1e6, maxdev / 1e6, jitter_pct, jitter_p95_pct, jitter_rms_pct);
        }
        /* drift_ppm is mean-based (incl. host stalls) and only meaningful at CONSTANT tempo -
         * meaningless for sweep (tempo change inside the measurement window) and volllast
         * (burst), the sign flips against the rate -> leave the field empty */
        if (g.kind != K_SWEEP && g.kind != K_VOLL) {
            double ideal = 60.0e9 / (24.0 * g.bpm0);
            drift_ppm = (sum - (double)ns_ * ideal) / ((double)ns_ * ideal) * 1e6;
            /* drift_ppm_stallfree - accumulates only deltas <= 1.1x median of the steady
             * segment, i.e. without host stalls (measured: contended host +1.6k ppm in
             * drift_ppm, engine itself +3..+5 ppm stall-free, 1/14198 ticks dropped).
             * med/sd/ns_ here are the steady segment (computed above). */
            double sfs = 0.0;
            int sfn = 0;
            for (int k = 0; k < ns_; k++)
                if ((double)sd[k] <= med * 1.1) { sfs += (double)sd[k]; sfn++; }
            if (sfn > 0)
                drift_sf_ppm = (sfs - (double)sfn * ideal) / ((double)sfn * ideal) * 1e6;
        }
    }
    double truth_end = bpm_at(&g, g.dur - 0.1);   /* segment end: bpm0 + slope*T */
    double bpm_err = bpm_ticks > 0.0 ? bpm_ticks - truth_end : 0.0;
    double octave = bpm_ticks > 0.0 && truth_end > 0.0 ? bpm_ticks / truth_end : 0.0;
    double rate_ppm = (bpm_ticks > 0.0 && truth_end > 0.0) ? bpm_err / truth_end * 1e6 : 0.0;
    char drift_s[32] = "", drift_sf_s[32] = "";
    if (g.kind != K_SWEEP && g.kind != K_VOLL) {
        snprintf(drift_s, sizeof drift_s, "%.1f", drift_ppm);
        snprintf(drift_sf_s, sizeof drift_sf_s, "%.1f", drift_sf_ppm);
    }

    printf("%s,%.1f,%.2f,%.1f,%.2f,%.3f,%.4f,%.1f,%s,%s,%.2f,%.2f,%.2f,%.1f,%d,%d,%d,%d,%d,%d,%s",
           g.name, g.dur, t_lock, last_bpm_readout, bpm_ticks, bpm_err, octave, rate_ppm, drift_s, drift_sf_s,
           jitter_pct, jitter_p95_pct, jitter_rms_pct, sweep_max_err_pct, octave_jumps,
           n_start, n_cont, n_stop, n_spp, pass_bad, status);
    if (g.kind == K_VOLL) {
        /* extra columns for the volllast scenario: max process() duration and
         * ring-full drops - no crash + bounded process() = PASS */
        unsigned dc = 0, df = 0;
        sscanf(drops, "%u %u", &dc, &df);
        printf(",%.1f,%u,%u", max_proc_ns / 1e3, dc - drop_calls0, df - drop_frames0);
    }
    printf("\n");
    fprintf(stderr, "  -> %s: t_lock=%.2fs bpm_ticks=%.2f (truth %.1f) rate=%.1fppm drift=%s stallfree=%s jitter max/p95/rms=%.2f/%.2f/%.2f%% start=%d cont=%d stop=%d spp=%d pass_bad=%d status=%s",
            g.name, t_lock, bpm_ticks, truth_end, rate_ppm, drift_s[0] ? drift_s : "n/a",
            drift_sf_s[0] ? drift_sf_s : "n/a",
            jitter_pct, jitter_p95_pct, jitter_rms_pct,
            n_start, n_cont, n_stop, n_spp, pass_bad, status);
    if (g.kind == K_VOLL) {
        unsigned dc = 0, df = 0;
        sscanf(drops, "%u %u", &dc, &df);
        fprintf(stderr, " max_process=%.1fus drops_calls=%u drops_frames=%u (total incl. burst)",
                max_proc_ns / 1e3, dc - drop_calls0, df - drop_frames0);
    }
    fprintf(stderr, "\n");
    return 0;
}
