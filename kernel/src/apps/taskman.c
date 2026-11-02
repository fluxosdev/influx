/* Task Manager: live process/thread table with CPU and memory statistics. */
#include "ui.h"
#include "mm.h"
#include "string.h"

#define TOOLBAR_H 44
#define ROW_H 24
#define HDR_H 26
#define SUMMARY_H 96
#define HIST 60

struct prow { int pid; char name[32]; int cpu; uint64_t mem; int threads; bool exited; bool user; int app_id; };
struct tm_app {
    struct window *w;
    struct prow rows[64]; int nrows;
    int selected_pid;
    int sort_col; bool sort_desc;      /* 0 name 1 cpu 2 mem 3 threads 4 pid */
    int cpu_hist[HIST]; int hist_pos;
    int pressed_btn;
    uint64_t last_click; int last_click_pid;
    int total_threads;
    bool show_threads;
};
enum { TCMD_QUIT_PROC = CMD_APP + 1, TCMD_SORT_CPU, TCMD_SORT_MEM, TCMD_SORT_NAME, TCMD_SORT_PID, TCMD_INFO, TCMD_REFRESH };
static const struct menu_item proc_items[] = { MENU("Force Quit Process…", TCMD_QUIT_PROC, KEY_BACKSPACE, MOD_CMD), MENU("Process Info", TCMD_INFO, 'i', MOD_CMD), MENU_SEP, MENU("Refresh Now", TCMD_REFRESH, 'r', MOD_CMD), MENU_SEP, MENU("Close", CMD_CLOSE, 'w', MOD_CMD), MENU("Quit Task Manager", CMD_QUIT, 'q', MOD_CMD) };
static const struct menu_item view_items[] = { MENU("Sort by CPU", TCMD_SORT_CPU, '1', MOD_CMD), MENU("Sort by Memory", TCMD_SORT_MEM, '2', MOD_CMD), MENU("Sort by Name", TCMD_SORT_NAME, '3', MOD_CMD), MENU("Sort by PID", TCMD_SORT_PID, '4', MOD_CMD) };
static const struct menu menus[] = { { "Process", proc_items, ARRAY_LEN(proc_items) }, { "View", view_items, ARRAY_LEN(view_items) } };
static const struct app_menus tm_menus = { menus, ARRAY_LEN(menus) };

static void format_mem(uint64_t b, char *out, size_t n) {
    if (b < 1024) snprintf(out, n, "%lu B", b);
    else if (b < 1024 * 1024) snprintf(out, n, "%lu KB", b / 1024);
    else snprintf(out, n, "%lu.%lu MB", b >> 20, ((b >> 10) % 1024) / 103);
}

static int cmp_rows(const struct prow *a, const struct prow *b, int col) {
    switch (col) {
    case 1: return a->cpu - b->cpu;
    case 2: return a->mem < b->mem ? -1 : a->mem > b->mem;
    case 3: return a->threads - b->threads;
    case 4: return a->pid - b->pid;
    default: return strcasecmp(a->name, b->name);
    }
}

static void refresh(struct tm_app *t) {
    sched_sample_cpu();
    struct process *list[64]; int n = process_list(list, 64);
    t->nrows = 0; t->total_threads = 0;
    for (int i = 0; i < n; i++) {
        struct process *p = list[i];
        struct prow *r = &t->rows[t->nrows++];
        r->pid = p->pid; strlcpy(r->name, p->name, sizeof r->name); r->exited = p->exited; r->user = p->is_user; r->app_id = p->app_id;
        r->mem = p->heap_bytes + (p->pml4 ? vmm_user_pages(p->pml4) * PAGE_SIZE : 0);
        r->cpu = 0; r->threads = 0;
        struct thread *th; list_for_each_entry(th, &p->threads, proc_link) { r->cpu += th->cpu_percent; r->threads++; }
        if (p->pid == 1) r->cpu = 0;   /* kernel: idle thread would dominate */
        t->total_threads += r->threads;
    }
    for (int i = 1; i < t->nrows; i++) { struct prow k = t->rows[i]; int j = i - 1; while (j >= 0) { int c = cmp_rows(&t->rows[j], &k, t->sort_col); if (t->sort_desc) c = -c; if (c <= 0) break; t->rows[j + 1] = t->rows[j]; j--; } t->rows[j + 1] = k; }
    t->cpu_hist[t->hist_pos] = 100 - sched_idle_percent();
    t->hist_pos = (t->hist_pos + 1) % HIST;
}

