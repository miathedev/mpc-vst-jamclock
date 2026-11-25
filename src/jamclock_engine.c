/* JamClock v0.2 - "fake effect" insert for the MPC: a listen-only effect (bit-identical
 * passthrough) that sits on an input track (e.g. Input 3/4), beat-tracks the incoming live
 * audio and emits MIDI Clock (24 ppq) + Start/Continue/Stop + SPP on its own ALSA sequencer
 * port ("JamClock MIDI Out"). `aconnect` that port to the MPC app's MIDI input and the MPC
 * follows the band's tempo (software MIDI loopback).
 *
 * v0.2 replaces the hand-rolled autocorrelation tracker of v0.1 with the vendored BTT
 * (src/btt/, Krzyzaniak MIT, upstream c039090 + the 4 AudioClock patches) and replaces the
 * per-tick-recomputed clock with a drift-free split clock (period/phase split as in the
 * AudioClock project):
 *
 *   Tracker: mono downmix -> btt_process() (STFT hop 128, spectral-flux onset, generalized
 *   autocorrelation + CBSS pulse-pattern cross-correlation, decaying Gaussian tempo histogram
 *   with log-Gaussian tempo prior). bias Auto/Half/Double maps to the prior mean (Half = bpm/2,
 *   Double = bpm*2); mean and width are always set together with a fixed width RATIO (0.625
 *   default) because the width setter stores (bpm/mean)^2. minbpm/maxbpm map to
 *   btt_set_min/max_tempo(). Silence is RMS-based, never beat-absence.
 *
 *   Clock: PERIOD comes straight from btt_get_beat_period_audio_samples() (BTT already smooths
 *   via its histogram - no second integrator). PHASE is corrected proportionally (alpha 0.20)
 *   from beat callbacks; a correction moves only the END of the current beat, the 24 ticks
 *   stretch/squeeze and never move backwards. Span is clamped to 0.5..1.5x period and a span
 *   floor keeps pending tick due-times in the future. Three phase errors > 30% in a row
 *   trigger a hard resync (one big error is usually syncopation). Ticks are sent on absolute
 *   CLOCK_MONOTONIC deadlines (worker sleeps until the next due tick - lookahead, never busy
 *   catch-up: > 2 ticks overdue are dropped, not burst). Beat sample-times are mapped to wall
 *   clock through a virtual audio clock (advances per fed sample, pulled 2% toward
 *   CLOCK_MONOTONIC whenever the ring is empty). Transport: first lock -> SPP + START, resume
 *   after silence -> SPP + CONTINUE (btt_clear() is NOT called on silence-stop, so the tempo
 *   is kept), silence > `silence` s (RMS) -> STOP, enable off -> STOP. SPP counts 6 clocks per
 *   unit (1/16 note).
 *
 * RT safety: create() only callocs and spawns the worker; process() only memmoves the
 * passthrough and pushes into a lock-free SPSC ring - no dlopen, no ALSA, no file I/O, no
 * malloc, no locks, bounded loops on the audio callback.
 *   - the worker is FORCED to SCHED_OTHER at thread birth (pthread_attr, not inherited -
 *     create() may run on the SCHED_FIFO audio callback). Affinity off the audio core (CPU 3 on
 *     Gen1, fast cluster 4-7 on Gen2) is set ONLY in-worker by worker_demote(): create() must not issue syscalls
 *     (sched_getaffinity for the affinity attr would be one on the possible SCHED_FIFO
 *     callback; RT rule: create() = small calloc + thread spawn only), so the affinity attr
 *     block lives entirely in the worker, where sched_getaffinity/sched_setaffinity are
 *     allowed. Best effort: failures are TRACE-logged only, the host test must keep working
 *     on x86.
 *   - the 2 MB SPSC ring is malloc'ed in the WORKER (SCHED_OTHER - malloc is allowed there),
 *     not in create(); process() drops until the ring pointer is atomically published
 *     (startup milliseconds without analysis ring are ok - passthrough is untouched).
 *   - rlock (taken by get_param on the RT callback while the worker holds it in publish())
 *     is initialized with PTHREAD_PRIO_INHERIT - the documented minimum against priority
 *     inversion.
 * The worker thread (SCHED_OTHER, nice 10) does ALL other work: dlopen(libasound), seq port,
 * BTT (btt_process does no allocation - checked), MIDI sends.
 *
 * Engine contract (wrapper/engine.h): 44100 Hz, interleaved int16 stereo, 128-frame blocks;
 * process() gets a non-NULL, non-aliasing in_lr (the wrapper copies), but memmove anyway.
 * "state" chunk = "jclk2\nkey=value..." (reads jclk1 of v0.1 too; magic-checked, truncated last
 * line dropped; new keys lookahead_ms, prior, sil_rms have defaults, so old presets load
 * unchanged). */
/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 mime */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <sys/resource.h>

#include "engine.h"
#include "alsa_seq_min.h"
#include "btt_wrap.h"

#define FS          44100.0
#define BTT_HOP     128                        /* BTT STFT hop (1024 FFT, overlap 8) */
#define RMS_RING    (16 * 44100 / 128 + 2)     /* per-hop RMS ring (~16 s) */
#define RING_CAP    524288                     /* stereo frames in the audio ring (~11.9 s) */
#define RING_MASK   (RING_CAP - 1)
#define FIFO_CAP    65536
#define FIFO_MASK   (FIFO_CAP - 1)              /* FIFO_CAP is 2^16 - ring wrap via mask */
#define TICK_MS     10                         /* worker work tick */
#define PPQN        24                         /* MIDI clocks per quarter */
#define SIL_RMS_DEF 1e-4f                      /* default silence threshold (~-80 dBFS hop RMS);
                                                 * chunk key sil_rms overrides it */
#define MIN_LOCK_S  4.0                        /* need >= 4 s of audio before locking */
#define PHASE_ALPHA 0.20                       /* proportional phase correction per beat */
#define PHASE_SLIP_LIMIT 0.30                  /* error > 30% of period counts as a slip */
#define PHASE_SLIP_COUNT 3                     /* slips in a row -> hard resync */
#define SPAN_MIN_FRAC 0.5
#define SPAN_MAX_FRAC 1.5

/* The big analysis buffers (fifo 256 KB + rms ~22 KB) are NOT in Inst any more - they would
 * have bloated create()'s calloc to ~285 KB on the possible SCHED_FIFO callback. They live in
 * this block, which the WORKER mallocs and publishes atomically (exactly like the SPSC ring);
 * process() never touches them (only work() uses fifo/rms). */
