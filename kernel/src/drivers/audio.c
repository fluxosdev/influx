/*
 * Audio core: one client stream at a time on the most recently registered
 * device (a USB headset plugged in takes over from the built-in card),
 * converted to the device's 16-bit stereo frames (any integer width, float,
 * up to 8 channels) and resampled by linear interpolation when the hardware
 * cannot run at the source rate. The master volume is applied here.
 *
 * /dev/audio accepts raw 48 kHz signed 16-bit little-endian stereo.
 */
#include "audio.h"
#include "fs.h"
#include "mm.h"
#include "sched.h"
#include "string.h"

#define CHUNK 1024   /* source frames converted per step */

struct audio_stream {
    struct audio_device *dev;
    uint32_t in_rate, out_rate;
    int channels, bits, frame_bytes;
    bool is_float;
    uint64_t step, phase;     /* 32.32 source frames per output frame; position after `last` */
    int16_t last[2];
    bool have_last;
    uint64_t out_frames;      /* device frames handed to the driver */
    int16_t *in, *out;        /* CHUNK stereo frames, and the resampled output */
    size_t out_cap;
};

static struct audio_device *devs[4];
static int ndevs;
static int volume = 70;
static struct mutex open_lock = MUTEX_INIT;
static struct audio_stream *active;

static spinlock_t devs_lock = SPINLOCK_INIT;
static bool dev_node;
static const struct vnode_ops dev_ops;

void audio_register(struct audio_device *d) {
    spin_lock(&devs_lock);
    if (ndevs < (int)ARRAY_LEN(devs)) devs[ndevs++] = d;
    spin_unlock(&devs_lock);
    if (!dev_node) { dev_node = true; devfs_add("audio", VN_DEV, &dev_ops, NULL, 0); }
    kprintf("audio: %s, %s\n", d->name, d->detail);
}
/* a stream still open on the device keeps its pointer; the driver fails its writes */
void audio_unregister(struct audio_device *d) {
    spin_lock(&devs_lock);
    for (int i = 0; i < ndevs; i++) if (devs[i] == d) { for (int j = i; j < ndevs - 1; j++) devs[j] = devs[j + 1]; ndevs--; break; }
    spin_unlock(&devs_lock);
    kprintf("audio: %s removed\n", d->name);
}
struct audio_device *audio_device(void) {
    spin_lock(&devs_lock);
    struct audio_device *d = ndevs ? devs[ndevs - 1] : NULL;
    spin_unlock(&devs_lock);
    return d;
}
void audio_set_volume(int percent) { volume = CLAMP(percent, 0, 100); }
int audio_volume(void) { return volume; }

/* float32 -> s16 without the FPU (the kernel has no vector state outside SIMD code) */
static int16_t f32_to_s16(uint32_t b) {
    int exp = (b >> 23) & 0xFF, sh = 135 - exp;
    if (exp == 0) return 0;
    int32_t v = sh <= 0 ? 32767 : sh >= 32 ? 0 : (int32_t)(((b & 0x7FFFFF) | 0x800000) >> sh);
    if (v > 32767) v = 32767;
    return (b >> 31) ? -v : v;
}

static int16_t sample_at(const uint8_t *p, int bits, bool is_float) {
    if (is_float) return f32_to_s16(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24);
    switch (bits) {
    case 8: return (int16_t)((p[0] - 128) << 8);
    case 16: return (int16_t)(p[0] | p[1] << 8);
    case 24: return (int16_t)(p[1] | p[2] << 8);
    default: return (int16_t)(p[2] | p[3] << 8);
    }
}

struct audio_stream *audio_open(uint32_t rate, int channels, int bits, bool is_float, int *err) {
    struct audio_device *d = audio_device();
    if (!d) { *err = -ENODEV; return NULL; }
    if (channels < 1 || channels > 8 || rate < 1000 || rate > 384000 || (is_float ? bits != 32 : (bits != 8 && bits != 16 && bits != 24 && bits != 32))) { *err = -EINVAL; return NULL; }
    mutex_lock(&open_lock);
    if (active) { mutex_unlock(&open_lock); *err = -EBUSY; return NULL; }
    struct audio_stream *s = kzalloc(sizeof *s);
    active = s;
    mutex_unlock(&open_lock);
    s->dev = d;
    s->in_rate = rate; s->channels = channels; s->bits = bits; s->is_float = is_float;
    s->frame_bytes = channels * bits / 8;
    /* the hardware rate: the source's when supported, otherwise resample to 48 kHz or whatever the device runs at */
    if (d->ops->set_rate(d->priv, rate) == 0) s->out_rate = rate;
    else if (d->ops->set_rate(d->priv, 48000) == 0) s->out_rate = 48000;
    else s->out_rate = d->rate;
    s->step = ((uint64_t)s->in_rate << 32) / s->out_rate;
    s->in = kmalloc(CHUNK * 4);
    s->out_cap = (uint64_t)CHUNK * s->out_rate / s->in_rate + 4;
    s->out = s->out_rate == s->in_rate ? NULL : kmalloc(s->out_cap * 4);
    return s;
}

