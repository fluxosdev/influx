/* USB HID: boot-protocol keyboards, and mice/tablets described by their
 * report descriptor (relative or absolute X/Y, buttons, wheel), falling back
 * to the boot mouse report. USB keyboards do not repeat keys by themselves,
 * so a worker thread does the typematic repeat; it also sets the Caps Lock
 * LED and recovers endpoints that stalled. */
#include "usb.h"
#include "input.h"
#include "mm.h"
#include "string.h"

struct hid_field { int bit, size; int32_t lmin, lmax; bool present; };

struct hid_dev {
    struct usb_interface *intf;
    struct usb_device *dev;
    struct usb_endpoint *ep;
    struct usb_xfer xfer;
    uint8_t *buf;
    bool kbd;
    volatile bool stalled;
    volatile int fails;            /* recoveries since the last good report: sets the retry backoff */
    uint64_t retry_at;
    /* keyboard */
    uint8_t prev[8];
    /* pointer */
    int report_id;                 /* 0: reports carry no ID byte */
    struct hid_field btn[3], x, y, wheel;
    bool abs;
    int buttons;
    struct hid_dev *next;
};

static struct hid_dev *devs;
static struct mutex hid_lock = MUTEX_INIT;     /* devs list, against the worker */
static struct semaphore work_sem;
static bool worker_started;
static volatile int caps;                      /* MOD_CAPS when Caps Lock is on, shared by all keyboards */
static volatile bool leds_dirty;
static spinlock_t rep_lock = SPINLOCK_INIT;
static int rep_usage, rep_mods;                /* key being repeated (HID usage), 0 for none */
static uint64_t rep_next;

/* ---- keyboard ---- */

