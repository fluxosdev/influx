#pragma once
#include "kernel.h"

enum {
    KEY_NONE = 0, KEY_BACKSPACE = 8, KEY_TAB = 9, KEY_ENTER = 13, KEY_ESC = 27, KEY_SPACE = 32, KEY_DELETE = 127,
    KEY_UP = 0x100, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN, KEY_INSERT,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_LSHIFT, KEY_RSHIFT, KEY_LCTRL, KEY_RCTRL, KEY_LALT, KEY_RALT, KEY_LCMD, KEY_RCMD, KEY_CAPSLOCK, KEY_MENU,
};
#define MOD_SHIFT 1
#define MOD_CTRL  2
#define MOD_ALT   4
#define MOD_CMD   8
#define MOD_CAPS  16

enum { INPUT_KEY_DOWN = 1, INPUT_KEY_UP, INPUT_MOUSE_MOVE, INPUT_MOUSE_BUTTON, INPUT_MOUSE_SCROLL };
#define BTN_LEFT 1
#define BTN_RIGHT 2
#define BTN_MIDDLE 4

struct input_event {
    int type;
    int key;          /* KEY_* or ASCII for printable */
    uint32_t ch;      /* unicode char produced (0 if none) */
    int mods;
    int dx, dy;       /* mouse motion */
    int buttons;      /* current button state */
    int button;       /* button that changed */
    bool pressed;
    int scroll;       /* +1 up, -1 down */
};

void input_init(void);
void input_push(const struct input_event *ev);
bool input_pop(struct input_event *ev, int timeout_ms);   /* false on timeout */
void input_wake(void);                                    /* wake a waiter without an event */
int input_modifiers(void);
void ps2_init(void);