typedef struct {
    float fifo[FIFO_CAP];       /* mono downmix, ring semantics (read index) */
    float rms[RMS_RING];        /* per-BTT-hop RMS ring (~16 s) for silence detection */
} worker_buf_t;

typedef struct {
    /* parameter slots (strings, as the wrapper sends them) */
    char enable[16], bias[16], minbpm[16], maxbpm[16], silence[16];
    char lookahead_ms[16], prior[16], sil_rms[16];   /* chunk-only keys (defaults below) */
    atomic_int en_i, bias_i, minbpm_i, maxbpm_i, silence_i, reset_req;
    atomic_int lookahead_i, prior_x1000, sil_rms_x1e6;

    /* SPSC ring: audio thread = producer (head), worker = consumer (tail); int16 stereo frames.
     * RT rule: the buffer is malloc'ed in the WORKER, never in create() - process() reads the
     * pointer atomically and drops until it is non-NULL (see process()). */
    _Atomic(int16_t *) ring;
    atomic_uint head, tail;
    atomic_uint drop_calls, drop_frames;   /* process() drops (ring not ready / full) -
                                            * read via get_param "drops" (harness full-load metric) */

    /* worker-owned analysis state */
    _Atomic(worker_buf_t *) wbuf;   /* malloc'd by the worker, atomic publish like the ring;
                                     * only work() dereferences it (process() never) */
    unsigned fifo_rd, fifo_wr;      /* mono fifo ring: monotone counters, used & FIFO_MASK
                                     * (read index instead of memmove per hop - drain is O(n)
                                     * instead of O(n^2) under full load) */
    btt_wrap_t bw;
    uint64_t va_ns;             /* wall-clock estimate of audio sample index bw.fed_samples */
    int rms_head, rms_count;

    /* clock state (worker-owned) */
    double bpm;                 /* published tempo estimate */
    float conf;
    int locked;
    int clock_running, ever_started;
    double period_ns;           /* sticky: last valid BTT period */
    uint64_t beat_start_ns, beat_end_ns;   /* current beat window (wall clock) */
    int tick_in_beat;           /* 0..PPQN-1 */
    uint64_t tick_counter;      /* clocks since START (for SPP) */
    int phase_slips;
    uint64_t last_beat_ns;      /* wall time of last drained beat event */
    unsigned tick;
    double anchor_bpm;          /* set on a bias or range change: Half -> bpm/2, Double -> bpm*2 */
    int anchor_valid;
    int last_bias;
    double last_geo_mid;        /* last seen geo-mid of the range - re-anchor on change
                                 * (see poll block in work()) */

    /* ALSA out (worker-owned) */
    int alsa;                   /* 0 = not tried, 1 = open, -1 = failed (retried) */
    void *seq;
    int seq_port;
    alsa_out_api_t A;
    atomic_uint send_errs;      /* events rejected by event_output_direct with rc<0
                                 * (e.g. -EAGAIN/-ENOSPC) - TRACE + port readout */

    atomic_int stop;
    pthread_t th;
    pthread_mutex_t rlock;      /* worker-published readout text */
    char bpm_text[16], state_text[16], port_text[48];
} Inst;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Auto prior anchor (bias=Auto): 120 BPM if it lies inside [minbpm..maxbpm], else the
 * geometric middle of the range. 120 is BTT's own upstream default prior mean and the
 * geometric middle of the musically common 60..240 range; the old geo-mid of the default
 * 40..180 range (84.9) sat so low that material above ~130 BPM locked at half tempo
 * (measured on GiantSteps+GTZAN, test/eval/results/: acc4 >150 BPM 0.16 -> 0.60,
 * 130-150: 0.69 -> 0.96 when moving the anchor to 120). Slow material (< ~100 BPM) that
 * now risks double-tempo locks is covered by bias=Half. */
static double auto_anchor(int minbpm, int maxbpm) {
    if (minbpm <= 120 && maxbpm >= 120) return 120.0;
    return sqrt((double)minbpm * (double)maxbpm);
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

#ifdef TRACE
#define TR(...) do { fprintf(stderr, __VA_ARGS__); fflush(stderr); } while (0)
/* SIGPROF backtrace sampler (kept from v0.1, now actually started from the worker under
 * -DTRACE; debug builds only, never on the RT path). backtrace_symbols_fd()/stdio in the
 * handler are async-signal-unsafe (malloc/locks in a signal handler), so the handler only
 * buffers RAW pointers into a preallocated static ring; the worker loop drains it via
 * trace_sampler_drain() and prints (backtrace_symbols_fd in worker context, not in the
 * handler). */
#include <signal.h>
#include <sys/time.h>
#include <execinfo.h>
#define TRACE_BT_DEPTH 16
#define TRACE_BT_RING  64
static void *trace_bt_ring[TRACE_BT_RING][TRACE_BT_DEPTH];  /* preallocated - the handler never mallocs */
static volatile sig_atomic_t trace_bt_n[TRACE_BT_RING];    /* frames per ring entry */
static volatile sig_atomic_t trace_bt_head;                 /* only the handler writes head */
static void trace_sigprof(int sig) {
    (void)sig;
    int h = trace_bt_head;
    /* only raw pointers into the preallocated ring - no backtrace_symbols*, no stdio.
     * write n before head (volatile ordering); the worker reads head then n. */
    trace_bt_n[h] = backtrace(trace_bt_ring[h], TRACE_BT_DEPTH);
    trace_bt_head = (h + 1) % TRACE_BT_RING;
}
/* drains and prints the handler ring - runs EXCLUSIVELY in the worker loop (SCHED_OTHER),
 * never in the signal handler; without -DTRACE none of this is compiled in */
static void trace_sampler_drain(void) {
    static int last;                 /* worker-only - no lock needed */
    int h = trace_bt_head;
    while (last != h) {
        int i = last % TRACE_BT_RING;
        backtrace_symbols_fd(trace_bt_ring[i], trace_bt_n[i], 2);
        last = (last + 1) % TRACE_BT_RING;
    }
}
static void trace_sampler_start(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = trace_sigprof;
    sigaction(SIGPROF, &sa, NULL);
    /* warm the unwinder up once BEFORE arming the SIGPROF timer: the first backtrace() call
     * can lazily allocate/load, which is not async-signal-safe inside the handler */
    void *warm[TRACE_BT_DEPTH];
    (void)backtrace(warm, TRACE_BT_DEPTH);
    struct itimerval it;
    it.it_interval.tv_sec = 0; it.it_interval.tv_usec = 20000;
    it.it_value = it.it_interval;
    setitimer(ITIMER_PROF, &it, NULL);
}
#else
#define TR(...) do {} while (0)
#endif

/* ---- ALSA out (worker thread only) ----
 * ONE ifdef block instead of three identical JAMCLOCK_HARNESS shims - the offline harness
 * build logs MIDI events instead of touching ALSA; the device build dlopens libasound
 * lazily here in the worker (never on the RT callback). */

/* Port readout name in one place - alsa_send_err() appends errors to this name
 * ("JamClock MIDI Out, send errs N") instead of overwriting the port name;
 * get_param("port") must return the port name. */
#define PORT_TEXT "JamClock MIDI Out"

static int alsa_send(Inst *s, int ev_type);
static int alsa_send_spp(Inst *s, int spp14);

/* port_text is read by get_param("port") under rlock on the possible RT callback - ALL write
 * accesses (worker side here + error path in alsa_send) must hold rlock too, otherwise a torn
 * read is possible. On the worker side the lock is uncritical (not the RT path); create()
 * writes before pthread_create (happens-before). */
static void set_port_text(Inst *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&s->rlock);
    vsnprintf(s->port_text, sizeof s->port_text, fmt, ap);
    pthread_mutex_unlock(&s->rlock);
    va_end(ap);
}

