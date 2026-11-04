#pragma once
#include "fs.h"

#define TTY_IN_SIZE 4096
struct tty {
    char in[TTY_IN_SIZE]; int in_head, in_tail;
    spinlock_t lock;
    struct waitq in_wq;
    bool raw;                 /* raw: bytes delivered immediately; cooked: line editing + echo */
    char line[1024]; int line_len;
    char cooked[2048]; int cooked_len, cooked_pos;   /* completed lines waiting to be read */
    bool eof_pending;
    void (*output)(void *priv, const char *s, size_t n);
    void *priv;
    int fg_pid;
    void (*on_interrupt)(void *priv);
    volatile bool *intr_flag;  /* raw mode: Ctrl+C also raises this (the shell's cancel for builtins) */
    int cols, rows;
    struct vnode *vn;
    bool closed;
    bool have_termios;
    uint8_t termios[36];      /* the last TCSETS (struct ktermios in tty.c) */
};
struct tty *tty_create(void (*output)(void *, const char *, size_t), void *priv);
void tty_destroy(struct tty *t);
void tty_input(struct tty *t, const char *s, size_t n);        /* from the terminal emulator */
int64_t tty_read(struct tty *t, void *buf, size_t n);          /* blocking */
void tty_write(struct tty *t, const char *s, size_t n);
void tty_set_raw(struct tty *t, bool raw);
struct file *tty_file(struct tty *t);

/* pipes */
int pipe_create(struct file **rd, struct file **wr);
