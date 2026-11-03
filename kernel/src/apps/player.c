/* Audio Player: plays WAV files opened from Files (or `open song.wav`). */
#include "ui.h"
#include "audio.h"
#include "fs.h"
#include "mm.h"
#include "string.h"

#define PW 440
#define PH 172
#define PAD 20
#define TRACK_Y 88
#define CTRL_Y 138

enum { PCMD_PLAY = CMD_APP + 1, PCMD_STOP, PCMD_BACK, PCMD_FWD, PCMD_VOLUP, PCMD_VOLDOWN };
static const struct menu_item file_items[] = { MENU("Close", CMD_CLOSE, 'w', MOD_CMD), MENU_SEP, MENU("Quit Audio Player", CMD_QUIT, 'q', MOD_CMD) };
static const struct menu_item ctl_items[] = {
    /* unmodified keys are shown here but handled by the window (the WM matches only ⌘ shortcuts) */
    MENU("Play / Pause", PCMD_PLAY, 0, 0), MENU("Stop", PCMD_STOP, '.', MOD_CMD), MENU_SEP,
    MENU("Back 5 Seconds", PCMD_BACK, KEY_LEFT, 0), MENU("Forward 5 Seconds", PCMD_FWD, KEY_RIGHT, 0), MENU_SEP,
    MENU("Volume Up", PCMD_VOLUP, KEY_UP, MOD_CMD), MENU("Volume Down", PCMD_VOLDOWN, KEY_DOWN, MOD_CMD),
};
static const struct menu menus[] = { { "File", file_items, ARRAY_LEN(file_items) }, { "Controls", ctl_items, ARRAY_LEN(ctl_items) } };
static const struct app_menus player_menus = { menus, ARRAY_LEN(menus) };

struct player {
    struct window *w;
    char name[NAME_MAX + 1];
    struct vnode *vn;
    struct wav_info wav;
    char desc[64], error[96];
    volatile bool playing;        /* playback thread running */
    int threads;                  /* playback threads that may still touch this struct */
    volatile bool stop_req;
    volatile uint64_t pos;        /* frame the playback is at */
    enum { DRAG_NONE, DRAG_SEEK, DRAG_VOLUME } drag;
    int drag_x;
    int pressed;                  /* 1 play, 2 stop */
};

/* ---- playback thread ---- */
static void play_thread(void *arg) {
    struct player *p = arg;
    int err;
    struct audio_stream *s = audio_open(p->wav.rate, p->wav.channels, p->wav.bits, p->wav.is_float, &err);
    if (!s) {
        strlcpy(p->error, err == -ENODEV ? "No sound hardware was found." : err == -EBUSY ? "The audio device is in use." : "This format cannot be played.", sizeof p->error);
        p->playing = false;
        struct event e = { .type = EV_USER }; window_post_event(p->w, &e);
        return;
    }
    size_t fb = p->wav.channels * p->wav.bits / 8, cap = 32768 / fb * fb;
    uint8_t *buf = kmalloc(cap);
    uint64_t base = p->pos;
    for (uint64_t off = base * fb; off < p->wav.data_len && !p->stop_req; ) {
        int64_t got = vfs_pread(p->vn, p->wav.data_off + off, buf, MIN((uint64_t)cap, p->wav.data_len - off));
        if (got <= 0) break;
        got -= got % fb;
        if (!got) break;
        if (audio_stream_write(s, buf, got / fb, &p->stop_req) < 0) {   /* USB device unplugged: keep the position */
            strlcpy(p->error, "The audio device was disconnected.", sizeof p->error);
            p->stop_req = true;
            break;
        }
        off += got;
        p->pos = base + audio_stream_position(s);
    }
    if (!p->stop_req) audio_stream_drain(s, &p->stop_req);
    if (p->stop_req) { p->pos = MIN(base + audio_stream_position(s), p->wav.frames); audio_stream_stop(s); }
    else p->pos = 0;   /* played to the end: rewind */
    audio_stream_close(s);
    kfree(buf);
    p->playing = false;
    struct event e = { .type = EV_USER }; window_post_event(p->w, &e);
    __atomic_sub_fetch(&p->threads, 1, __ATOMIC_SEQ_CST);   /* last access to p */
}

static void start(struct player *p) {
    if (p->playing || !p->vn || !p->wav.frames) return;
    if (p->pos >= p->wav.frames) p->pos = 0;
    p->error[0] = 0;
    p->stop_req = false;
    p->playing = true;
    __atomic_add_fetch(&p->threads, 1, __ATOMIC_SEQ_CST);
    thread_create(current_thread->proc, "playback", play_thread, p);
}
static void stop(struct player *p) {
    if (!p->playing) return;
    p->stop_req = true;
    while (p->playing) thread_sleep_ms(5);
}
static void seek(struct player *p, int64_t frame) {
    bool was = p->playing;
    stop(p);
    if (frame >= (int64_t)p->wav.frames) { p->pos = 0; return; }   /* past the end: finished, rewound */
    p->pos = MAX(frame, 0);
    if (was) start(p);
}

