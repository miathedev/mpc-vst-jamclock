/* eval_btt.c - offline dataset evaluation of the JamClock tracker stack (btt_wrap + vendored
 * BTT), replicating the engine's parameter mapping and lock rule, but feeding audio as fast
 * as the CPU allows (no real-time pacing, no ALSA).
 *
 * For every manifest row (id,dataset,raw,ref_bpm,beats) it feeds raw mono 44.1 kHz f32 audio
 * through bttw_feed() in 128-sample hops (exactly like the engine's worker) and writes:
 *   <outdir>/states.csv : id,t_s,bpm,period_samples,certainty,locked,silent   (every 0.25 s)
 *   <outdir>/beats.csv  : id,beat_sample,bpm                                  (every BTT beat)
 *
 * Lock rule = engine work(): valid period && a beat within 2 periods && >= 4 s of audio fed &&
 * not silent (mean per-hop RMS over the last `silence` s < sil_rms).
 *
 * Usage:
 *   eval_btt <manifest.csv> <outdir> [options]
 *   Defaults mirror the engine after the v0.3 tracker changes (see test/eval/README.md):
 *     --min F --max F        BTT tempo range (engine minbpm/maxbpm), default 40 200
 *     --anchor F             log-Gaussian prior mean (BPM); default = 120 if 120 is inside
 *                            [min,max], else sqrt(min*max) (engine Auto anchor rule)
 *     --bias 0|1|2           multiply anchor by 1, 1/2, 2 (engine bias Auto/Half/Double)
 *     --ratio F              prior width/mean ratio, default 0.625 (engine `prior` chunk key)
 *     --decay F              gaussian tempo histogram decay, default 0.996006 (BTTW_HISTOGRAM_DECAY)
 *     --sil F                silence seconds, default 3
 *     --sil-rms F            silence threshold, default 1e-4
 *     --acorr-exp F          autocorrelation exponent (BTT default 0.5)
 *     --num-cand N           tempo candidates (BTT default 10)
 *     --cbss-eta F           cbss window width (BTT default 300)
 *     --cbss-alpha F         cbss blend (BTT default 0.9)
 *     --onset-th F           onset threshold in std devs (BTT default 0.1)
 *     --onset-min F          onset threshold min raw flux (BTT default 5.0)
 *     --oss-cutoff F         OSS lowpass cutoff Hz (BTT default 10)
 *     --cbss-mode 0|1|2      CBSS candidate score mode (engine default 2; 0 = upstream)
 *     --id SUBSTR            only rows whose id contains SUBSTR
 *     --limit N              at most N rows
 *     --max-seconds F        stop feeding audio after F seconds per track (0 = full)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <sys/stat.h>
#include "btt_wrap.h"

#define FS 44100.0
#define HOP 128
#define RMS_RING (16 * 44100 / HOP + 2)
#define MIN_LOCK_S 4.0

typedef struct {
    double min_bpm, max_bpm, anchor, ratio, decay, sil, sil_rms;
    double acorr_exp, cbss_eta, cbss_alpha, onset_th, onset_min, oss_cutoff;
    int num_cand, bias, cbss_mode;
    const char *id_substr;
    int limit;
    double max_seconds;
} opts_t;

static void apply_btt_opts(btt_wrap_t *w, const opts_t *o) {
    btt_set_gaussian_tempo_histogram_decay(w->btt, o->decay);
    btt_set_autocorrelation_exponent(w->btt, o->acorr_exp);
    btt_set_num_tempo_candidates(w->btt, o->num_cand);
    btt_set_cbss_eta(w->btt, o->cbss_eta);
    btt_set_cbss_alpha(w->btt, o->cbss_alpha);
    btt_set_onset_threshold(w->btt, o->onset_th);
    btt_set_onset_threshold_min(w->btt, o->onset_min);
    btt_set_oss_filter_cutoff(w->btt, o->oss_cutoff);
    btt_set_cbss_score_mode(w->btt, o->cbss_mode);
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: eval_btt <manifest> <outdir> [opts]\n"); return 2; }
    opts_t o;
    o.min_bpm = 40; o.max_bpm = 200; o.anchor = -1; o.ratio = 0.625; o.decay = 0.996006;
    o.sil = 3.0; o.sil_rms = 1e-4;
    o.acorr_exp = 0.5; o.num_cand = 10; o.cbss_eta = 300; o.cbss_alpha = 0.9; o.cbss_mode = 2;
    o.onset_th = 0.1; o.onset_min = 5.0; o.oss_cutoff = 10;
    o.bias = 0; o.id_substr = NULL; o.limit = 0; o.max_seconds = 0;
    for (int i = 3; i + 1 < argc; i += 2) {
        const char *k = argv[i], *v = argv[i + 1];
        if (!strcmp(k, "--min")) o.min_bpm = atof(v);
        else if (!strcmp(k, "--max")) o.max_bpm = atof(v);
        else if (!strcmp(k, "--anchor")) o.anchor = atof(v);
        else if (!strcmp(k, "--bias")) o.bias = atoi(v);
        else if (!strcmp(k, "--ratio")) o.ratio = atof(v);
        else if (!strcmp(k, "--decay")) o.decay = atof(v);
        else if (!strcmp(k, "--sil")) o.sil = atof(v);
        else if (!strcmp(k, "--sil-rms")) o.sil_rms = atof(v);
        else if (!strcmp(k, "--acorr-exp")) o.acorr_exp = atof(v);
        else if (!strcmp(k, "--num-cand")) o.num_cand = atoi(v);
        else if (!strcmp(k, "--cbss-eta")) o.cbss_eta = atof(v);
        else if (!strcmp(k, "--cbss-alpha")) o.cbss_alpha = atof(v);
        else if (!strcmp(k, "--onset-th")) o.onset_th = atof(v);
        else if (!strcmp(k, "--onset-min")) o.onset_min = atof(v);
        else if (!strcmp(k, "--oss-cutoff")) o.oss_cutoff = atof(v);
        else if (!strcmp(k, "--cbss-mode")) o.cbss_mode = atoi(v);
        else if (!strcmp(k, "--id")) o.id_substr = v;
        else if (!strcmp(k, "--limit")) o.limit = atoi(v);
        else if (!strcmp(k, "--max-seconds")) o.max_seconds = atof(v);
        else { fprintf(stderr, "unknown opt %s\n", k); return 2; }
    }
    /* engine Auto anchor rule (jamclock_engine.c): 120 if inside the range, else geo-mid */
    double geo_mid = sqrt(o.min_bpm * o.max_bpm);
    if (o.anchor < 0) o.anchor = (o.min_bpm <= 120.0 && o.max_bpm >= 120.0) ? 120.0 : geo_mid;
    if (o.bias == 1) o.anchor *= 0.5;
    if (o.bias == 2) o.anchor *= 2.0;

    const char *ds_dir = getenv("JAMCLOCK_DS");
    if (!ds_dir) ds_dir = "test/datasets";
    mkdir(argv[2], 0755);

    FILE *mf = fopen(argv[1], "r");
    if (!mf) { perror("manifest"); return 1; }
    char out_states[1024], out_beats[1024];
    snprintf(out_states, sizeof out_states, "%s/states.csv", argv[2]);
    snprintf(out_beats, sizeof out_beats, "%s/beats.csv", argv[2]);
    FILE *fs = fopen(out_states, "w"), *fb = fopen(out_beats, "w");
    if (!fs || !fb) { perror("out"); return 1; }
    fprintf(fs, "id,t_s,bpm,period_samples,certainty,locked,silent\n");
    fprintf(fb, "id,beat_sample,bpm\n");

    char line[4096];
    if (!fgets(line, sizeof line, mf)) return 1;   /* header */
    int done = 0, skipped = 0;
    while (fgets(line, sizeof line, mf)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        char *id = strtok(line, ",");
        char *dataset = strtok(NULL, ",");
        char *raw = strtok(NULL, ",");
        (void)dataset;
        if (!id || !raw) continue;
        if (o.id_substr && !strstr(id, o.id_substr)) continue;
        if (o.limit && done >= o.limit) break;
        char path[2048];
        snprintf(path, sizeof path, "%s/%s", ds_dir, raw);
        FILE *f = fopen(path, "rb");
        if (!f) { fprintf(stderr, "skip (no raw): %s\n", id); skipped++; continue; }

        btt_wrap_t w;
        bttw_init(&w);
        apply_btt_opts(&w, &o);
        bttw_set_range(&w, o.min_bpm, o.max_bpm);
        bttw_set_bias(&w, o.anchor, o.ratio);

        float rms_ring[RMS_RING];
        int rms_head = 0, rms_count = 0;
        int need = (int)(o.sil * FS / HOP);
        float hop[HOP];
        uint64_t fed = 0;
        int64_t last_beat = -1;
        int hop_idx = 0;
        long total_read;
        while ((total_read = (long)fread(hop, sizeof(float), HOP, f)) == HOP) {
            if (o.max_seconds > 0 && fed / FS >= o.max_seconds) break;
            float rms = 0;
            for (int k = 0; k < HOP; k++) rms += hop[k] * hop[k];
            rms = sqrtf(rms / HOP);
            bttw_feed(&w, hop, HOP);
            fed += HOP;
            rms_ring[rms_head] = rms;
            rms_head = (rms_head + 1) % RMS_RING;
            if (rms_count < RMS_RING) rms_count++;
            uint64_t st;
            while (bttw_beat_pop(&w, &st)) {
                last_beat = (int64_t)st;
                bttw_poll(&w);
                fprintf(fb, "%s,%llu,%.3f\n", id, (unsigned long long)st, w.bpm);
            }
            bttw_poll(&w);
            int period = btt_get_beat_period_audio_samples(w.btt);
            int silent = 0;
            if (rms_count >= need && need > 0) {
                double m = 0;
                int start = rms_head - need;
                for (int i = 0; i < need; i++) m += rms_ring[(start + i + RMS_RING) % RMS_RING];
                silent = (m / need) < o.sil_rms;
            }
            int beat_recent = period > 0 && last_beat >= 0 &&
                (int64_t)(fed - (uint64_t)last_beat) <= 2 * (int64_t)period;
            int locked = beat_recent && !silent && (fed / FS) >= MIN_LOCK_S;
            if ((hop_idx++ % 86) == 0)   /* ~every 0.25 s of audio */
                fprintf(fs, "%s,%.2f,%.3f,%d,%.5f,%d,%d\n", id, fed / FS, w.bpm, period,
                        w.certainty, locked, silent);
        }
        if (total_read > 0) {   /* tail: feed zero-padded last partial hop once */
            for (long k = total_read; k < HOP; k++) hop[k] = 0;
            bttw_feed(&w, hop, HOP);
            fed += HOP;
        }
        fclose(f);
        btt_destroy(w.btt);
        done++;
        if ((done % 50) == 0) { fprintf(stderr, "done %d (skipped %d)\n", done, skipped); fflush(stderr); }
    }
    fclose(mf); fclose(fs); fclose(fb);
    fprintf(stderr, "eval_btt: %d tracks done, %d skipped\n", done, skipped);
    return 0;
}
