#pragma once
#include "kernel.h"

/*
 * Sound output. A hardware driver (drivers/hda.c, usb/uac.c) registers an audio_device
 * that plays interleaved signed 16-bit stereo frames at a rate it supports.
 * The audio core (drivers/audio.c) sits in front of it: one client stream at
 * a time, conversion from any PCM layout and rate, the master volume, the
 * /dev/audio node and the WAV reader used by `play` and the player.
 */

struct audio_ops {
    int (*set_rate)(void *priv, uint32_t hz);                          /* -EINVAL if unsupported */
    int64_t (*write)(void *priv, const int16_t *frames, size_t count, volatile bool *cancel);   /* blocks for room */
    void (*drain)(void *priv, volatile bool *cancel);                  /* until everything queued has played */
    void (*stop)(void *priv);                                          /* drop what is queued */
    size_t (*queued)(void *priv);                                      /* frames not yet played */
};
struct audio_device {
    char name[96];            /* controller, e.g. "Intel HD Audio (8086:293e)" */
    char detail[160];         /* codec and outputs */
    const struct audio_ops *ops;
    void *priv;
    uint32_t rate;            /* current hardware rate */
};
void audio_register(struct audio_device *d);     /* becomes the output device */
void audio_unregister(struct audio_device *d);   /* hot unplug */
void audio_init(void);        /* probes the drivers, creates /dev/audio */
struct audio_device *audio_device(void);   /* the newest device, NULL without sound hardware */

/* master volume, 0..100 */
void audio_set_volume(int percent);
int audio_volume(void);

/* client streams: any rate, 1..8 channels, 8/16/24/32-bit integer or 32-bit float */
struct audio_stream;
struct audio_stream *audio_open(uint32_t rate, int channels, int bits, bool is_float, int *err);   /* -EBUSY, -ENODEV, -EINVAL */
int64_t audio_stream_write(struct audio_stream *s, const void *data, size_t frames, volatile bool *cancel);
uint64_t audio_stream_position(struct audio_stream *s);   /* source frames played so far */
void audio_stream_drain(struct audio_stream *s, volatile bool *cancel);   /* wait until it has all played */
void audio_stream_stop(struct audio_stream *s);           /* silence now, dropping what is queued */
void audio_stream_close(struct audio_stream *s);          /* release the device (queued audio keeps playing) */

/* WAV files */
struct wav_info { uint32_t rate; int channels, bits; bool is_float; uint64_t data_off, data_len, frames; };
struct vnode;
int wav_parse(struct vnode *v, struct wav_info *w);       /* 0, -EINVAL (not WAV), -ENOTSUP (codec) */
void wav_describe(const struct wav_info *w, char *out, size_t n);   /* "44100 Hz, stereo, 16-bit" */

/* drivers */
void hda_init(void);
