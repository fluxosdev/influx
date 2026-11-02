#include "input.h"
#include "arch.h"
#include "io.h"
#include "sched.h"

/* ---- controller ---- */
static void ps2_wait_write(void) { for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) pause(); }
static void ps2_wait_read(void) { for (int i = 0; i < 100000 && !(inb(0x64) & 1); i++) pause(); }
static void ps2_cmd(uint8_t c) { ps2_wait_write(); outb(0x64, c); }
static void ps2_write_data(uint8_t d) { ps2_wait_write(); outb(0x60, d); }
static uint8_t ps2_read_data(void) { ps2_wait_read(); return inb(0x60); }
static void mouse_write(uint8_t v) { ps2_cmd(0xD4); ps2_write_data(v); }
static uint8_t mouse_cmd(uint8_t v) { mouse_write(v); return ps2_read_data(); }

/* ---- keyboard ---- */
static const char keymap_normal[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 8, 9,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 13, 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
static const char keymap_shift[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 8, 9,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 13, 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ', 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
static int kmods;
static bool e0;

static int special_key(uint8_t sc, bool ext) {
    if (ext) switch (sc) {
        case 0x48: return KEY_UP; case 0x50: return KEY_DOWN; case 0x4B: return KEY_LEFT; case 0x4D: return KEY_RIGHT;
        case 0x47: return KEY_HOME; case 0x4F: return KEY_END; case 0x49: return KEY_PGUP; case 0x51: return KEY_PGDN;
        case 0x52: return KEY_INSERT; case 0x53: return KEY_DELETE; case 0x1D: return KEY_RCTRL; case 0x38: return KEY_RALT;
        case 0x5B: return KEY_LCMD; case 0x5C: return KEY_RCMD; case 0x5D: return KEY_MENU; case 0x1C: return KEY_ENTER;
        case 0x35: return '/';
        default: return 0;
    }
    switch (sc) {
        case 0x2A: return KEY_LSHIFT; case 0x36: return KEY_RSHIFT; case 0x1D: return KEY_LCTRL; case 0x38: return KEY_LALT;
        case 0x3A: return KEY_CAPSLOCK; case 0x01: return KEY_ESC; case 0x0E: return KEY_BACKSPACE; case 0x0F: return KEY_TAB;
        case 0x1C: return KEY_ENTER; case 0x39: return KEY_SPACE;
        case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F: case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: return KEY_F1 + (sc - 0x3B);
        case 0x57: return KEY_F11; case 0x58: return KEY_F12;
        default: return 0;
    }
}

static void keyboard_irq(struct regs *r, void *ctx) {
    uint8_t sc = inb(0x60);
    lapic_eoi();
    if (sc == 0xE0) { e0 = true; return; }
    bool ext = e0; e0 = false;
    bool release = sc & 0x80; sc &= 0x7F;
    int key = special_key(sc, ext);
    int modbit = 0;
    switch (key) {
        case KEY_LSHIFT: case KEY_RSHIFT: modbit = MOD_SHIFT; break;
        case KEY_LCTRL: case KEY_RCTRL: modbit = MOD_CTRL; break;
        case KEY_LALT: case KEY_RALT: modbit = MOD_ALT; break;
        case KEY_LCMD: case KEY_RCMD: modbit = MOD_CMD; break;
        case KEY_CAPSLOCK: if (!release) kmods ^= MOD_CAPS; break;
    }
    if (modbit) { if (release) kmods &= ~modbit; else kmods |= modbit; }
    struct input_event ev = { 0 };
    ev.type = release ? INPUT_KEY_UP : INPUT_KEY_DOWN;
    ev.mods = kmods;
    if (key) { ev.key = key; if (key == KEY_ENTER) ev.ch = '\n'; else if (key == KEY_TAB) ev.ch = '\t'; else if (key == KEY_SPACE) ev.ch = ' '; else if (key == KEY_BACKSPACE) ev.ch = 8; else if (key == KEY_ESC) ev.ch = 27; else if (key == '/') ev.ch = '/'; }
    else {
        bool shift = (kmods & MOD_SHIFT) != 0;
        char c = shift ? keymap_shift[sc] : keymap_normal[sc];
        if ((kmods & MOD_CAPS) && c >= 'a' && c <= 'z') c -= 32;
        else if ((kmods & MOD_CAPS) && c >= 'A' && c <= 'Z' && shift) c += 32;
        if (!c) return;
        ev.key = keymap_normal[sc];
        ev.ch = (kmods & (MOD_CTRL | MOD_CMD)) ? 0 : (uint8_t)c;
        if ((kmods & MOD_CTRL) && !(kmods & MOD_CMD)) {
            char base = keymap_normal[sc];
            if (base >= 'a' && base <= 'z') ev.ch = base - 'a' + 1;   /* control characters */
        }
    }
    input_push(&ev);
}

/* ---- mouse ---- */
static uint8_t mpkt[4]; static int midx; static bool wheel;
static int mbuttons;

static void mouse_irq(struct regs *r, void *ctx) {
    uint8_t b = inb(0x60);
    lapic_eoi();
    if (midx == 0 && !(b & 0x08)) return;   /* resync: first byte always has bit 3 set */
    mpkt[midx++] = b;
    int need = wheel ? 4 : 3;
    if (midx < need) return;
    midx = 0;
    int dx = mpkt[1], dy = mpkt[2];
    if (mpkt[0] & 0x10) dx -= 256;
    if (mpkt[0] & 0x20) dy -= 256;
    if (mpkt[0] & 0xC0) return;   /* overflow */
    int btn = mpkt[0] & 7;
    struct input_event ev = { 0 };
    if (dx || dy) { ev.type = INPUT_MOUSE_MOVE; ev.dx = dx; ev.dy = -dy; ev.buttons = btn; ev.mods = kmods; input_push(&ev); }
    if (btn != mbuttons) {
        for (int i = 0; i < 3; i++) {
            int m = 1 << i;
            if ((btn & m) != (mbuttons & m)) {
                struct input_event be = { 0 };
                be.type = INPUT_MOUSE_BUTTON; be.button = m; be.pressed = (btn & m) != 0; be.buttons = btn; be.mods = kmods;
                input_push(&be);
            }
        }
        mbuttons = btn;
    }
    if (wheel) {
        int8_t z = (int8_t)(mpkt[3] & 0x0F); if (z & 8) z |= 0xF0;
        if (z) { struct input_event se = { 0 }; se.type = INPUT_MOUSE_SCROLL; se.scroll = -z; se.buttons = btn; se.mods = kmods; input_push(&se); }
    }
}

void ps2_init(void) {
    ps2_cmd(0xAD); ps2_cmd(0xA7);          /* disable both ports */
    while (inb(0x64) & 1) inb(0x60);      /* flush */
    ps2_cmd(0x20); uint8_t cfg = ps2_read_data();
    cfg |= 0x03;                          /* enable IRQ1 + IRQ12 */
    cfg &= ~0x30;                         /* clocks on */
    cfg |= 0x40;                          /* translation to set 1 */
    ps2_cmd(0x60); ps2_write_data(cfg);
    ps2_cmd(0xAE); ps2_cmd(0xA8);          /* enable ports */
    /* keyboard: enable scanning */
    ps2_write_data(0xF4); ps2_read_data();
    /* mouse: defaults, try to enable the scroll wheel (IntelliMouse) */
    mouse_cmd(0xF6);
    mouse_cmd(0xF3); mouse_cmd(200); mouse_cmd(0xF3); mouse_cmd(100); mouse_cmd(0xF3); mouse_cmd(80);
    mouse_cmd(0xF2); uint8_t id = ps2_read_data();
    wheel = (id == 3);
    mouse_cmd(0xF3); mouse_cmd(100);     /* sample rate */
    mouse_cmd(0xE8); mouse_cmd(2);       /* resolution 4 counts/mm */
    mouse_cmd(0xF4);                     /* enable streaming */
    while (inb(0x64) & 1) inb(0x60);
    irq_register(IRQ_BASE + IRQ_KEYBOARD, keyboard_irq, NULL);
    irq_register(IRQ_BASE + IRQ_MOUSE, mouse_irq, NULL);
    ioapic_route_irq(IRQ_KEYBOARD, IRQ_BASE + IRQ_KEYBOARD);
    ioapic_route_irq(IRQ_MOUSE, IRQ_BASE + IRQ_MOUSE);
    kprintf("ps2: keyboard + mouse initialised (wheel %s)\n", wheel ? "yes" : "no");
}