static struct rect btn_quit(struct tm_app *t) { return rect_make(14, TITLEBAR_H + 8, 32, 28); }
static struct rect table_rect(struct tm_app *t) { return rect_make(0, TITLEBAR_H + TOOLBAR_H, t->w->frame.w, t->w->frame.h - TITLEBAR_H - TOOLBAR_H - SUMMARY_H); }
static int col_x(struct tm_app *t, int col) { int W = t->w->frame.w; static const int widths[] = { 0, 70, 90, 70, 60, 70 }; int x = W; for (int i = 5; i >= col; i--) x -= widths[i]; return col == 0 ? 12 : x; }

static void draw(struct tm_app *t) {
    struct canvas *c = window_begin_draw(t->w);
    int W = c->w, H = c->h;
    gfx_fill(c, theme.bg);
    /* toolbar */
    gfx_fill_rect(c, 0, 0, W, TITLEBAR_H + TOOLBAR_H, theme.titlebar);
    gfx_hline(c, 0, TITLEBAR_H + TOOLBAR_H - 1, W, theme.titlebar_line);
    struct rect bq = btn_quit(t);
    bool can_quit = t->selected_pid > 1;
    gfx_fill_rounded_rect(c, bq.x, bq.y, bq.w, bq.h, 6, t->pressed_btn == 1 ? (theme.dark ? RGBA(255, 255, 255, 0x30) : RGBA(0, 0, 0, 0x18)) : theme.control_bg);
    gfx_draw_rounded_rect(c, bq.x, bq.y, bq.w, bq.h, 6, theme.control_border);
    color_t xc = can_quit ? theme.text_secondary : theme.text_disabled;
    gfx_draw_circle(c, bq.x + 16, bq.y + 14, 7, xc);
    gfx_line(c, bq.x + 13, bq.y + 11, bq.x + 19, bq.y + 17, xc); gfx_line(c, bq.x + 19, bq.y + 11, bq.x + 13, bq.y + 17, xc);
    font_draw(c, font_ui_semibold_large, 60, TITLEBAR_H + 8 + (28 - font_height(font_ui_semibold_large)) / 2, theme.text, "All Processes");
    /* segmented sort control */
    const char *segs[] = { "CPU", "Memory", "Name" }; int segw = 64; int sx = W - 14 - 3 * segw;
    gfx_fill_rounded_rect(c, sx, TITLEBAR_H + 8, 3 * segw, 28, 6, theme.dark ? RGBA(255, 255, 255, 0x14) : RGBA(0, 0, 0, 0x0C));
    for (int i = 0; i < 3; i++) {
        int col = i == 0 ? 1 : i == 1 ? 2 : 0; bool act = t->sort_col == col;
        if (act) { gfx_fill_rounded_rect(c, sx + i * segw + 2, TITLEBAR_H + 10, segw - 4, 24, 5, theme.control_bg); gfx_draw_rounded_rect(c, sx + i * segw + 2, TITLEBAR_H + 10, segw - 4, 24, 5, theme.control_border); }
        font_draw_centered(c, act ? font_ui_semibold : font_ui, rect_make(sx + i * segw, TITLEBAR_H + 8, segw, 28), theme.text, segs[i]);
    }
    /* table header */
    struct rect tr = table_rect(t);
    gfx_fill_rect(c, 0, tr.y, W, HDR_H, theme.bg_secondary);
    gfx_hline(c, 0, tr.y + HDR_H - 1, W, theme.separator);
    const char *cols[] = { "Process Name", "% CPU", "Memory", "Threads", "PID", "State" };
    int sortmap[] = { 0, 1, 2, 3, 4, -1 };
    for (int i = 0; i < 6; i++) {
        char h[32]; snprintf(h, sizeof h, "%s%s", cols[i], sortmap[i] == t->sort_col ? (t->sort_desc ? " ▼" : " ▲") : "");
        struct rect hr = rect_make(col_x(t, i), tr.y, (i < 5 ? col_x(t, i + 1) : W) - col_x(t, i) - 8, HDR_H);
        font_draw_aligned(c, font_ui_medium_small, hr, theme.text_secondary, h, i == 0 ? 0 : 2);
        if (i) gfx_vline(c, col_x(t, i) - 6, tr.y + 5, HDR_H - 10, theme.separator);
    }
    /* rows */
    struct canvas rows = canvas_sub(c, rect_make(0, tr.y + HDR_H, W, tr.h - HDR_H));
    for (int i = 0; i < t->nrows; i++) {
        struct prow *r = &t->rows[i];
        int y = i * ROW_H;
        if (y > rows.h) break;
        bool sel = r->pid == t->selected_pid;
        if (sel) gfx_fill_rect(&rows, 0, y, W, ROW_H, theme.selection);
        else if (i % 2) gfx_fill_rect(&rows, 0, y, W, ROW_H, theme.dark ? RGBA(255, 255, 255, 0x08) : RGBA(0, 0, 0, 0x05));
        color_t tc = sel ? theme.accent_text : theme.text, sc = sel ? theme.accent_text : theme.text_secondary;
        struct canvas *ic = NULL;
        if (r->app_id >= 0 && r->app_id < APP_COUNT) ic = icon_get(app_table[r->app_id].icon, 32);
        else if (r->pid == 1) ic = icon_get("logo", 64);
        else ic = icon_get(r->user ? "file-exec" : "terminal", 32);
        if (ic) { if (r->pid == 1) gfx_blit_scaled_tinted(&rows, 14, y + 4, 16, 16, ic, tc); else gfx_blit_scaled(&rows, 12, y + 3, 18, 18, ic); }
        font_draw_ellipsized(&rows, font_ui, 38, y + (ROW_H - font_height(font_ui)) / 2, col_x(t, 1) - 50, tc, r->name);
        char buf[32];
        snprintf(buf, sizeof buf, "%d.0", r->cpu); font_draw_aligned(&rows, font_ui, rect_make(col_x(t, 1), y, col_x(t, 2) - col_x(t, 1) - 8, ROW_H), sc, buf, 2);
        format_mem(r->mem, buf, sizeof buf); font_draw_aligned(&rows, font_ui, rect_make(col_x(t, 2), y, col_x(t, 3) - col_x(t, 2) - 8, ROW_H), sc, buf, 2);
        snprintf(buf, sizeof buf, "%d", r->threads); font_draw_aligned(&rows, font_ui, rect_make(col_x(t, 3), y, col_x(t, 4) - col_x(t, 3) - 8, ROW_H), sc, buf, 2);
        snprintf(buf, sizeof buf, "%d", r->pid); font_draw_aligned(&rows, font_ui, rect_make(col_x(t, 4), y, col_x(t, 5) - col_x(t, 4) - 8, ROW_H), sc, buf, 2);
        font_draw_aligned(&rows, font_ui, rect_make(col_x(t, 5), y, W - col_x(t, 5) - 12, ROW_H), sc, r->exited ? "Zombie" : r->user ? "User" : "Kernel", 2);
    }
    /* summary */
    int sy = H - SUMMARY_H;
    gfx_fill_rect(c, 0, sy, W, SUMMARY_H, theme.bg_secondary);
    gfx_hline(c, 0, sy, W, theme.separator);
    /* CPU graph */
    int gx = 16, gy = sy + 14, gw = 200, gh = SUMMARY_H - 28;
    gfx_fill_rounded_rect(c, gx, gy, gw, gh, 4, theme.dark ? RGB(0x14, 0x14, 0x16) : RGB(0xFF, 0xFF, 0xFF));
    gfx_draw_rounded_rect(c, gx, gy, gw, gh, 4, theme.border);
    for (int i = 0; i < HIST; i++) {
        int v = t->cpu_hist[(t->hist_pos + i) % HIST];
        int bh = v * (gh - 4) / 100;
        int bx = gx + 2 + i * (gw - 4) / HIST, bw = (gw - 4) / HIST;
        if (bh > 0) gfx_fill_rect(c, bx, gy + gh - 2 - bh, bw - 1, bh, color_with_alpha(theme.accent, 0xC0));
    }
    int cur = t->cpu_hist[(t->hist_pos + HIST - 1) % HIST];
    char buf[64];
    int lx = gx + gw + 16;
    snprintf(buf, sizeof buf, "CPU  %d%%", cur); font_draw(c, font_ui_semibold, lx, gy, theme.text, buf);
    snprintf(buf, sizeof buf, "Idle: %d%%", 100 - cur); font_draw(c, font_ui_small, lx, gy + 20, theme.text_secondary, buf);
    snprintf(buf, sizeof buf, "Context switches: %lu", sched_context_switches); font_draw(c, font_ui_small, lx, gy + 36, theme.text_secondary, buf);
    uint64_t s = system_ticks / 1000;
    snprintf(buf, sizeof buf, "Uptime: %lu:%02lu:%02lu", s / 3600, (s / 60) % 60, s % 60); font_draw(c, font_ui_small, lx, gy + 52, theme.text_secondary, buf);
    /* memory */
    int mx = lx + 190;
    uint64_t total = pmm_total_bytes(), used = pmm_used_bytes();
    char a[32], b[32]; format_mem(used, a, sizeof a); format_mem(total, b, sizeof b);
    snprintf(buf, sizeof buf, "Memory  %s of %s", a, b); font_draw(c, font_ui_semibold, mx, gy, theme.text, buf);
    int bw = MIN(220, W - mx - 20);
    gfx_fill_rounded_rect(c, mx, gy + 22, bw, 10, 5, theme.dark ? RGB(0x3A, 0x3A, 0x3E) : RGB(0xDD, 0xDD, 0xE0));
    gfx_fill_rounded_rect(c, mx, gy + 22, MAX(10, (int)(bw * used / total)), 10, 5, RGB(0x34, 0xC7, 0x59));
    snprintf(buf, sizeof buf, "Kernel heap: %lu KB used", heap_used_bytes() / 1024); font_draw(c, font_ui_small, mx, gy + 36, theme.text_secondary, buf);
    snprintf(buf, sizeof buf, "Processes: %d   Threads: %d", t->nrows, t->total_threads); font_draw(c, font_ui_small, mx, gy + 52, theme.text_secondary, buf);
    window_end_draw(t->w, NULL);
}