#ifdef JAMCLOCK_HARNESS
extern void jclk_log_event(int ev_type, uint64_t now_ns);
extern void jclk_log_spp(int spp14, uint64_t now_ns);
static void alsa_out_init(Inst *s) {
    s->alsa = 1;
    set_port_text(s, "harness");
}
/* return >= 0 = "went out" (harness log always does) - keeps ever_started testable */
static int alsa_send(Inst *s, int ev_type) {
    (void)s;
    jclk_log_event(ev_type, now_ns());
    return 1;
}
static int alsa_send_spp(Inst *s, int spp14) {
    (void)s;
    jclk_log_spp(spp14, now_ns());
    return 1;
}
#else
static void alsa_send_err(Inst *s, int ev_type, int rc);
static void alsa_out_init(Inst *s) {
    if (s->alsa) return;
    s->alsa = -1;
    alsa_out_api_t A;
    if (alsa_out_load(&A) != 0) { set_port_text(s, "no ALSA lib"); return; }
    void *seq = NULL;
    if (A.open(&seq, "default", ALSASEQ_OPEN_OUTPUT, 0) < 0) { set_port_text(s, "alsa open failed"); return; }
    if (A.nonblock) A.nonblock(seq, 1);
    if (A.set_client_name) A.set_client_name(seq, "JamClock");
    int port = A.create_simple_port(seq, "MIDI Out",
                                    ALSASEQ_CAP_READ | ALSASEQ_CAP_SUBS_READ,
                                    ALSASEQ_TYPE_MIDI_GENERIC | ALSASEQ_TYPE_APPLICATION);
    if (port < 0) { if (A.close) A.close(seq); set_port_text(s, "alsa port failed"); return; }
    s->seq = seq; s->A = A; s->seq_port = port; s->alsa = 1;
    set_port_text(s, PORT_TEXT);
    /* ALSA seq does NOT replay anything - a subscriber that connects via aconnect only AFTER
     * the first START never sees it. On a successful (re-)open of the port (start + retry
     * every 2 s), therefore resend SPP + CONTINUE/START while the clock is running so that
     * late subscribers sync up. */
    if (s->clock_running) {
        alsa_send_spp(s, (int)((s->tick_counter / 6) & 0x3FFF));
        if (alsa_send(s, s->ever_started ? ALSASEQ_EV_CONTINUE : ALSASEQ_EV_START) >= 0)
            s->ever_started = 1;
    }
}

/* count rc<0 from event_output_direct (send_errs) + TRACE + port readout. The port name is
 * NOT overwritten (it used to permanently read "send err N" instead of "JamClock MIDI Out") -
 * the error is appended to the port name so that get_param("port") returns the port name. */
static void alsa_send_err(Inst *s, int ev_type, int rc) {
    (void)ev_type; (void)rc;   /* without -DTRACE, TR() is empty */
    unsigned e = atomic_fetch_add(&s->send_errs, 1) + 1;
    TR("jamclock: ALSA send ev %d rc=%d (total %u)\n", ev_type, rc, e);
    set_port_text(s, PORT_TEXT ", send errs %u", e);   /* fits safely into 48 B (snprintf-bounded) */
}

/* returns >= 0 if the event really went out (port open + rc ok), < 0 if dropped -
 * ever_started and send_errs rely on this */
static int alsa_send(Inst *s, int ev_type) {
    if (s->alsa != 1) return -1;
    int rc = alsa_out_send(&s->A, s->seq, s->seq_port, ev_type);
    if (rc < 0) alsa_send_err(s, ev_type, rc);
    return rc;
}

static int alsa_send_spp(Inst *s, int spp14) {
    if (s->alsa != 1) return -1;
    int rc = alsa_out_send_spp(&s->A, s->seq, s->seq_port, spp14);
    if (rc < 0) alsa_send_err(s, ALSASEQ_EV_SONGPOS, rc);
    return rc;
}
#endif

/* one helper instead of 4 identical inline STOP sequences (avoid duplicated code).
 * Worker-only - alsa_send() is allowed here. */
static void stop_clock(Inst *s) {
    if (s->clock_running) { alsa_send(s, ALSASEQ_EV_STOP); s->clock_running = 0; }
}

/* ---- worker-published readouts ---- */

/* rlock is taken by get_param() on the possible SCHED_FIFO audio callback while the worker
 * holds it here - initialized with PTHREAD_PRIO_INHERIT in create() (the documented minimum
 * against priority inversion on the RT path). Critical section = two snprintf()s. */
static void publish(Inst *s, const char *state) {
    pthread_mutex_lock(&s->rlock);
    snprintf(s->state_text, sizeof s->state_text, "%s", state);
    if (s->period_ns > 0.0 && s->bpm > 0.0) snprintf(s->bpm_text, sizeof s->bpm_text, "%.1f", s->bpm);
    else snprintf(s->bpm_text, sizeof s->bpm_text, "--");
    pthread_mutex_unlock(&s->rlock);
}

/* ---- split clock (period/phase split, AudioClock architecture) ---- */

/* start/continue the clock at a predicted beat (sample-time mapped to wall clock, 29.25 ms
 * = 1290 samples in the future - see btt_wrap.h; clamped to [now, now+lookahead_ms]) */