static void load(struct player *p, const char *path) {
    stop(p);
    if (p->vn) vnode_unref(p->vn);
    p->vn = NULL; p->pos = 0; p->error[0] = 0; p->desc[0] = 0;
    memset(&p->wav, 0, sizeof p->wav);
    strlcpy(p->name, vfs_basename(path), sizeof p->name);
    window_set_title(p->w, p->name);
    p->vn = vfs_lookup(path);
    if (!p->vn) { strlcpy(p->error, "The file could not be opened.", sizeof p->error); return; }
    int r = wav_parse(p->vn, &p->wav);
    if (r < 0) {
        strlcpy(p->error, r == -ENOTSUP ? "Only PCM and floating-point WAV files can be played." : "This is not a WAV file.", sizeof p->error);
        vnode_unref(p->vn); p->vn = NULL;
        return;
    }
    wav_describe(&p->wav, p->desc, sizeof p->desc);
}

/* ---- drawing ---- */
static struct rect track_rect(void) { return rect_make(PAD, TRACK_Y, PW - PAD * 2, 4); }
static struct rect play_rect(void) { return rect_make(PW / 2 - 20, CTRL_Y - 20, 40, 40); }
static struct rect stop_rect(void) { return rect_make(PW / 2 - 66, CTRL_Y - 15, 30, 30); }
static struct rect vol_rect(void) { return rect_make(PW - PAD - 96, CTRL_Y - 2, 96, 4); }

static void fmt_time(uint64_t s, char *out, size_t n) { snprintf(out, n, "%lu:%02lu", s / 60, s % 60); }

static void fill_triangle_right(struct canvas *c, int x, int y, int h, color_t col) {
    for (int i = 0; i < h; i++) { int half = i < h / 2 ? i : h - 1 - i; gfx_hline(c, x, y + i, half * 2 * 7 / 8 + 1, col); }
}

static void slider(struct canvas *c, struct rect r, int fill_w, bool knob) {
    gfx_fill_rounded_rect(c, r.x, r.y, r.w, r.h, 2, theme.dark ? RGBA(255, 255, 255, 0x30) : RGBA(0, 0, 0, 0x1C));
    if (fill_w > 0) gfx_fill_rounded_rect(c, r.x, r.y, fill_w, r.h, 2, theme.accent);
    if (knob) {
        int kx = r.x + fill_w, ky = r.y + r.h / 2;
        gfx_fill_circle(c, kx, ky, 7, theme.dark ? RGB(0xE8, 0xE8, 0xEA) : RGB(0xFF, 0xFF, 0xFF));
        gfx_draw_circle(c, kx, ky, 7, theme.control_border);
    }
}

static void draw(struct player *p) {
    struct canvas *c = window_begin_draw(p->w);
    gfx_fill(c, theme.bg);
    struct canvas *icon = icon_get("file-audio", 48);
    if (icon) gfx_blit(c, PAD, 18, icon);
    int tx = PAD + 60, tw = PW - tx - PAD;
    font_draw_ellipsized(c, font_ui_semibold, tx, 22, tw, theme.text, p->name[0] ? p->name : "No file");
    if (p->error[0]) font_draw_ellipsized(c, font_ui_small, tx, 44, tw, RGB(0xE0, 0x40, 0x40), p->error);
    else font_draw_ellipsized(c, font_ui_small, tx, 44, tw, theme.text_secondary, p->desc);

    /* progress */
    struct rect tr = track_rect();
    uint64_t frames = p->wav.frames, pos = p->drag == DRAG_SEEK ? (uint64_t)CLAMP(p->drag_x - tr.x, 0, tr.w) * frames / tr.w : MIN(p->pos, frames);
    int fill = frames ? (int)(pos * tr.w / frames) : 0;
    slider(c, tr, fill, frames && (p->playing || pos || p->drag == DRAG_SEEK));
    char a[16], b[16];
    uint32_t rate = p->wav.rate ? p->wav.rate : 1;
    fmt_time(pos / rate, a, sizeof a);
    b[0] = '-'; fmt_time((frames - pos + rate - 1) / rate, b + 1, sizeof b - 1);
    font_draw(c, font_ui_small, tr.x, TRACK_Y + 12, theme.text_secondary, a);
    font_draw(c, font_ui_small, tr.x + tr.w - font_text_width(font_ui_small, b), TRACK_Y + 12, theme.text_secondary, b);

    /* transport */
    bool enabled = p->vn && frames;
    color_t glyph = enabled ? theme.text : theme.text_disabled;
    struct rect pr = play_rect(), sr = stop_rect();
    gfx_fill_circle(c, pr.x + 20, pr.y + 20, 20, p->pressed == 1 ? theme.control_hover : theme.control_bg);
    gfx_draw_circle(c, pr.x + 20, pr.y + 20, 20, theme.control_border);
    if (p->playing) { gfx_fill_rounded_rect(c, pr.x + 13, pr.y + 12, 5, 16, 1, glyph); gfx_fill_rounded_rect(c, pr.x + 22, pr.y + 12, 5, 16, 1, glyph); }
    else fill_triangle_right(c, pr.x + 15, pr.y + 11, 18, glyph);
    gfx_fill_circle(c, sr.x + 15, sr.y + 15, 15, p->pressed == 2 ? theme.control_hover : theme.control_bg);
    gfx_draw_circle(c, sr.x + 15, sr.y + 15, 15, theme.control_border);
    gfx_fill_rounded_rect(c, sr.x + 10, sr.y + 10, 10, 10, 1, glyph);

    /* volume */
    struct rect vr = vol_rect();
    int sx = vr.x - 24, sy = vr.y - 6;
    gfx_fill_rect(c, sx, sy + 4, 4, 8, theme.text_secondary);
    for (int i = 0; i < 8; i++) gfx_vline(c, sx + 4 + i / 2 + (i & 1), sy + 4 - i / 2, 8 + i, theme.text_secondary);
    slider(c, vr, audio_volume() * vr.w / 100, true);
    window_end_draw(p->w, NULL);
}

