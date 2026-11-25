/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 mime */
/* btt_wrap.c - see btt_wrap.h. The gaussian-tempo-histogram decay is the 4th power of the
 * stock 0.999 (0.996006) because the vendored BTT decimates btt_tempo_tracking() by
 * BTT_TEMPO_DECIMATION=4 (patch 3): the decay is applied once per call, so decimating
 * stretches its time constant by 4. */
#include <string.h>
#include <math.h>
#include "btt_wrap.h"

/* histogram decay with decimation 4: 0.999^4 (the "MED" responsiveness of AudioClock) */
#define BTTW_HISTOGRAM_DECAY 0.996006

static void bttw_beat_cb(void *self, unsigned long long sample_time) {
    btt_wrap_t *w = (btt_wrap_t *)self;
    int next = (w->beatq_head + 1) % BTTW_BEATQ_CAP;
    if (next != w->beatq_tail) {
        w->beat_sample_time[w->beatq_head] = (uint64_t)sample_time;
        w->beatq_head = next;
    } else {
        /* Queue full: count the drop atomically (like send_errs in the engine) instead of
         * silently discarding it. Full only on worker stall: the worker drains every ~10 ms,
         * BTTW_BEATQ_CAP=16 beats at min 40 BPM is a 24 s horizon. */
        atomic_fetch_add(&w->beatq_drops, 1);
    }
}

void bttw_init(btt_wrap_t *w) {
    memset(w, 0, sizeof *w);
    atomic_store(&w->beatq_drops, 0);      /* explicit store after memset - memset on an
                                            * atomic object is formally UB, a store is clean */
    w->btt = btt_new_default();            /* 44100 Hz, hop 128, oss 1024 frames */
    if (!w->btt) return;
    btt_set_beat_tracking_callback(w->btt, bttw_beat_cb, w);
    btt_set_tracking_mode(w->btt, BTT_ONSET_AND_TEMPO_AND_BEAT_TRACKING);
    btt_set_gaussian_tempo_histogram_decay(w->btt, BTTW_HISTOGRAM_DECAY);
    /* JAMCLOCK PATCH (see src/btt/VENDORED.md): CBSS candidate score mode 2 = argmax of the
     * raw pulse-pattern xcorr peak per candidate. The upstream score (max/sum_max +
     * variance/sum_variance) compares maxima over DIFFERENT phase counts (lag-dependent),
     * which skews the pick toward large lags and made >~130 BPM material lock at half
     * tempo. Measured on GiantSteps+GTZAN (test/eval/): mode 2 lifts 130-150 BPM accuracy
     * from 0.69 to 0.96 and >150 from 0.16 to 0.60 (raw 4% tempo accuracy, 1660 tracks). */
    btt_set_cbss_score_mode(w->btt, 2);
    w->last_min_bpm = w->last_max_bpm = -1.0;
    w->last_prior_mean = w->last_prior_width = -1.0;
}

void bttw_clear(btt_wrap_t *w) {
    if (w->btt) btt_clear(w->btt);
    w->beatq_head = w->beatq_tail = 0;
    atomic_store(&w->beatq_drops, 0);
    w->fed_samples = 0;
    w->period_ns = 0.0;
    w->bpm = 0.0;
    w->certainty = 0.0;
    w->last_min_bpm = w->last_max_bpm = -1.0;
    w->last_prior_mean = w->last_prior_width = -1.0;
}

void bttw_feed(btt_wrap_t *w, const float *mono, int n) {
    if (!w->btt || n <= 0) return;
    btt_process(w->btt, (dft_sample_t *)mono, n);
    w->fed_samples += (uint64_t)n;
}

void bttw_poll(btt_wrap_t *w) {
    if (!w->btt) return;
    int period_samples = btt_get_beat_period_audio_samples(w->btt);
    w->period_ns = period_samples > 0 ? (double)period_samples * 1e9 / BTTW_FS : 0.0;
    w->bpm = btt_get_tempo_bpm(w->btt);
    w->certainty = btt_get_tempo_certainty(w->btt);
}

void bttw_set_range(btt_wrap_t *w, double min_bpm, double max_bpm) {
    if (!w->btt) return;
    if (min_bpm < 20.0) min_bpm = 20.0;    /* cbss_length 1024 @ hop 128 limits lag */
    if (max_bpm > 480.0) max_bpm = 480.0;
    if (min_bpm >= max_bpm) return;
    if (min_bpm != w->last_min_bpm) { btt_set_min_tempo(w->btt, min_bpm); w->last_min_bpm = min_bpm; }
    if (max_bpm != w->last_max_bpm) { btt_set_max_tempo(w->btt, max_bpm); w->last_max_bpm = max_bpm; }
}

void bttw_set_bias(btt_wrap_t *w, double base_bpm, double width_ratio) {
    if (!w->btt || base_bpm <= 0.0) return;
    double mean = base_bpm;
    if (mean < 20.0) mean = 20.0;
    if (mean > 480.0) mean = 480.0;
    if (width_ratio < 0.2) width_ratio = 0.2;
    if (width_ratio > 2.0) width_ratio = 2.0;
    double width = mean * width_ratio;          /* width is stored as (width/mean)^2 ratio -
                                                   set mean FIRST, then re-pin the width at the
                                                   new mean */
    if (fabs(mean - w->last_prior_mean) < 1e-9 && fabs(width - w->last_prior_width) < 1e-9)
        return;
    btt_set_log_gaussian_tempo_weight_mean(w->btt, mean);
    btt_set_log_gaussian_tempo_weight_width(w->btt, width);
    w->last_prior_mean = mean;
    w->last_prior_width = width;
}

int bttw_beat_pop(btt_wrap_t *w, uint64_t *sample_time) {
    if (w->beatq_head == w->beatq_tail) return 0;
    *sample_time = w->beat_sample_time[w->beatq_tail];
    w->beatq_tail = (w->beatq_tail + 1) % BTTW_BEATQ_CAP;
    return 1;
}