static void start_clock(Inst *s, uint64_t beat_ns, uint64_t now) {
    int64_t look = (int64_t)atomic_load(&s->lookahead_i) * 1000000LL;
    int64_t bs = (int64_t)beat_ns;
    if (bs < (int64_t)now) bs = (int64_t)now;
    if (bs > (int64_t)now + look) bs = (int64_t)now + look;
    s->beat_start_ns = (uint64_t)bs;
    s->beat_end_ns = s->beat_start_ns + (uint64_t)s->period_ns;
    s->tick_in_beat = 0;
    s->phase_slips = 0;
    s->clock_running = 1;
    alsa_send_spp(s, (int)((s->tick_counter / 6) & 0x3FFF));   /* SPP: 6 clocks per 1/16 note */
    /* set ever_started only if the transport send REALLY went out (port open + rc ok) -
     * otherwise there would be a later CONTINUE without any START ever seen, and slaves
     * that need a START would not start (late-subscriber problem). */
    if (alsa_send(s, s->ever_started ? ALSASEQ_EV_CONTINUE : ALSASEQ_EV_START) >= 0)
        s->ever_started = 1;
}

/* proportional phase correction from one beat event; moves only the END of the current beat,
 * clamped to 0.5..1.5x period, with the span floor keeping pending tick due-times in the
 * future - due times never move backwards */
static void handle_beat(Inst *s, uint64_t beat_ns, uint64_t now) {
    s->last_beat_ns = beat_ns;
    if (s->period_ns <= 0.0) return;
    if (!s->clock_running) {
        if (s->locked) start_clock(s, beat_ns, now);
        return;
    }
    int64_t period = (int64_t)s->period_ns;
    int64_t err = (int64_t)beat_ns - (int64_t)s->beat_end_ns;
    int64_t half = period / 2;
    /* fold err into [-half, half] in O(1) via modulo arithmetic instead of while-loops
     * (formally unbounded - practically bounded, but why loop). C99 division truncates
     * toward zero: the remainder has the sign of err and |rem| < period, so a single sign
     * correction suffices - equivalent to the while-loops (the remainder lands in exactly
     * the same interval). */
    int64_t rem = err % period;
    if (rem > half) rem -= period;
    else if (rem < -half) rem += period;
    err = rem;
    if (llabs(err) > (int64_t)(PHASE_SLIP_LIMIT * (double)period)) {
        /* one big error is usually syncopation; three in a row = genuine re-phase */
        if (++s->phase_slips >= PHASE_SLIP_COUNT) {
            s->beat_end_ns = beat_ns;
            s->phase_slips = 0;
        }
    } else {
        s->phase_slips = 0;
        s->beat_end_ns += (uint64_t)(int64_t)(PHASE_ALPHA * (double)err);
    }
    int64_t span = (int64_t)s->beat_end_ns - (int64_t)s->beat_start_ns;
    int64_t span_min = (int64_t)(SPAN_MIN_FRAC * (double)period);
    int64_t span_max = (int64_t)(SPAN_MAX_FRAC * (double)period);
    if (span < span_min) { s->beat_end_ns = s->beat_start_ns + (uint64_t)span_min; span = span_min; }
    if (span > span_max) { s->beat_end_ns = s->beat_start_ns + (uint64_t)span_max; span = span_max; }
    /* span floor: tick k is due at beat_start + k*(span/PPQN); keep that > now for the
     * pending tick (otherwise a shrunk span moves due times into the past and they fire as
     * a burst) */
    if (s->tick_in_beat > 0 && s->tick_in_beat < PPQN) {
        int64_t elapsed = (int64_t)now - (int64_t)s->beat_start_ns;
        if (elapsed > 0) {
            int64_t min_span = elapsed * PPQN / s->tick_in_beat + PPQN * 1000;
            if (span < min_span) s->beat_end_ns = s->beat_start_ns + (uint64_t)min_span;
        }
    }
}

/* send all ticks whose deadline passed; drop (never burst) anything > 2 tick intervals
 * overdue - the phase resyncs instead of firing a catch-up series */
static void send_due_ticks(Inst *s, uint64_t now) {
    if (!s->clock_running || s->period_ns <= 0.0) return;
    uint64_t span = s->beat_end_ns - s->beat_start_ns;
    uint64_t iv = span / PPQN;
    if (iv == 0) iv = 1;
    while (s->tick_in_beat < PPQN) {
        uint64_t due = s->beat_start_ns + (uint64_t)s->tick_in_beat * iv;
        if (due > now) break;
        if (now - due <= 2 * iv + 1000000ULL) {
            alsa_send(s, ALSASEQ_EV_CLOCK);
            s->tick_counter++;
        }
        s->tick_in_beat++;
    }
    if (s->tick_in_beat >= PPQN) {          /* beat rollover: freewheel on the BTT period */
        s->beat_start_ns = s->beat_end_ns;
        s->beat_end_ns = s->beat_start_ns + (uint64_t)s->period_ns;
        s->tick_in_beat = 0;
    }
}

static uint64_t next_tick_due(Inst *s) {
    if (!s->clock_running || s->period_ns <= 0.0 || s->tick_in_beat >= PPQN) return 0;
    uint64_t span = s->beat_end_ns - s->beat_start_ns;
    uint64_t iv = span / PPQN;
    if (iv == 0) iv = 1;
    return s->beat_start_ns + (uint64_t)s->tick_in_beat * iv;
}

/* ---- worker ---- */