/* ---- events ---- */
static void mouse_down(struct player *p, int x, int y) {
    struct rect tr = track_rect(), vr = vol_rect();
    if (rect_contains(play_rect(), x, y)) p->pressed = 1;
    else if (rect_contains(stop_rect(), x, y)) p->pressed = 2;
    else if (p->wav.frames && x >= tr.x - 6 && x <= tr.x + tr.w + 6 && y >= tr.y - 10 && y <= tr.y + tr.h + 10) { p->drag = DRAG_SEEK; p->drag_x = x; }
    else if (x >= vr.x - 8 && x <= vr.x + vr.w + 8 && y >= vr.y - 10 && y <= vr.y + vr.h + 10) { p->drag = DRAG_VOLUME; audio_set_volume((x - vr.x) * 100 / vr.w); }
}
static void mouse_up(struct player *p, int x, int y) {
    if (p->pressed == 1 && rect_contains(play_rect(), x, y)) { if (p->playing) stop(p); else start(p); }
    else if (p->pressed == 2 && rect_contains(stop_rect(), x, y)) { stop(p); p->pos = 0; }
    else if (p->drag == DRAG_SEEK) { struct rect tr = track_rect(); seek(p, (int64_t)CLAMP(x - tr.x, 0, tr.w) * p->wav.frames / tr.w); }
    p->pressed = 0; p->drag = DRAG_NONE;
}

static void command(struct player *p, int cmd) {
    int64_t step = (int64_t)p->wav.rate * 5;
    switch (cmd) {
    case PCMD_PLAY: if (p->playing) stop(p); else start(p); break;
    case PCMD_STOP: stop(p); p->pos = 0; break;
    case PCMD_BACK: seek(p, (int64_t)p->pos - step); break;
    case PCMD_FWD: seek(p, (int64_t)p->pos + step); break;
    case PCMD_VOLUP: audio_set_volume(audio_volume() + 5); break;
    case PCMD_VOLDOWN: audio_set_volume(audio_volume() - 5); break;
    }
}

void player_main(void *arg) {
    struct player *p = kzalloc(sizeof *p);
    p->w = window_create("Audio Player", PW, PH, WIN_CLOSABLE | WIN_MINIMIZABLE);
    window_set_menus(p->w, &player_menus);
    if (arg) load(p, arg);
    else strlcpy(p->error, "Open a WAV file from Files.", sizeof p->error);
    draw(p);
    window_show(p->w);
    start(p);
    window_set_timer(p->w, 250);
    struct event ev;
    while (window_wait_event(p->w, &ev, -1)) {
        if (ev.type == EV_CLOSE || ev.type == EV_QUIT || (ev.type == EV_MENU && (ev.cmd == CMD_QUIT || ev.cmd == CMD_CLOSE))) break;
        switch (ev.type) {
        case EV_OPEN: load(p, ev.path); kfree(ev.path); start(p); break;
        case EV_MOUSE_DOWN: if (ev.button == BTN_LEFT) mouse_down(p, ev.x, ev.y); break;
        case EV_MOUSE_MOVE:
            if (p->drag == DRAG_SEEK) p->drag_x = ev.x;
            else if (p->drag == DRAG_VOLUME) { struct rect vr = vol_rect(); audio_set_volume((ev.x - vr.x) * 100 / vr.w); }
            break;
        case EV_MOUSE_UP: if (ev.button == BTN_LEFT) mouse_up(p, ev.x, ev.y); break;
        case EV_KEY_DOWN:
            if (ev.mods & MOD_CMD) break;
            if (ev.key == KEY_SPACE || ev.ch == ' ') command(p, PCMD_PLAY);
            else if (ev.key == KEY_LEFT) command(p, PCMD_BACK);
            else if (ev.key == KEY_RIGHT) command(p, PCMD_FWD);
            break;
        case EV_MENU: command(p, ev.cmd); break;
        }
        draw(p);
    }
    stop(p);
    /* 'playing' drops before the thread posts its final event: wait until it is really done with p */
    while (__atomic_load_n(&p->threads, __ATOMIC_SEQ_CST)) thread_sleep_ms(5);
    if (p->vn) vnode_unref(p->vn);
    window_destroy(p->w);
    kfree(p);
}
