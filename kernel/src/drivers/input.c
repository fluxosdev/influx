#include "input.h"
#include "sched.h"
#include "string.h"

#define QSIZE 256
static struct input_event queue[QSIZE];
static int qhead, qtail;
static spinlock_t qlock = SPINLOCK_INIT;
static struct semaphore qsem;
static int mods;

void input_init(void) { sem_init(&qsem, 0); }
int input_modifiers(void) { return mods; }

void input_push(const struct input_event *ev) {
    spin_lock(&qlock);
    int next = (qtail + 1) % QSIZE;
    if (next == qhead) {
        /* full: coalesce by dropping the oldest mouse move */
        spin_unlock(&qlock);
        return;
    }
    /* coalesce consecutive mouse moves */
    if (ev->type == INPUT_MOUSE_MOVE && qtail != qhead) {
        int last = (qtail + QSIZE - 1) % QSIZE;
        if (queue[last].type == INPUT_MOUSE_MOVE) {
            queue[last].dx += ev->dx; queue[last].dy += ev->dy; queue[last].buttons = ev->buttons;
            spin_unlock(&qlock);
            sem_post(&qsem);
            return;
        }
    }
    queue[qtail] = *ev;
    if (ev->type == INPUT_KEY_DOWN || ev->type == INPUT_KEY_UP) mods = ev->mods;
    qtail = next;
    spin_unlock(&qlock);
    sem_post(&qsem);
}

bool input_pop(struct input_event *ev, int timeout_ms) {
    if (timeout_ms < 0) sem_wait(&qsem);
    else if (!sem_wait_timeout(&qsem, timeout_ms)) return false;
    spin_lock(&qlock);
    if (qhead == qtail) { spin_unlock(&qlock); return false; }
    *ev = queue[qhead];
    qhead = (qhead + 1) % QSIZE;
    spin_unlock(&qlock);
    return true;
}
void input_wake(void) { sem_post(&qsem); }