static void work(Inst *s, uint64_t now) {
    /* reset (momentary): flush analysis + transport (btt_clear here is fine - user action) */
    if (atomic_exchange(&s->reset_req, 0)) {
        stop_clock(s);                       /* see helper */
        bttw_clear(&s->bw);
        s->fifo_rd = s->fifo_wr = 0;
        s->rms_head = s->rms_count = 0;
        s->va_ns = now;
        s->locked = 0; s->bpm = 0.0; s->conf = 0.0f;
        s->period_ns = 0.0;
        s->beat_start_ns = s->beat_end_ns = 0;
        s->tick_in_beat = 0; s->tick_counter = 0; s->phase_slips = 0; s->last_beat_ns = 0;
        s->ever_started = 0;
        s->anchor_valid = 0;
        s->last_bias = atomic_load(&s->bias_i);
    }

    worker_buf_t *wb = atomic_load_explicit(&s->wbuf, memory_order_acquire);
    if (!wb) return;   /* worker calls work() only after wbuf is published - defensive only */
    /* hoist the ring pointer out of the loop - saves one atomic pointer load per frame.
     * Worker-only, and the pointer does not change after publish until destroy() (only after
     * pthread_join) - the hoist is safe. process() (RT side) keeps reading the pointer
     * atomically per call - caching is not allowed there. */
    const int16_t *ring = atomic_load_explicit(&s->ring, memory_order_acquire);
    if (!ring) return; /* defensive - the worker only calls work() with a published ring */

    /* drain the SPSC ring into the mono fifo. The fifo is a RING (fifo_wr/fifo_rd monotone,
     * used & FIFO_MASK) - the consume side advances a read index instead of a memmove per BTT
     * hop, so a full-fifo burst is O(n) total, not O(n^2) (worker-only; process() on the RT
     * side is untouched). */
    for (;;) {
        if (atomic_load_explicit(&s->stop, memory_order_relaxed)) break;   /* teardown bounded */
        unsigned t = atomic_load(&s->tail);
        unsigned h = atomic_load_explicit(&s->head, memory_order_acquire);
        unsigned avail = h - t;
        if (!avail) break;
        unsigned room = FIFO_CAP - (s->fifo_wr - s->fifo_rd);
        unsigned n = avail < room ? avail : room;
        if (!n) break;
        for (unsigned i = 0; i < n; i++) {
            const int16_t *f = &ring[((t + i) & RING_MASK) * 2];   /* cached pointer */
            wb->fifo[(s->fifo_wr + i) & FIFO_MASK] = (f[0] + f[1]) * (0.5f / 32768.0f);
        }
        s->fifo_wr += n;
        atomic_store_explicit(&s->tail, t + n, memory_order_release);
    }
    int ring_empty;
    {
        unsigned t = atomic_load(&s->tail);
        unsigned h = atomic_load_explicit(&s->head, memory_order_acquire);
        ring_empty = (h == t);
    }

    /* feed BTT in hop-sized chunks; advance the virtual audio clock per chunk, keep the
     * per-hop RMS ring for silence detection. send_due_ticks() is interleaved so one
     * btt_process() call (~50-200 us) is the worst-case tick lateness, not a whole work
     * tick - the analysis must never delay a due clock tick. Hop read from the fifo ring
     * (BTT_HOP=128 divides FIFO_CAP=2^16, a hop wraps at most once - then via a small stack
     * buffer). */
    float hopbuf[BTT_HOP];
    while (s->fifo_wr - s->fifo_rd >= BTT_HOP) {
        if (atomic_load_explicit(&s->stop, memory_order_relaxed)) break;   /* teardown bounded */
        unsigned base = s->fifo_rd & FIFO_MASK;
        const float *src;
        if (base + BTT_HOP <= FIFO_CAP) src = &wb->fifo[base];
        else {
            for (int i = 0; i < BTT_HOP; i++) hopbuf[i] = wb->fifo[(base + i) & FIFO_MASK];
            src = hopbuf;
        }
        float rms = 0.0f;
        for (int i = 0; i < BTT_HOP; i++) { float x = src[i]; rms += x * x; }
        rms = sqrtf(rms / BTT_HOP);
        bttw_feed(&s->bw, src, BTT_HOP);
        s->fifo_rd += BTT_HOP;
        s->va_ns += (uint64_t)((double)BTT_HOP * 1e9 / FS);
        wb->rms[s->rms_head] = rms;
        s->rms_head = (s->rms_head + 1) % RMS_RING;
        if (s->rms_count < RMS_RING) s->rms_count++;
        send_due_ticks(s, now_ns());
    }
    /* virtual audio clock: with the ring empty, all fed audio is "now" - pull va_ns toward
     * wall clock (2% per work tick, ~0.5 s time constant; absorbs rounding + scheduling
     * slack). If the queue backs up, eight blocks must NOT get nearly the same wall time. */
    if (ring_empty)
        s->va_ns += (uint64_t)(int64_t)(0.02 * (double)((int64_t)now - (int64_t)s->va_ns));

    /* drain beat events: audio sample domain -> wall clock via the virtual audio clock */
    uint64_t st;
    while (bttw_beat_pop(&s->bw, &st)) {
        int64_t beat_ns = (int64_t)s->va_ns +
            (int64_t)((double)((int64_t)st - (int64_t)s->bw.fed_samples) * (1e9 / FS));
        handle_beat(s, (uint64_t)beat_ns, now);
    }

    /* tempo poll + param mapping - every 5th tick (50 ms) is plenty */
    if ((s->tick++ % 5) == 0) {
        bttw_poll(&s->bw);
        if (s->bw.period_ns > 0.0) s->period_ns = s->bw.period_ns;   /* sticky period */
        s->bpm = s->bw.bpm;
        s->conf = (float)s->bw.certainty;
        /* prior anchor: Auto = 120 BPM when inside [minbpm..maxbpm], else the geo-mid of
         * the range (auto_anchor()); re-anchored on a bias change from the current estimate
         * (Half -> bpm/2, Double -> bpm*2). Additionally re-anchor whenever the range changes
         * (bias OR range change). The anchor is thus a function of (range, bias) - NEVER of
         * the lock or the estimate: following the per-frame estimate or the lock median would
         * be a positive feedback loop into octave errors; BTT's own decaying histogram
         * provides the temporal smoothing and anti-flip, the fixed prior only disambiguates
         * octaves. */
        int bias = atomic_load(&s->bias_i);
        int minbpm_i = atomic_load(&s->minbpm_i), maxbpm_i = atomic_load(&s->maxbpm_i);
        double geo_mid = sqrt((double)minbpm_i * (double)maxbpm_i);
        double base_anchor = auto_anchor(minbpm_i, maxbpm_i);
        if (bias != s->last_bias) {
            s->last_bias = bias;
            double b = s->bpm > 0.0 ? s->bpm : 120.0;
            s->anchor_bpm = bias == 1 ? b / 2.0 : bias == 2 ? b * 2.0 : b;
            s->anchor_valid = 1;
        } else if (s->anchor_valid && geo_mid != s->last_geo_mid) {
            /* range change - hang the anchor on the NEW Auto anchor, keep the bias factor
             * (anchor = f(range, bias), never f(lock)) */
            double f = bias == 1 ? 0.5 : bias == 2 ? 2.0 : 1.0;
            s->anchor_bpm = base_anchor * f;
        }
        s->last_geo_mid = geo_mid;
        double base = s->anchor_valid ? s->anchor_bpm : base_anchor;
        bttw_set_range(&s->bw, (double)atomic_load(&s->minbpm_i), (double)atomic_load(&s->maxbpm_i));
        bttw_set_bias(&s->bw, base, (double)atomic_load(&s->prior_x1000) / 1000.0);
    }

    /* silence over the last `silence` seconds (RMS - never beat absence). Threshold = chunk
     * key sil_rms (default 1e-4 ~ -80 dBFS). Note: silence detection needs a RUNNING
     * process() callback - the RMS ring only fills while audio is fed. On the MPC that holds:
     * the monitored input track runs continuously (even with zeros), so silence is detected;
     * if a host stops the callback completely, STOP does not fire (documented). */
    float sil_thr = (float)atomic_load(&s->sil_rms_x1e6) * 1e-6f;
    int sil_s = atomic_load(&s->silence_i);
    int need = (int)(sil_s * FS / BTT_HOP);
    int silent = 0;
    if (s->rms_count >= need && need > 0) {
        double m = 0.0;
        int start = s->rms_head - need;
        for (int i = 0; i < need; i++) m += wb->rms[(start + i + RMS_RING) % RMS_RING];
        silent = (m / need) < sil_thr;
    }

    /* lock: valid period + a beat within 2 periods + not silent + enough audio seen */
    int beat_recent = s->period_ns > 0.0 &&
        (int64_t)(now - s->last_beat_ns) <= (int64_t)(2.0 * s->period_ns);
    double audio_s = s->bw.fed_samples / FS;
    s->locked = beat_recent && !silent && audio_s >= MIN_LOCK_S;

    /* transport state machine */
    int en = atomic_load(&s->en_i);
    if (!en) {
        stop_clock(s);                     /* see helper */
        publish(s, "OFF");
    } else if (silent) {
        stop_clock(s);                     /* see helper */
        s->locked = 0;
        publish(s, "STOPPED");     /* BTT state is kept: resume = CONTINUE at same tempo */
    } else if (s->locked) {
        publish(s, "LOCKED");      /* clock start happens on the next beat event */
    } else {
        publish(s, "SEARCH");
    }
}