/* HID usages 0x04..0x38: letters, digits, punctuation (US layout) */
static const char usage_lo[] = "abcdefghijklmnopqrstuvwxyz1234567890\r\x1b\b\t -=[]\\\\;'`,./";
static const char usage_hi[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\r\x1b\b\t _+{}||:\"~<>?";

static int special_key(int u) {
    switch (u) {
    case 0x28: case 0x58: return KEY_ENTER; case 0x29: return KEY_ESC; case 0x2A: return KEY_BACKSPACE; case 0x2B: return KEY_TAB; case 0x2C: return KEY_SPACE;
    case 0x39: return KEY_CAPSLOCK; case 0x49: return KEY_INSERT; case 0x4A: return KEY_HOME; case 0x4B: return KEY_PGUP; case 0x4C: return KEY_DELETE;
    case 0x4D: return KEY_END; case 0x4E: return KEY_PGDN; case 0x4F: return KEY_RIGHT; case 0x50: return KEY_LEFT; case 0x51: return KEY_DOWN; case 0x52: return KEY_UP;
    case 0x65: return KEY_MENU;
    }
    if (u >= 0x3A && u <= 0x45) return KEY_F1 + (u - 0x3A);
    return 0;
}
/* keypad: always digits, like the PS/2 driver */
static char keypad_char(int u) {
    static const char kp[] = "/*-+\r1234567890.";
    return u >= 0x54 && u <= 0x63 ? kp[u - 0x54] : 0;
}

static void key_event(int usage, bool down, int mods) {
    struct input_event ev = { 0 };
    ev.type = down ? INPUT_KEY_DOWN : INPUT_KEY_UP;
    ev.mods = mods;
    int key = special_key(usage);
    char base = 0, c = 0;
    if (!key && usage >= 0x04 && usage <= 0x38) { base = usage_lo[usage - 4]; c = (mods & MOD_SHIFT) ? usage_hi[usage - 4] : base; }
    else if (!key && usage == 0x64) { base = '\\'; c = (mods & MOD_SHIFT) ? '|' : '\\'; }   /* non-US backslash */
    else if (!key && (c = base = keypad_char(usage)) == '\r') { key = KEY_ENTER; base = c = 0; }
    if (key) {
        ev.key = key;
        if (key == KEY_ENTER) ev.ch = '\n'; else if (key == KEY_TAB) ev.ch = '\t'; else if (key == KEY_SPACE) ev.ch = ' ';
        else if (key == KEY_BACKSPACE) ev.ch = 8; else if (key == KEY_ESC) ev.ch = 27;
    } else if (base) {
        if ((mods & MOD_CAPS) && base >= 'a' && base <= 'z') c = (mods & MOD_SHIFT) ? base : base - 32;
        ev.key = base;
        ev.ch = (mods & (MOD_CTRL | MOD_CMD)) ? 0 : (uint8_t)c;
        if ((mods & MOD_CTRL) && !(mods & MOD_CMD) && base >= 'a' && base <= 'z') ev.ch = base - 'a' + 1;
    } else return;
    input_push(&ev);
}

static int report_mods(uint8_t m) {
    int r = 0;
    if (m & 0x11) r |= MOD_CTRL;
    if (m & 0x22) r |= MOD_SHIFT;
    if (m & 0x44) r |= MOD_ALT;
    if (m & 0x88) r |= MOD_CMD;
    return r;
}

static void kbd_report(struct hid_dev *h, const uint8_t *r, uint32_t len) {
    if (len < 8 || r[2] == 0x01) return;   /* short, or phantom state (too many keys) */
    static const int modkeys[8] = { KEY_LCTRL, KEY_LSHIFT, KEY_LALT, KEY_LCMD, KEY_RCTRL, KEY_RSHIFT, KEY_RALT, KEY_RCMD };
    int mods = report_mods(r[0]) | caps;
    for (int b = 0; b < 8; b++) {
        if (((r[0] ^ h->prev[0]) >> b) & 1) {
            struct input_event ev = { 0 };
            ev.type = (r[0] >> b) & 1 ? INPUT_KEY_DOWN : INPUT_KEY_UP; ev.key = modkeys[b]; ev.mods = mods;
            input_push(&ev);
        }
    }
    for (int i = 2; i < 8; i++) {   /* released */
        int u = h->prev[i]; bool still = false;
        if (u < 4) continue;
        for (int j = 2; j < 8; j++) if (r[j] == u) still = true;
        if (still) continue;
        key_event(u, false, mods);
        spin_lock(&rep_lock); if (rep_usage == u) rep_usage = 0; spin_unlock(&rep_lock);
    }
    for (int i = 2; i < 8; i++) {   /* pressed */
        int u = r[i]; bool was = false;
        if (u < 4) continue;
        for (int j = 2; j < 8; j++) if (h->prev[j] == u) was = true;
        if (was) continue;
        if (u == 0x39) { caps ^= MOD_CAPS; mods ^= MOD_CAPS; leds_dirty = true; sem_post(&work_sem); }
        key_event(u, true, mods);
        if (u != 0x39 && u != 0x53) {
            spin_lock(&rep_lock); rep_usage = u; rep_next = time_ms() + 500; spin_unlock(&rep_lock);
            sem_post(&work_sem);
        }
    }
    spin_lock(&rep_lock); rep_mods = mods; spin_unlock(&rep_lock);
    memcpy(h->prev, r, 8);
}

/* ---- pointer ---- */

static int32_t field_get(const uint8_t *r, uint32_t len, const struct hid_field *f) {
    uint32_t v = 0;
    for (int i = 0; i < f->size; i++) {
        int bit = f->bit + i;
        if ((uint32_t)(bit >> 3) >= len) break;
        v |= (uint32_t)((r[bit >> 3] >> (bit & 7)) & 1) << i;
    }
    if (f->lmin < 0 && f->size > 0 && f->size < 32 && (v & (1u << (f->size - 1)))) v |= ~0u << f->size;   /* sign extend */
    return (int32_t)v;
}

static void mouse_report(struct hid_dev *h, const uint8_t *r, uint32_t len) {
    if (h->report_id) {
        if (len < 1 || r[0] != h->report_id) return;
        r++; len--;
    }
    int btn = 0;
    for (int i = 0; i < 3; i++) if (h->btn[i].present && field_get(r, len, &h->btn[i])) btn |= 1 << i;   /* left, right, middle = BTN_* bits */
    int mods = input_modifiers();
    struct input_event ev = { 0 };
    ev.type = INPUT_MOUSE_MOVE; ev.buttons = btn; ev.mods = mods;
    if (h->abs) {
        int32_t range = MAX(h->x.lmax - h->x.lmin, 1), rangey = MAX(h->y.lmax - h->y.lmin, 1);
        ev.abs = true;
        ev.x = CLAMP((int)((int64_t)(field_get(r, len, &h->x) - h->x.lmin) * 65535 / range), 0, 65535);
        ev.y = CLAMP((int)((int64_t)(field_get(r, len, &h->y) - h->y.lmin) * 65535 / rangey), 0, 65535);
        input_push(&ev);
    } else {
        ev.dx = field_get(r, len, &h->x); ev.dy = field_get(r, len, &h->y);
        if (ev.dx || ev.dy) input_push(&ev);
    }
    for (int i = 0; i < 3; i++) {
        int m = 1 << i;
        if ((btn & m) == (h->buttons & m)) continue;
        struct input_event be = { 0 };
        be.type = INPUT_MOUSE_BUTTON; be.button = m; be.pressed = (btn & m) != 0; be.buttons = btn; be.mods = mods;
        input_push(&be);
    }
    h->buttons = btn;
    if (h->wheel.present) {
        int z = field_get(r, len, &h->wheel);
        if (z) { struct input_event se = { 0 }; se.type = INPUT_MOUSE_SCROLL; se.scroll = z > 0 ? 1 : -1; se.buttons = btn; se.mods = mods; input_push(&se); }
    }
}

/* Walk the report descriptor for the first report with X and Y on the
 * generic desktop page, plus its buttons 1-3 and wheel. */
static bool parse_report(struct hid_dev *h, const uint8_t *d, int n) {
    struct hid_global { uint32_t page, rsize, rcount, rid; int32_t lmin, lmax; } g = { 0 }, stack[4];
    int sp = 0;
    uint32_t usages[16]; int nusage = 0; uint32_t umin = 0, umax = 0; bool range = false;
    int offs[256] = { 0 };   /* bit offset so far in each report ID's input report */
    struct hid_field f[6]; int frid[6]; bool frel[6];   /* btn1-3, x, y, wheel */
    memset(f, 0, sizeof f);
    for (int i = 0; i < n; ) {
        uint8_t b = d[i];
        if (b == 0xFE) { if (i + 1 >= n) break; i += 3 + d[i + 1]; continue; }   /* long item */
        int sz = b & 3; if (sz == 3) sz = 4;
        if (i + 1 + sz > n) break;
        uint32_t v = 0;
        for (int k = 0; k < sz; k++) v |= (uint32_t)d[i + 1 + k] << (8 * k);
        int32_t sv = sz == 1 ? (int8_t)v : sz == 2 ? (int16_t)v : (int32_t)v;
        i += 1 + sz;
        switch (b & 0xFC) {
        case 0x04: g.page = v; break;
        case 0x14: g.lmin = sv; break;
        case 0x24: g.lmax = g.lmin < 0 ? sv : (int32_t)v; break;
        case 0x74: g.rsize = v; break;
        case 0x84: g.rid = v & 0xFF; break;
        case 0x94: g.rcount = v; break;
        case 0xA4: if (sp < 4) stack[sp++] = g; break;   /* Push / Pop of the global state: too deep or nothing pushed is ignored */
        case 0xB4: if (sp > 0) g = stack[--sp]; break;
        case 0x08: if (nusage < 16) usages[nusage++] = sz == 4 ? v : g.page << 16 | v; break;
        case 0x18: umin = sz == 4 ? v : g.page << 16 | v; range = true; break;
        case 0x28: umax = sz == 4 ? v : g.page << 16 | v; break;
        case 0x80: {   /* Input */
            uint32_t rsize = g.rsize, rid = g.rid;
            /* only the first 256 elements can be a field we use, but every element takes its
             * bits in the report: the later fields' offsets must count all of them */
            uint32_t nk = MIN(g.rcount, 256u);
            for (uint32_t k = 0; k < nk; k++) {
                uint32_t u = range ? umin + k : k < (uint32_t)nusage ? usages[k] : nusage ? usages[nusage - 1] : 0;
                if (range && umin + k > umax) u = 0;
                int idx = -1;
                if (u >= 0x90001 && u <= 0x90003) idx = u - 0x90001;
                else if (u == 0x10030) idx = 3; else if (u == 0x10031) idx = 4; else if (u == 0x10038) idx = 5;
                if (idx >= 0 && !(v & 1) && !f[idx].present && rsize >= 1 && rsize <= 32) {
                    f[idx] = (struct hid_field){ offs[rid], rsize, g.lmin, g.lmax, true };
                    frid[idx] = rid; frel[idx] = (v & 4) != 0;
                }
                offs[rid] = (int)MIN((uint64_t)offs[rid] + rsize, 1u << 24);   /* no overflow on absurd sizes */
            }
            if (g.rcount > nk) offs[rid] = (int)MIN((uint64_t)offs[rid] + (uint64_t)(g.rcount - nk) * rsize, 1u << 24);
        }
            __attribute__((fallthrough));
        case 0x90: case 0xB0: case 0xA0: case 0xC0:   /* every main item ends the local state */
            nusage = 0; range = false; umin = umax = 0;
            break;
        }
    }
    if (!f[3].present || !f[4].present || frid[4] != frid[3]) return false;
    h->report_id = frid[3];
    h->x = f[3]; h->y = f[4];
    h->abs = !frel[3];
    for (int k = 0; k < 3; k++) if (f[k].present && frid[k] == frid[3]) h->btn[k] = f[k];
    if (f[5].present && frid[5] == frid[3] && frel[5]) h->wheel = f[5];
    return true;
}

static void boot_mouse(struct hid_dev *h) {
    for (int i = 0; i < 3; i++) h->btn[i] = (struct hid_field){ i, 1, 0, 1, true };
    h->x = (struct hid_field){ 8, 8, -127, 127, true };
    h->y = (struct hid_field){ 16, 8, -127, 127, true };
}

/* ---- transfers and the worker ---- */

static void hid_done(struct usb_xfer *x) {
    struct hid_dev *h = x->ctx;
    if (x->status == -ENODEV || x->dev->gone) return;
    if (x->status < 0) { h->stalled = true; sem_post(&work_sem); return; }   /* endpoint halted: the worker resets it */
    h->fails = 0;
    if (h->kbd) kbd_report(h, h->buf, x->actual); else mouse_report(h, h->buf, x->actual);
    usb_submit(x);
}

static void set_leds(struct hid_dev *h) {
    uint8_t leds = caps ? 2 : 0;
    usb_control(h->dev, USB_TYPE_CLASS | USB_RECIP_INTF, 0x09 /* SET_REPORT */, 0x0200, h->intf->num, &leds, 1);
}

static void worker(void *arg) {
    uint64_t retry = 0;   /* earliest pending endpoint recovery retry, 0 for none */
    for (;;) {
        spin_lock(&rep_lock);
        int u = rep_usage; uint64_t next = rep_next;
        spin_unlock(&rep_lock);
        uint64_t now = time_ms();
        uint64_t wake = u ? next : now + 1000;
        if (retry && retry < wake) wake = retry;
        if (u && now >= next) {
            /* pushed under the lock, so a release seen meanwhile cannot be followed by a stale press */
            spin_lock(&rep_lock);
            if (rep_usage == u) { key_event(u, true, rep_mods); rep_next = now + 33; }
            spin_unlock(&rep_lock);
        } else if (wake > now) sem_wait_timeout(&work_sem, wake - now);
        if (leds_dirty) {
            leds_dirty = false;
            mutex_lock(&hid_lock);
            for (struct hid_dev *h = devs; h; h = h->next) if (h->kbd && !h->dev->gone) set_leds(h);
            mutex_unlock(&hid_lock);
        }
        /* Clear the halt and resubmit. A failed attempt (or a transfer that
         * fails again before a good report) stays marked and is retried
         * after 100, 200, 400, then every 500 ms until it works or the device
         * is gone, without the worker spinning on it. */
        retry = 0; now = time_ms();
        mutex_lock(&hid_lock);
        for (struct hid_dev *h = devs; h; h = h->next) {
            if (!h->stalled || h->dev->gone) continue;
            if (h->fails && now < h->retry_at) { if (!retry || h->retry_at < retry) retry = h->retry_at; continue; }
            int nf = MIN(h->fails + 1, 4);
            h->fails = nf;
            h->retry_at = now + MIN(100u << (nf - 1), 500u);
            if (usb_clear_halt(h->dev, h->ep) == 0) {
                h->stalled = false;   /* before the submit: the completion may set it again */
                if (usb_submit(&h->xfer) == 0) continue;
                h->stalled = true;
            }
            if (!retry || h->retry_at < retry) retry = h->retry_at;
        }
        mutex_unlock(&hid_lock);
    }
}

static bool hid_probe(struct usb_interface *i) {
    if (i->cls != 3) return false;
    struct usb_endpoint *ep = usb_find_ep(i, USB_EP_INTR, true);
    if (!ep) return false;
    struct usb_device *d = i->dev;
    struct hid_dev *h = kzalloc(sizeof *h);
    h->intf = i; h->dev = d; h->ep = ep;
    h->kbd = i->subcls == 1 && i->proto == 1;
    bool boot_capable = i->subcls == 1;
    const char *what;
    if (h->kbd) {
        usb_control(d, USB_TYPE_CLASS | USB_RECIP_INTF, 0x0B /* SET_PROTOCOL */, 0 /* boot */, i->num, NULL, 0);
        what = "keyboard";
    } else {
        bool ok = false;
        const uint8_t *hd = usb_find_desc(i, USB_DT_HID);
        if (hd && hd[0] >= 9 && hd[6] == USB_DT_REPORT) {
            int len = MIN(hd[7] | hd[8] << 8, 4096);
            uint8_t *rd = kmalloc(len);
            int r = usb_control(d, USB_DIR_IN | USB_RECIP_INTF, USB_REQ_GET_DESCRIPTOR, USB_DT_REPORT << 8, i->num, rd, len);
            if (r > 0) ok = parse_report(h, rd, r);
            kfree(rd);
        }
        if (ok && boot_capable) usb_control(d, USB_TYPE_CLASS | USB_RECIP_INTF, 0x0B, 1 /* report */, i->num, NULL, 0);
        if (!ok && boot_capable && i->proto == 2) {
            usb_control(d, USB_TYPE_CLASS | USB_RECIP_INTF, 0x0B, 0, i->num, NULL, 0);
            boot_mouse(h);
            ok = true;
        }
        if (!ok) { kfree(h); return false; }   /* not a pointer we understand (gamepad, media keys, ...) */
        what = h->abs ? "tablet (absolute pointer)" : h->wheel.present ? "mouse with wheel" : "mouse";
    }
    usb_control(d, USB_TYPE_CLASS | USB_RECIP_INTF, 0x0A /* SET_IDLE */, 0, i->num, NULL, 0);
    if (!worker_started) { worker_started = true; sem_init(&work_sem, 0); thread_create(kernel_process, "usbhid", worker, NULL); }
    h->buf = usb_buf_alloc(d, MAX(ep->mps, 8));
    h->xfer.dev = d; h->xfer.ep = ep; h->xfer.buf = h->buf; h->xfer.len = ep->mps ? ep->mps : 8;   /* one packet: a longer transfer would wait for the next report */
    h->xfer.done = hid_done; h->xfer.ctx = h;
    mutex_lock(&hid_lock);
    h->next = devs; devs = h;
    mutex_unlock(&hid_lock);
    i->drvdata = h;
    if (h->kbd && caps) set_leds(h);
    usb_submit(&h->xfer);
    kprintf("usb:   interface %d: HID %s\n", i->num, what);
    return true;
}

static void hid_disconnect(struct usb_interface *i) {
    struct hid_dev *h = i->drvdata;
    if (!h) return;
    mutex_lock(&hid_lock);
    for (struct hid_dev **pp = &devs; *pp; pp = &(*pp)->next) if (*pp == h) { *pp = h->next; break; }
    mutex_unlock(&hid_lock);
    if (h->kbd) {   /* release whatever it held down */
        spin_lock(&rep_lock);
        for (int k = 2; k < 8; k++) if (h->prev[k] == rep_usage) rep_usage = 0;
        spin_unlock(&rep_lock);
        uint8_t none[8] = { 0 };
        kbd_report(h, none, 8);
    }
    usb_buf_free(h->dev, h->buf);
    kfree(h);
}

const struct usb_driver usb_hid_driver = { "hid", hid_probe, hid_disconnect };
