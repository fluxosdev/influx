/*
 * System sounds: short tones synthesized on the fly (no sound files), played
 * through the audio core at the master volume. A sound never waits for the
 * device: if a program is playing audio (the stream is busy) it is skipped.
 * The kernel has no floating point, so each tone is an integer resonator
 * (y[n] = 2cos(w) y[n-1] - y[n-2], Q30) with an exponential decay.
 */
#include "audio.h"
#include "sched.h"
#include "mm.h"
#include "string.h"
#include "sounds.h"

bool sounds_enabled = true;

#define RATE 48000
struct tone { int64_t k, s1; int ms, start_ms; };   /* k = 2cos(w), s1 = sin(w), both Q30 */

/* coefficients for 48 kHz: E6 1318.5, B6 1975.5, A5 880, E5 659.3, G5 784, C6 1046.5 Hz */
#define E6 2115578141LL, 184401157LL
#define B6 2076079596LL, 274581286LL
#define A5 2133251770LL, 123412826LL
#define E5 2139492524LL, 92544258LL
#define G5 2136185247LL, 109998420LL
#define C6 2127366040LL, 146628572LL

static const struct tone notify_tones[] = { { E6, 180, 0 }, { B6, 260, 90 } };
static const struct tone alert_tones[] = { { A5, 140, 0 }, { E5, 220, 110 } };
static const struct tone done_tones[] = { { G5, 120, 0 }, { C6, 220, 80 } };

static size_t synth(int16_t *out, const struct tone *t, int n) {
    int total_ms = 0;
    for (int i = 0; i < n; i++) total_ms = MAX(total_ms, t[i].start_ms + t[i].ms);
    size_t frames = (size_t)total_ms * RATE / 1000;
    memset(out, 0, frames * 4);
    for (int i = 0; i < n; i++) {
        size_t start = (size_t)t[i].start_ms * RATE / 1000, len = (size_t)t[i].ms * RATE / 1000;
        int64_t y2 = 0, y1 = t[i].s1;
        int64_t env = 9000LL << 16;                           /* peak amplitude ~0.27 */
        int64_t decay = 65536 - (65536LL * 5 / (int64_t)len);  /* about e^-5 over the tone */
        for (size_t s = 0; s < len && start + s < frames; s++) {
            int64_t y = ((t[i].k * y1) >> 30) - y2;
            y2 = y1; y1 = y;
            int64_t attack = s < 96 ? (int64_t)s * 65536 / 96 : 65536;   /* 2 ms fade-in: no click */
            int64_t v = (y >> 14) * (env >> 16) / 32768 * attack / 65536;
            env = env * decay >> 16;
            int32_t mixed = out[(start + s) * 2] + (int32_t)v;
            mixed = CLAMP(mixed, -32767, 32767);
            out[(start + s) * 2] = out[(start + s) * 2 + 1] = (int16_t)mixed;
        }
    }
    return frames;
}

static void sound_thread(void *arg) {
    int which = (int)(intptr_t)arg;
    const struct tone *t = which == SOUND_ALERT ? alert_tones : which == SOUND_DONE ? done_tones : notify_tones;
    int n = which == SOUND_ALERT ? ARRAY_LEN(alert_tones) : which == SOUND_DONE ? ARRAY_LEN(done_tones) : ARRAY_LEN(notify_tones);
    int16_t *buf = kmalloc_try((size_t)RATE * 4);   /* up to a second */
    if (!buf) return;
    size_t frames = synth(buf, t, n);
    int err;
    struct audio_stream *s = audio_open(RATE, 2, 16, false, &err);
    if (s) {
        audio_stream_write(s, buf, frames, NULL);
        audio_stream_drain(s, NULL);
        audio_stream_close(s);
    }
    kfree(buf);
}

void sound_play(int which) {
    if (!sounds_enabled || !audio_device()) return;
    thread_create(kernel_process, "sound", sound_thread, (void *)(intptr_t)which);
}