/* create() may run on the SCHED_FIFO audio callback and pthread_create() would INHERIT that
 * policy - the worker must be SCHED_OTHER and stay off the audio core (CPU 3 on Gen1 armv7,
 * the fast cluster 4-7 on Gen2 aarch64). create()
 * requests the POLICY at thread birth via pthread_attr_t (SCHED_OTHER + PTHREAD_EXPLICIT_SCHED
 * - attr calls are no syscalls). The AFFINITY (off CPU 3) is set EXCLUSIVELY HERE in the
 * worker - create() must not issue syscalls (sched_getaffinity would be one on the possible
 * SCHED_FIFO callback). This in-worker call also covers the plain-pthread_create fallback path.
 * Best effort on purpose: failures are TRACE-logged only - the host test on x86 must keep
 * working regardless. */
static void worker_demote(void) {
    struct sched_param sp;
    memset(&sp, 0, sizeof sp);                  /* SCHED_OTHER: priority must be 0 */
    if (pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp) != 0)
        TR("worker: pthread_setschedparam(SCHED_OTHER) failed: %s\n", strerror(errno));
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    if (sched_getaffinity(0, sizeof cpus, &cpus) == 0 && CPU_COUNT(&cpus) > 1) {
#ifdef __aarch64__
        /* Gen2 (Live III etc.): cores 4-7 are the Cortex-A76 fast cluster where MPC pins its
         * AudioWorker0-3 under SCHED_FIFO - keep this SCHED_OTHER worker on the little cores */
        CPU_CLR(4, &cpus); CPU_CLR(5, &cpus); CPU_CLR(6, &cpus); CPU_CLR(7, &cpus);
#else
        CPU_CLR(3, &cpus);                      /* Gen1: keep off the audio callback's core */
#endif
        if (CPU_COUNT(&cpus) > 0 && sched_setaffinity(0, sizeof cpus, &cpus) != 0)
            TR("worker: sched_setaffinity failed: %s\n", strerror(errno));
    }
}

/* RT rule (no malloc on the possible SCHED_FIFO audio callback): the 2 MB SPSC ring and the
 * analysis block wbuf (fifo 256 KB + rms 22 KB) are allocated HERE in the worker (SCHED_OTHER
 * - malloc allowed) and published atomically; process() drops until the pointer is non-NULL.
 * Right after a successful malloc, PRE-TOUCH (memset) - otherwise the pages are faulted in
 * lazily on first access, and under a full-load burst first-touch page faults could land in
 * process() (RT callback). In the worker context (SCHED_OTHER) the memset is allowed. One
 * helper instead of two identical malloc+publish blocks. */
static void worker_alloc_buffers(Inst *s) {
    if (!atomic_load_explicit(&s->ring, memory_order_acquire)) {
        int16_t *r = (int16_t *)malloc((size_t)RING_CAP * 2 * sizeof(int16_t));
        if (r) {
            memset(r, 0, (size_t)RING_CAP * 2 * sizeof(int16_t));   /* pre-touch - keep first-touch
                                                                     * faults out of process() */
            atomic_store_explicit(&s->ring, r, memory_order_release);
        } else TR("worker: ring malloc failed, retrying in loop\n");
    }
    if (!atomic_load_explicit(&s->wbuf, memory_order_acquire)) {
        worker_buf_t *wb = (worker_buf_t *)malloc(sizeof *wb);
        if (wb) {
            memset(wb, 0, sizeof *wb);   /* wbuf is worker-only - pre-touch does no harm */
            atomic_store_explicit(&s->wbuf, wb, memory_order_release);
        } else TR("worker: wbuf malloc failed, retrying in loop\n");
    }
}

static void *worker(void *arg) {
    Inst *s = (Inst *)arg;
    TR("worker %p start\n", (void *)s);
#ifdef TRACE
    trace_sampler_start();
#endif
    worker_demote();                            /* policy+affinity in the worker - see create() */
    setpriority(PRIO_PROCESS, 0, 10);           /* best effort: below the audio thread */
    pthread_setname_np(pthread_self(), "jamclock");
    worker_alloc_buffers(s);                    /* malloc + pre-touch + publish */
    alsa_out_init(s);
    bttw_init(&s->bw);
    uint64_t next_work = now_ns();
    s->va_ns = next_work;
    int alsa_retry = 0;
    uint64_t next_alloc_retry = 0;              /* own timestamp for the malloc retry */
    while (!atomic_load(&s->stop)) {
        uint64_t now = now_ns();
        /* couple the malloc retry to time (retry only every ~100 ms). If the ring/wbuf malloc
         * fails permanently, next_work is never advanced, the deadline clamp would hold the
         * loop at now+1 ms and the malloc would come at ~1 kHz - hence an own timestamp instead
         * of a retry per loop iteration. */
        if (now >= next_alloc_retry &&
            (!atomic_load_explicit(&s->ring, memory_order_acquire) ||
             !atomic_load_explicit(&s->wbuf, memory_order_acquire))) {
            next_alloc_retry = now + 100 * 1000000ULL;      /* 100 ms */
            worker_alloc_buffers(s);
        }
        if (now >= next_work &&
            atomic_load_explicit(&s->ring, memory_order_acquire) &&
            atomic_load_explicit(&s->wbuf, memory_order_acquire)) {
            work(s, now);
            /* the block condition is anyway now >= next_work, i.e. always "behind
             * (slow/ASan): reschedule from now" */
            next_work = now + TICK_MS * 1000000ULL;
        }
#ifdef TRACE
        trace_sampler_drain();                  /* drain + print the handler ring */
#endif
        /* ALSA retry decoupled from the work() branch - even if the ring/wbuf malloc fails
         * permanently, the port must still be (re)tried. 200 loops are ~2 s in normal
         * operation (10 ms tick); in the degraded case (no ring, 1 ms clamp) correspondingly
         * faster, so the port retry does not starve there. */
        if (s->alsa != 1 && ++alsa_retry >= 200) { alsa_retry = 0; s->alsa = 0; alsa_out_init(s); }
        send_due_ticks(s, now);
        /* sleep until the next work tick or the next tick deadline - whichever comes first
         * (lookahead scheduling; the tick stream is paced on absolute deadlines) */
        uint64_t deadline = next_work;
        uint64_t nd = next_tick_due(s);
        if (nd && nd < deadline) deadline = nd;
        if (deadline <= now) deadline = now + 1000000ULL;   /* never busy-loop: sleep at least 1 ms */
        struct timespec ts = { (time_t)(deadline / 1000000000ULL), (long)(deadline % 1000000000ULL) };
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
    }
#ifndef JAMCLOCK_HARNESS
    /* best-effort STOP before the port disappears - subscribers see a clean stop instead of a
     * silent port disappearance / clock timeout. Device path only: the harness logs events for
     * the A/B metrics and should not see an extra teardown STOP. Uses the stop_clock() helper. */
    stop_clock(s);
#endif
    if (s->alsa == 1 && s->A.close) s->A.close(s->seq);
    return NULL;
}

