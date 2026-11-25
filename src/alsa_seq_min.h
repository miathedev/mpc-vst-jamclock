/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 mime */
/* Minimal ALSA sequencer declarations for dlopen()ed libasound - no build-time -lasound needed
 * (the device ships /usr/lib/libasound.so.2; the builder image has no alsa headers).
 * Output side only: open an OUTPUT sequencer, create a READ|SUBS_READ port (so a track or
 * `aconnect` can subscribe to it), and send system-realtime events (MIDI clock/start/stop/
 * continue) with snd_seq_event_output_direct. Struct layout matches alsa/sequencer.h exactly
 * (snd_seq_event_t is 28 bytes on 32- and 64-bit: the timestamp union is two u32s).
 *
 * The snd_seq_ev_set_* helpers are static inline in alsa/seq_event.h, i.e. NOT dynamic symbols,
 * so they are replicated here by hand on our alsa_ev_t:
 *   snd_seq_ev_clear      -> memset 0
 *   snd_seq_ev_set_source -> source.port = port
 *   snd_seq_ev_set_subs   -> dest.client = 254 (SND_SEQ_ADDRESS_SUBSCRIBERS), dest.port = 253 (UNKNOWN)
 *   snd_seq_ev_set_direct -> queue = 253 (SND_SEQ_QUEUE_DIRECT)
 * Event types (alsa/seq.h enum snd_seq_event_type): START=30, CONTINUE=31, STOP=32, CLOCK=36.
 * A RtMidi-based receiver (tkgl rtpmidid in the MPC app) decodes those back to 0xFA/0xFB/0xFC/0xF8
 * via snd_midi_event_decode, so raw MIDI clock arrives byte-exact. */
#pragma once
#include <stdint.h>
#include <string.h>
#include <dlfcn.h>

/* the harness build (-DJAMCLOCK_HARNESS) never calls the load/send functions (it logs MIDI
 * events instead) - mark them maybe-unused so this header compiles warning-free in both
 * the device and the harness TU */
#if defined(__GNUC__)
#define ALSASEQ_MAYBE_UNUSED __attribute__((unused))
#else
#define ALSASEQ_MAYBE_UNUSED
#endif

typedef struct { unsigned char client, port; } alsa_addr_t;
typedef union { unsigned int tick; struct { unsigned int tv_sec, tv_nsec; } time; } alsa_ts_t;
typedef struct {
    unsigned char type, flags, tag, queue;
    alsa_ts_t time;
    alsa_addr_t source, dest;
    union {
        struct { unsigned char channel, note, velocity, off_velocity; unsigned int duration; } note;
        struct { unsigned char channel, pad[3]; unsigned int param; int value; } control;
        unsigned char raw[12];
    } data;
} alsa_ev_t;

#define ALSASEQ_OPEN_OUTPUT      1   /* SND_SEQ_OPEN_OUTPUT */
#define ALSASEQ_CAP_READ         (1u << 0)
#define ALSASEQ_CAP_SUBS_READ    (1u << 5)
#define ALSASEQ_TYPE_MIDI_GENERIC (1u << 1)
#define ALSASEQ_TYPE_APPLICATION  (1u << 20)
#define ALSASEQ_EV_SONGPOS  20   /* SND_SEQ_EVENT_SONGPOS: data.control.value = 14-bit SPP;
                                   snd_midi_event_decode turns it into 0xF2 lsb msb */
#define ALSASEQ_EV_START    30
#define ALSASEQ_EV_CONTINUE 31
#define ALSASEQ_EV_STOP     32
#define ALSASEQ_EV_CLOCK    36
#define ALSASEQ_ADDR_SUBSCRIBERS 254
#define ALSASEQ_ADDR_UNKNOWN     253
#define ALSASEQ_QUEUE_DIRECT     253

typedef struct {
    int  (*open)(void **seq, const char *name, int streams, int mode);
    int  (*nonblock)(void *seq, int on);
    int  (*set_client_name)(void *seq, const char *name);
    int  (*client_id)(void *seq);
    int  (*create_simple_port)(void *seq, const char *name, unsigned int caps, unsigned int type);
    int  (*event_output_direct)(void *seq, alsa_ev_t *ev);
    int  (*close)(void *seq);
} alsa_out_api_t;

static int ALSASEQ_MAYBE_UNUSED alsa_out_load(alsa_out_api_t *a) {
    void *h = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen("libasound.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) return -1;
    a->open                = (int (*)(void **, const char *, int, int))dlsym(h, "snd_seq_open");
    a->nonblock            = (int (*)(void *, int))dlsym(h, "snd_seq_nonblock");
    a->set_client_name     = (int (*)(void *, const char *))dlsym(h, "snd_seq_set_client_name");
    a->client_id           = (int (*)(void *))dlsym(h, "snd_seq_client_id");
    a->create_simple_port  = (int (*)(void *, const char *, unsigned, unsigned))dlsym(h, "snd_seq_create_simple_port");
    a->event_output_direct = (int (*)(void *, alsa_ev_t *))dlsym(h, "snd_seq_event_output_direct");
    a->close               = (int (*)(void *))dlsym(h, "snd_seq_close");
    if (!a->open || !a->create_simple_port || !a->event_output_direct) {
        dlclose(h);   /* do not leak the handle on the dlsym error path (the worker retries every 2 s otherwise) */
        return -1;
    }
    return 0;
}

/* one direct event to all subscribers; spp14 is only read for ALSASEQ_EV_SONGPOS
 * (one implementation instead of two near-identical senders).
 * Returns the rc of snd_seq_event_output_direct (0 = sent, <0 = error, e.g.
 * -EAGAIN/-ENOSPC on a nonblocking seq) - the caller counts/logs lost events, because a
 * silently lost CLOCK is a gap for every slave. */
static int ALSASEQ_MAYBE_UNUSED alsa_out_send_ev(alsa_out_api_t *A, void *seq, int port, int ev_type, int spp14) {
    alsa_ev_t ev;
    memset(&ev, 0, sizeof ev);
    ev.type = (unsigned char)ev_type;
    ev.source.port = (unsigned char)port;
    ev.dest.client = ALSASEQ_ADDR_SUBSCRIBERS;
    ev.dest.port   = ALSASEQ_ADDR_UNKNOWN;
    ev.queue = ALSASEQ_QUEUE_DIRECT;
    if (ev_type == ALSASEQ_EV_SONGPOS)
        ev.data.control.value = spp14 & 0x3FFF;   /* 0xF2 lsb msb via snd_midi_event_decode */
    return A->event_output_direct(seq, &ev);
}

/* one system-realtime byte (0xFA/0xFB/0xFC/0xF8 ...) as a direct event to all subscribers;
 * returns the rc of event_output_direct (0 = sent, <0 = lost) */
static int ALSASEQ_MAYBE_UNUSED alsa_out_send(alsa_out_api_t *A, void *seq, int port, int ev_type) {
    return alsa_out_send_ev(A, seq, port, ev_type, 0);
}

/* Song Position Pointer (0xF2 lsb msb) as SND_SEQ_EVENT_SONGPOS to all subscribers;
 * spp14 is the 14-bit value in MIDI beats (6 clocks = 1/16 note each);
 * returns the rc of event_output_direct (0 = sent, <0 = lost) */
static int ALSASEQ_MAYBE_UNUSED alsa_out_send_spp(alsa_out_api_t *A, void *seq, int port, int spp14) {
    return alsa_out_send_ev(A, seq, port, ALSASEQ_EV_SONGPOS, spp14);
}