static void force_quit(struct tm_app *t) {
    if (t->selected_pid <= 1) return;
    struct prow *r = NULL; for (int i = 0; i < t->nrows; i++) if (t->rows[i].pid == t->selected_pid) r = &t->rows[i];
    if (!r) return;
    if (r->pid == current_thread->proc->pid) { if (dialog_confirm("Task Manager", "Quit Task Manager?", "Quit", false)) process_exit(0); return; }
    char msg[160]; snprintf(msg, sizeof msg, "Do you want to force “%s” (PID %d) to quit?\nYou will lose any unsaved changes.", r->name, r->pid);
    if (dialog_confirm("Force Quit", msg, "Force Quit", true)) process_kill(r->pid, 9);
}
static void process_info(struct tm_app *t) {
    struct prow *r = NULL; for (int i = 0; i < t->nrows; i++) if (t->rows[i].pid == t->selected_pid) r = &t->rows[i];
    if (!r) return;
    struct process *p = process_find(r->pid);
    char mem[32]; format_mem(r->mem, mem, sizeof mem);
    char msg[400]; int o = snprintf(msg, sizeof msg, "%s\n\nPID: %d\nType: %s\nThreads: %d\nMemory: %s\nCPU: %d%%", r->name, r->pid, r->user ? "User process" : "Kernel process", r->threads, mem, r->cpu);
    if (p) { struct thread *th; list_for_each_entry(th, &p->threads, proc_link) if (o < (int)sizeof msg - 40) o += snprintf(msg + o, sizeof msg - o, "\n  thread %d “%s”: %lu ms", th->tid, th->name, th->cpu_ticks); if (p->is_user == false && p->cwd[0]) o += snprintf(msg + o, sizeof msg - o, "\nWorking directory: %s", p->cwd); }
    dialog_message("Process Info", msg);
}