/* ---- engine interface ---- */

static void apply(Inst *s) {
    atomic_store(&s->en_i, clampi(atoi(s->enable), 0, 1));
    atomic_store(&s->bias_i, clampi(atoi(s->bias), 0, 2));
    int minbpm = clampi(atoi(s->minbpm), 40, 120);
    /* maxbpm is settable down to 60 (not 120): capping the range below 2x the slowest
     * expected tempo blocks double-tempo (octave-up) locks - with maxbpm=115 the <=120 BPM
     * subset of GiantSteps+GTZAN measures acc4 0.81 vs 0.56 at the 200 default (test/eval/,
     * results/sl_cap115 vs sl_new). The Auto prior anchor follows the range (geo-mid when
     * 120 is outside it), so a capped range also re-anchors the prior low. */
    int maxbpm = clampi(atoi(s->maxbpm), 60, 240);
    /* minbpm>=maxbpm is settable - bttw_set_range() rejects min>=max (early return) and the
     * BTT range would stay stale while the prior anchor is computed from the new range.
     * Never leave the range empty: raise maxbpm to minbpm+1 (minbpm <= 120, so
     * maxbpm <= 121 - within the param range). */
    if (maxbpm <= minbpm) maxbpm = minbpm + 1;
    atomic_store(&s->minbpm_i, minbpm);
    atomic_store(&s->maxbpm_i, maxbpm);
    atomic_store(&s->silence_i, clampi(atoi(s->silence), 1, 10));
    atomic_store(&s->lookahead_i, clampi(atoi(s->lookahead_ms), 5, 100));
    atomic_store(&s->prior_x1000, clampi((int)(atof(s->prior) * 1000.0 + 0.5), 200, 2000));
    /* sil_rms: stored as threshold*1e6 (default 1e-4 -> 100), clamp 1e-6..1e-2 */
    atomic_store(&s->sil_rms_x1e6, clampi((int)(atof(s->sil_rms) * 1e6 + 0.5), 1, 10000));
}

static char *slot(Inst *s, const char *k) {
    if (!strcmp(k, "enable"))       return s->enable;
    if (!strcmp(k, "bias"))         return s->bias;
    if (!strcmp(k, "minbpm"))       return s->minbpm;
    if (!strcmp(k, "maxbpm"))       return s->maxbpm;
    if (!strcmp(k, "silence"))      return s->silence;
    if (!strcmp(k, "lookahead_ms")) return s->lookahead_ms;
    if (!strcmp(k, "prior"))        return s->prior;
    if (!strcmp(k, "sil_rms"))      return s->sil_rms;
    return NULL;
}

static void *create(const char *data_dir) {
    (void)data_dir;
    TR("create\n");
    /* RT rule: create() may run on the SCHED_FIFO audio callback - calloc + pthread_create
     * ONLY. No malloc of the ring here (the worker allocates it); no dlopen/ALSA/file I/O, no
     * locks taken. The calloc is SMALL again (a few hundred bytes): the big fifo/rms arrays
     * moved into the worker-malloc'ed wbuf block, so create() no longer allocates ~285 KB. */
    Inst *s = (Inst *)calloc(1, sizeof *s);
    if (!s) return NULL;
    strcpy(s->enable, "1"); strcpy(s->bias, "0");
    /* minbpm default 40, NOT 60: BTT's CBSS normalization is skewed at the lower search edge -
     * with minbpm == true tempo a 60 BPM click flips to 120. 40 keeps 60 BPM material safe;
     * old jclk1/jclk2 presets carry their own minbpm and load unchanged (set 40 or bias=Half
     * there for 60 BPM material). */
    strcpy(s->minbpm, "40"); strcpy(s->maxbpm, "200"); strcpy(s->silence, "3");
    strcpy(s->lookahead_ms, "30"); strcpy(s->prior, "0.625");
    snprintf(s->sil_rms, sizeof s->sil_rms, "%g", (double)SIL_RMS_DEF);
    atomic_store(&s->reset_req, 0);
    atomic_store(&s->stop, 0);
    atomic_store_explicit(&s->ring, (int16_t *)NULL, memory_order_relaxed);
    s->alsa = 0;
    /* get_param() can run on the SCHED_FIFO audio callback and takes rlock while the worker
     * holds it in publish() - PRIO_INHERIT is the documented minimum on the RT path. */
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&s->rlock, &ma);
    pthread_mutexattr_destroy(&ma);
    snprintf(s->bpm_text, sizeof s->bpm_text, "--");
    snprintf(s->state_text, sizeof s->state_text, "SEARCH");
    snprintf(s->port_text, sizeof s->port_text, "opening...");
    apply(s);
    s->last_bias = atomic_load(&s->bias_i);
    /* force the worker to SCHED_OTHER at thread BIRTH - create() may itself be SCHED_FIFO and
     * pthread_create would inherit that policy (workers must be SCHED_OTHER). NO affinity attr
     * block here: sched_getaffinity() for the CPU mask is a syscall, and create() must not
     * issue syscalls on the possible SCHED_FIFO audio callback (RT rule: create() = small
     * calloc + thread spawn only). The affinity (off CPU 3, audio core) is set exclusively by
     * worker_demote() IN THE WORKER, where sched_getaffinity/sched_setaffinity are allowed. */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    struct sched_param sp;
    memset(&sp, 0, sizeof sp);
    pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
    pthread_attr_setschedparam(&attr, &sp);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    int rc = pthread_create(&s->th, &attr, worker, s);
    pthread_attr_destroy(&attr);
    if (rc != 0) rc = pthread_create(&s->th, NULL, worker, s);  /* fallback: worker_demote() */
    if (rc != 0) {
        pthread_mutex_destroy(&s->rlock);
        free(s);
        return NULL;
    }
    return s;
}