/* linear interpolation between consecutive frames; `last` is the frame before in[0] */
static size_t resample(struct audio_stream *s, const int16_t *in, size_t n) {
    if (!s->have_last) { s->last[0] = in[0]; s->last[1] = in[1]; s->have_last = true; }
    size_t k = 0;
    while ((s->phase >> 32) < n && k < s->out_cap) {
        size_t i = s->phase >> 32;
        int64_t frac = (s->phase >> 16) & 0xFFFF;
        const int16_t *a = i == 0 ? s->last : in + (i - 1) * 2, *b = in + i * 2;
        s->out[k * 2] = a[0] + (((b[0] - a[0]) * frac) >> 16);
        s->out[k * 2 + 1] = a[1] + (((b[1] - a[1]) * frac) >> 16);
        k++;
        s->phase += s->step;
    }
    s->phase -= (uint64_t)n << 32;
    s->last[0] = in[(n - 1) * 2]; s->last[1] = in[(n - 1) * 2 + 1];
    return k;
}

int64_t audio_stream_write(struct audio_stream *s, const void *data, size_t frames, volatile bool *cancel) {
    const uint8_t *src = data;
    int bps = s->bits / 8;
    int32_t gain = volume * volume * 32768 / 10000;   /* square law: even steps to the ear */
    size_t done = 0;
    while (done < frames && !(cancel && *cancel)) {
        size_t n = MIN((size_t)CHUNK, frames - done);
        for (size_t i = 0; i < n; i++) {
            const uint8_t *f = src + (done + i) * s->frame_bytes;
            s->in[i * 2] = sample_at(f, s->bits, s->is_float);
            s->in[i * 2 + 1] = s->channels > 1 ? sample_at(f + bps, s->bits, s->is_float) : s->in[i * 2];
        }
        int16_t *out = s->in;
        size_t outn = n;
        if (s->out) { outn = resample(s, s->in, n); out = s->out; }
        for (size_t i = 0; i < outn * 2; i++) out[i] = (int16_t)((out[i] * gain) >> 15);
        int64_t w = s->dev->ops->write(s->dev->priv, out, outn, cancel);
        if (w < 0) return done ? (int64_t)done : w;   /* the device went away */
        s->out_frames += w;
        done += n;
        if (w < (int64_t)outn) break;
    }
    return done;
}

uint64_t audio_stream_position(struct audio_stream *s) {
    size_t queued = s->dev->ops->queued(s->dev->priv);
    uint64_t played = s->out_frames > queued ? s->out_frames - queued : 0;
    return played * s->in_rate / s->out_rate;
}

void audio_stream_drain(struct audio_stream *s, volatile bool *cancel) { s->dev->ops->drain(s->dev->priv, cancel); }
void audio_stream_stop(struct audio_stream *s) { s->dev->ops->stop(s->dev->priv); }

void audio_stream_close(struct audio_stream *s) {
    mutex_lock(&open_lock);
    if (active == s) active = NULL;
    mutex_unlock(&open_lock);
    kfree(s->in); kfree(s->out); kfree(s);
}

/* ---- /dev/audio ---- */
static int64_t dev_write(struct vnode *v, uint64_t off, const void *buf, size_t n) {
    int err;
    struct audio_stream *s = audio_open(48000, 2, 16, false, &err);
    if (!s) return err;
    audio_stream_write(s, buf, n / 4, (volatile bool *)&current_thread->kill_pending);
    audio_stream_close(s);
    return n;
}
static const struct vnode_ops dev_ops = { .write = dev_write };

void audio_init(void) {
    hda_init();
    if (!audio_device()) kprintf("audio: no sound card (USB audio devices register when plugged in)\n");
}

/* ---- WAV ---- */
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

int wav_parse(struct vnode *v, struct wav_info *w) {
    uint8_t hdr[12];
    memset(w, 0, sizeof *w);
    if (vfs_pread(v, 0, hdr, 12) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) return -EINVAL;
    int tag = 0;
    bool have_fmt = false;
    for (uint64_t off = 12; off + 8 <= v->size; ) {
        uint8_t ch[8];
        if (vfs_pread(v, off, ch, 8) != 8) break;
        uint32_t len = le32(ch + 4);
        if (!memcmp(ch, "fmt ", 4)) {
            uint8_t f[40] = { 0 };
            if (vfs_pread(v, off + 8, f, MIN(len, 40u)) < 16) return -EINVAL;
            tag = f[0] | f[1] << 8;
            w->channels = f[2] | f[3] << 8; w->rate = le32(f + 4); w->bits = f[14] | f[15] << 8;
            if (tag == 0xFFFE && len >= 26) tag = f[24] | f[25] << 8;   /* WAVE_FORMAT_EXTENSIBLE: the sub-format */
            have_fmt = true;
        } else if (!memcmp(ch, "data", 4)) {
            w->data_off = off + 8;
            w->data_len = MIN((uint64_t)len, v->size - w->data_off);   /* streamed files say 0xFFFFFFFF */
            break;
        }
        off += 8 + (uint64_t)len + (len & 1);
    }
    if (!have_fmt || !w->data_off) return -EINVAL;
    if (tag == 3) w->is_float = true;
    else if (tag != 1) return -ENOTSUP;
    if (w->channels < 1 || w->channels > 8 || w->rate < 1000 || w->rate > 384000) return -ENOTSUP;
    if (w->is_float ? w->bits != 32 : (w->bits != 8 && w->bits != 16 && w->bits != 24 && w->bits != 32)) return -ENOTSUP;
    w->frames = w->data_len / (w->channels * w->bits / 8);
    return 0;
}

void wav_describe(const struct wav_info *w, char *out, size_t n) {
    char ch[16];
    if (w->channels == 1) strcpy(ch, "mono"); else if (w->channels == 2) strcpy(ch, "stereo"); else snprintf(ch, sizeof ch, "%d channels", w->channels);
    snprintf(out, n, "%u Hz, %s, %d-bit%s", w->rate, ch, w->bits, w->is_float ? " float" : "");
}
