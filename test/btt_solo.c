/* btt_solo.c - feed a synthetic signal straight into vendored BTT (no engine) and print the
 * tempo estimate over time. Isolates BTT's own behavior from the JamClock wrapper.
 * Usage: btt_solo [drums|click] [bpm] [lp_fc] [seconds] [prior_mean] [prior_ratio] [min_bpm] [max_bpm]
 *   (all 8 arguments optional; defaults: drums 120 0 15 120 0.625 40 240)
 * Example (see test/README.md): build/btt_solo click 120 0 10 120 0.625 40 240
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "btt/BTT.h"

#define FS 44100.0

typedef struct { double b0,b1,b2,a1,a2,x1,x2,y1,y2; } bq_t;
static void bq_lp(bq_t *b, double fc) {
    double w0 = 2*M_PI*fc/FS, cw = cos(w0), sw = sin(w0), al = sw/(2*0.7071), a0 = 1+al;
    b->b0=(1-cw)/2/a0; b->b1=(1-cw)/a0; b->b2=b->b0; b->a1=-2*cw/a0; b->a2=(1-al)/a0;
    b->x1=b->x2=b->y1=b->y2=0;
}
static double bq_run(bq_t *b, double x) {
    double y = b->b0*x + b->b1*b->x1 + b->b2*b->x2 - b->a1*b->y1 - b->a2*b->y2;
    b->x2=b->x1; b->x1=x; b->y2=b->y1; b->y1=y; return y;
}

int main(int argc, char **argv) {
    const char *kind = argc > 1 ? argv[1] : "drums";
    double bpm = argc > 2 ? atof(argv[2]) : 120.0;
    double lp = argc > 3 ? atof(argv[3]) : 0.0;
    double dur = argc > 4 ? atof(argv[4]) : 15.0;
    double prior_mean = argc > 5 ? atof(argv[5]) : 120.0;
    double prior_ratio = argc > 6 ? atof(argv[6]) : 0.625;

    BTT *b = btt_new_default();
    btt_set_tracking_mode(b, BTT_ONSET_AND_TEMPO_AND_BEAT_TRACKING);
    btt_set_min_tempo(b, argc > 7 ? atof(argv[7]) : 40.0);
    btt_set_max_tempo(b, argc > 8 ? atof(argv[8]) : 240.0);
    btt_set_gaussian_tempo_histogram_decay(b, 0.996006);   /* decimation-4 4th power */
    btt_set_log_gaussian_tempo_weight_mean(b, prior_mean);
    btt_set_log_gaussian_tempo_weight_width(b, prior_mean * prior_ratio);

    bq_t lpq; bq_lp(&lpq, lp);
    long total = (long)(dur * FS);
    float buf[128];
    for (long i = 0; i < total; i += 128) {
        for (int k = 0; k < 128; k++) {
            long idx = i + k;
            double t = (double)idx / FS;
            double beat = t * bpm / 60.0;
            double s = 0.0;
            if (!strcmp(kind, "click")) {
                double ph = beat - floor(beat);
                if (ph < 0.02) s = 0.9 * sin(2*M_PI*1800.0*t) * exp(-ph*250.0);
            } else {
                double tt  = t - floor(beat) * 60.0 / bpm;
                double tt8 = t - floor(beat*2.0) * 30.0 / bpm;
                if (tt < 0.30) s += exp(-tt*25.0) * sin(2*M_PI*55.0*tt);
                if (tt8 < 0.04) s += sin(2*M_PI*8000.0*t) * exp(-tt8*200.0) * 0.12;
            }
            if (lp > 0.0) s = bq_run(&lpq, s);
            buf[k] = (float)s;
        }
        btt_process(b, buf, 128);
        if ((i / 128) % 22 == 0)   /* ~every 0.5 s of audio */
            printf("t=%5.1fs  bpm=%7.2f  period=%d  certainty=%.4f\n",
                   (double)i / FS, btt_get_tempo_bpm(b),
                   btt_get_beat_period_audio_samples(b), btt_get_tempo_certainty(b));
    }
    btt_destroy(b);
    return 0;
}