static void destroy(void *inst) {
    Inst *s = (Inst *)inst;
    TR("destroy %p: stop+join\n", (void *)s);
    atomic_store(&s->stop, 1);
    pthread_join(s->th, NULL);      /* worker closes the ALSA seq itself */
    TR("destroy %p: joined\n", (void *)s);
    if (s->bw.btt) btt_destroy(s->bw.btt);
    pthread_mutex_destroy(&s->rlock);
    free(atomic_load(&s->ring));    /* allocated by the worker; NULL-safe after join */
    free(atomic_load(&s->wbuf));    /* likewise worker-allocated (fifo/rms block) */
    free(s);
}

static void midi(void *inst, const uint8_t *m, int len) { (void)inst; (void)m; (void)len; }

static void set_param(void *inst, const char *k, const char *val) {
    Inst *s = (Inst *)inst;
    if (!strcmp(k, "state")) {   /* chunk restore: "jclk1\n..." (v0.1) or "jclk2\n..."; unknown or
                                   * missing keys keep their defaults, so old presets load.
                                   * Check the magic line (otherwise ignore a foreign chunk) and
                                   * detect truncation - a truncated last line must not parse as a
                                   * wrong value (apply() clamps, but the value would still be wrong
                                   * instead of the default). */
        char buf[4096];
        int w = snprintf(buf, sizeof buf, "%s", val);
        if (w < 0) return;
        if ((size_t)w >= sizeof buf) {
            /* val did not fit: drop the last (possibly incomplete) line */
            char *last = strrchr(buf, '\n');
            if (last) *last = '\0'; else buf[0] = '\0';
        }
        char *first = strtok(buf, "\n");
        if (!first || (strcmp(first, "jclk1") && strcmp(first, "jclk2"))) return;  /* not our chunk */
        for (char *line = strtok(NULL, "\n"); line; line = strtok(NULL, "\n")) {
            line[strcspn(line, "\r")] = 0;   /* strip a trailing CR so CRLF-ended chunks match too */
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            char *sl = slot(s, line);
            if (sl) snprintf(sl, 16, "%s", eq + 1);
        }
        apply(s);
        return;
    }
    if (!strcmp(k, "reset")) {
        if (atoi(val)) atomic_store(&s->reset_req, 1);
        return;
    }
    char *sl = slot(s, k);
    if (sl) { snprintf(sl, 16, "%s", val); apply(s); }
}

static int get_param(void *inst, const char *k, char *buf, int buf_len) {
    Inst *s = (Inst *)inst;
    if (!strcmp(k, "state"))
        return snprintf(buf, buf_len,
                        "jclk2\nenable=%s\nbias=%s\nminbpm=%s\nmaxbpm=%s\nsilence=%s\nlookahead_ms=%s\nprior=%s\nsil_rms=%s\n",
                        s->enable, s->bias, s->minbpm, s->maxbpm, s->silence,
                        s->lookahead_ms, s->prior, s->sil_rms);
    if (!strcmp(k, "bpm") || !strcmp(k, "status") || !strcmp(k, "port")) {
        pthread_mutex_lock(&s->rlock);
        const char *txt = !strcmp(k, "bpm") ? s->bpm_text
                          : !strcmp(k, "status") ? s->state_text : s->port_text;
        int n = snprintf(buf, buf_len, "%s", txt);
        pthread_mutex_unlock(&s->rlock);
        return n;
    }
    if (!strcmp(k, "reset")) return snprintf(buf, buf_len, "0");
    if (!strcmp(k, "drops"))   /* "<drop_calls> <drop_frames>" - harness full-load metric */
        return snprintf(buf, buf_len, "%u %u",
                        (unsigned)atomic_load(&s->drop_calls), (unsigned)atomic_load(&s->drop_frames));
    char *sl = slot(s, k);
    if (!sl) return 0;
    pthread_mutex_lock(&s->rlock);   /* same readout lock as the text readouts above */
    int n = snprintf(buf, buf_len, "%s", sl);
    pthread_mutex_unlock(&s->rlock);
    return n;
}

static void render(void *inst, int16_t *out, int frames) { (void)inst; memset(out, 0, (size_t)frames * 4); }

static void process(void *inst, const int16_t *in_lr, int16_t *out_lr, int frames) {
    Inst *s = (Inst *)inst;
    memmove(out_lr, in_lr, (size_t)frames * 4);   /* bit-identical passthrough, alias-safe */
    /* push into the SPSC ring; if full, drop the newest (analysis only, audio is untouched).
     * The ring buffer is malloc'ed by the WORKER (no malloc here on the possible SCHED_FIFO
     * callback) - until the pointer is published, drop (first milliseconds after create();
     * passthrough above is unaffected) */
    int16_t *r = atomic_load_explicit(&s->ring, memory_order_acquire);
    if (!r) {   /* ring not allocated yet (first ms) - drop, count it */
        atomic_fetch_add(&s->drop_calls, 1);
        atomic_fetch_add(&s->drop_frames, (unsigned)frames);
        return;
    }
    unsigned h = atomic_load_explicit(&s->head, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&s->tail, memory_order_acquire);
    unsigned space = RING_CAP - (h - t);
    unsigned n = (unsigned)frames < space ? (unsigned)frames : space;
    for (unsigned i = 0; i < n; i++) {
        r[((h + i) & RING_MASK) * 2] = in_lr[2 * i];
        r[((h + i) & RING_MASK) * 2 + 1] = in_lr[2 * i + 1];
    }
    if (n < (unsigned)frames) {   /* ring full - drop the newest, count it */
        atomic_fetch_add(&s->drop_calls, 1);
        atomic_fetch_add(&s->drop_frames, (unsigned)frames - n);
    }
    atomic_store_explicit(&s->head, h + n, memory_order_release);
}

static const mpc_engine_t ENGINE = { create, destroy, midi, set_param, get_param, render, process, NULL };
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
