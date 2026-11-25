/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 mime */
/* btt_wrap.h - thin wrapper around the vendored BTT (src/btt/, upstream c039090 + the 4
 * AudioClock patches) for the JamClock engine.
 *
 * All functions are worker-thread only (BTT itself is not thread-safe; the engine only ever
 * calls these from its worker). btt_new_default() callocs internally - called once at worker
 * start, never from process(); btt_process() itself does no allocation (checked against the
 * vendored sources), so feeding it from the worker is RT-rule compliant.
 *
 * Sample rate is 44100 Hz (btt_new_default) - matches the engine contract. */
#pragma once
#include <stdint.h>
#include <stdatomic.h>
#include "btt/BTT.h"

#define BTTW_FS 44100.0
/* Queue size rationale: 16 beats at min 40 BPM is a 24 s horizon; the worker drains the
 * queue every ~10 ms, so "full" can only happen on a worker stall. */
#define BTTW_BEATQ_CAP 16

typedef struct {
    BTT *btt;
    /* beat events, audio-sample domain (sample_time 29.25 ms = 1290 samples in the future,
     * see btt_beat_tracking: num_audio_samples_processed + prediction_adjustment*hop
     * - analysis_latency_beat_adjustment_fine = n + 10*128 - (1270 - 128*round(1270/128))
     * = n + 1290). Produced inside btt_process(), consumed by the worker after the feed. */
    uint64_t beat_sample_time[BTTW_BEATQ_CAP];
    int beatq_head, beatq_tail;
    atomic_uint beatq_drops;   /* beats dropped when the queue was full (like send_errs in the
                                * engine) - counted atomically for debug/post-mortem */
    uint64_t fed_samples;          /* total samples fed via bttw_feed */
    /* cached poll results */
    double period_ns;              /* btt_get_beat_period_audio_samples() * 1e9/FS (0 = none) */
    double bpm;
    double certainty;              /* histogram value at the current lag (relative measure) */
    /* param shadow to avoid redundant setter calls */
    double last_min_bpm, last_max_bpm, last_prior_mean, last_prior_width;
} btt_wrap_t;

void bttw_init(btt_wrap_t *w);                                   /* create BTT + callbacks */
void bttw_clear(btt_wrap_t *w);                                  /* full reset (user reset only -
                                                                  NOT on silence-stop: continue keeps
                                                                  the tempo) */
void bttw_feed(btt_wrap_t *w, const float *mono, int n);         /* btt_process + fed_samples */
void bttw_poll(btt_wrap_t *w);                                   /* refresh period_ns/bpm/certainty */
void bttw_set_range(btt_wrap_t *w, double min_bpm, double max_bpm);
/* prior mean = base_bpm (engine passes its FIXED session anchor: geometric middle of
 * [minbpm..maxbpm], re-anchored per bias change - Half -> bpm/2, Double -> bpm*2 - and
 * per range change to the new geo-mid). The anchor deliberately
 * does NOT follow the lock median or the per-frame
 * estimate: that is a positive feedback loop into octave errors (the log-Gaussian prior
 * then amplifies the wrong octave ~13x; measured 60 -> 120 ramp).
 * BTT's own decaying tempo histogram provides the temporal smoothing/anti-flip; the fixed
 * prior only disambiguates octaves.
 * width_ratio is the width/mean RATIO (stock 0.625; BTT uses log2, so an octave off the
 * mean is suppressed by exp(-1/ratio^2) = 0.077 at 0.625). mean and width are always set
 * TOGETHER - set_log_gaussian_tempo_weight_width() stores (bpm/mean)^2, so setting the
 * mean alone would drift the ratio. */
void bttw_set_bias(btt_wrap_t *w, double base_bpm, double width_ratio);
/* pop one beat sample_time; returns 0 when the queue is empty */
int  bttw_beat_pop(btt_wrap_t *w, uint64_t *sample_time);