void taskman_main(void *arg) {
    struct tm_app *t = kzalloc(sizeof *t);
    t->sort_col = 1; t->sort_desc = true;
    t->w = window_create("Task Manager", 760, 500, WIN_STANDARD | WIN_FULL_TITLE);
    window_set_min_size(t->w, 560, 360);
    window_set_menus(t->w, &tm_menus);
    refresh(t);
    draw(t);
    window_show(t->w);
    window_set_timer(t->w, 1000);
    struct event ev; bool quit = false;
    while (!quit && window_wait_event(t->w, &ev, -1)) {
        switch (ev.type) {
        case EV_QUIT: case EV_CLOSE: quit = true; break;
        case EV_TIMER: refresh(t); break;
        case EV_MENU:
            if (ev.cmd == CMD_QUIT || ev.cmd == CMD_CLOSE) quit = true;
            else if (ev.cmd == TCMD_QUIT_PROC) { force_quit(t); refresh(t); }
            else if (ev.cmd == TCMD_INFO) process_info(t);
            else if (ev.cmd == TCMD_REFRESH) refresh(t);
            else if (ev.cmd == TCMD_SORT_CPU) { t->sort_col = 1; t->sort_desc = true; refresh(t); }
            else if (ev.cmd == TCMD_SORT_MEM) { t->sort_col = 2; t->sort_desc = true; refresh(t); }
            else if (ev.cmd == TCMD_SORT_NAME) { t->sort_col = 0; t->sort_desc = false; refresh(t); }
            else if (ev.cmd == TCMD_SORT_PID) { t->sort_col = 4; t->sort_desc = false; refresh(t); }
            break;
        case EV_MOUSE_DOWN: {
            if (ev.button != BTN_LEFT) break;
            if (rect_contains(btn_quit(t), ev.x, ev.y)) { t->pressed_btn = 1; break; }
            int W = t->w->frame.w, sx = W - 14 - 3 * 64;
            if (ev.y >= TITLEBAR_H + 8 && ev.y < TITLEBAR_H + 36 && ev.x >= sx && ev.x < sx + 192) { int i = (ev.x - sx) / 64; t->sort_col = i == 0 ? 1 : i == 1 ? 2 : 0; t->sort_desc = i != 2; refresh(t); break; }
            struct rect tr = table_rect(t);
            if (ev.y >= tr.y && ev.y < tr.y + HDR_H) { int col = 0; for (int i = 1; i < 5; i++) if (ev.x >= col_x(t, i) - 6) col = i; if (t->sort_col == col) t->sort_desc = !t->sort_desc; else { t->sort_col = col; t->sort_desc = col != 0 && col != 4; } refresh(t); break; }
            if (ev.y >= tr.y + HDR_H && ev.y < tr.y + tr.h) {
                int i = (ev.y - tr.y - HDR_H) / ROW_H;
                if (i < t->nrows) {
                    bool dbl = t->rows[i].pid == t->last_click_pid && system_ticks - t->last_click < 450;
                    t->selected_pid = t->rows[i].pid; t->last_click = system_ticks; t->last_click_pid = t->selected_pid;
                    if (dbl) process_info(t);
                } else t->selected_pid = 0;
            }
            break; }
        case EV_MOUSE_UP: if (t->pressed_btn == 1 && rect_contains(btn_quit(t), ev.x, ev.y)) { t->pressed_btn = 0; draw(t); force_quit(t); refresh(t); } t->pressed_btn = 0; break;
        case EV_KEY_DOWN:
            if (ev.key == KEY_DOWN || ev.key == KEY_UP) { int idx = -1; for (int i = 0; i < t->nrows; i++) if (t->rows[i].pid == t->selected_pid) idx = i; idx = CLAMP(idx + (ev.key == KEY_DOWN ? 1 : -1), 0, t->nrows - 1); t->selected_pid = t->rows[idx].pid; }
            else if (ev.key == KEY_DELETE || (ev.key == KEY_BACKSPACE && (ev.mods & (MOD_CMD | MOD_CTRL)))) { force_quit(t); refresh(t); }
            else if (ev.key == KEY_ENTER) process_info(t);
            break;
        default: break;
        }
        if (!quit) draw(t);
    }
    window_destroy(t->w);
    kfree(t);
}
