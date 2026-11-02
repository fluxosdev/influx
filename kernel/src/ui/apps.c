#include "ui.h"
#include "mm.h"
#include "string.h"
#include "io.h"
#include "fs.h"

void files_main(void *arg);
void terminal_main(void *arg);
void editor_main(void *arg);
void calc_main(void *arg);
void taskman_main(void *arg);
void settings_main(void *arg);

const struct app_info app_table[APP_COUNT] = {
    [APP_FILES]   = { "Files", "files", files_main, false },
    [APP_TERMINAL] = { "Terminal", "terminal", terminal_main, false },
    [APP_EDITOR]   = { "Text Editor", "editor", editor_main, false },
    [APP_CALC]     = { "Calculator", "calculator", calc_main, true },
    [APP_TASKMAN]  = { "Task Manager", "taskman", taskman_main, true },
    [APP_SETTINGS] = { "System Settings", "settings", settings_main, true },
};

struct app_start { int id; char *arg; };
static void app_thread(void *p) {
    struct app_start *s = p;
    int id = s->id; char *arg = s->arg;
    kfree(s);
    wm_notify_app_state();
    app_table[id].main(arg);
    kfree(arg);
    /* process exits when this (last) thread returns */
}

int app_launch(int id, const char *arg) {
    if (id < 0 || id >= APP_COUNT) return -1;
    if (app_table[id].single_instance && app_is_running(id)) {
        app_activate(id);
        if (arg) { /* forward the open request */
            struct process *list[64]; int n = process_list(list, 64);
            for (int i = 0; i < n; i++) if (list[i]->app_id == id && !list[i]->exited) {
                struct window *w = window_top_of_process(list[i]);
                if (w) { struct event e = { .type = EV_OPEN, .path = strdup(arg) }; window_post_event(w, &e); }
            }
        }
        return 0;
    }
    struct process *p = process_create(app_table[id].name, false);
    p->app_id = id;
    strcpy(p->cwd, "/home/flux");
    struct app_start *s = kmalloc(sizeof *s);
    s->id = id; s->arg = arg ? strdup(arg) : NULL;
    thread_create(p, app_table[id].name, app_thread, s);
    return p->pid;
}

bool app_is_running(int id) {
    struct process *list[64]; int n = process_list(list, 64);
    for (int i = 0; i < n; i++) if (list[i]->app_id == id && !list[i]->exited) return true;
    return false;
}

void app_activate(int id) {
    struct process *list[64]; int n = process_list(list, 64);
    for (int i = 0; i < n; i++) if (list[i]->app_id == id && !list[i]->exited) {
        struct window *w = window_top_of_process(list[i]);
        if (w) { window_focus(w); return; }
    }
}

const char *icon_for_file(const char *name, int type) {
    if (type == VN_DIR) {
        if (!strcmp(name, "Documents")) return "folder-documents";
        if (!strcmp(name, "Downloads")) return "folder-downloads";
        if (!strcmp(name, "Pictures")) return "folder-pictures";
        if (!strcmp(name, "Desktop")) return "desktop";
        if (!strcmp(name, "flux")) return "folder-home";
        if (!strcmp(name, ".Trash")) return "trash";
        if (!strcmp(name, "sys") || !strcmp(name, "etc") || !strcmp(name, "dev")) return "folder-system";
        return "folder";
    }
    if (type == VN_BLK) return "drive";
    if (type == VN_DEV) return "file-binary";
    const char *ext = strrchr(name, '.');
    if (!ext) { if (!strcasecmp(name, "Makefile") || !strcasecmp(name, "README")) return !strcasecmp(name, "Makefile") ? "file-makefile" : "file-text"; return "file-generic"; }
    ext++;
    if (!strcasecmp(ext, "txt") || !strcasecmp(ext, "log")) return "file-text";
    if (!strcasecmp(ext, "md")) return "file-markdown";
    if (!strcasecmp(ext, "c")) return "file-c";
    if (!strcasecmp(ext, "h")) return "file-h";
    if (!strcasecmp(ext, "sh") || !strcasecmp(ext, "zsh")) return "file-script";
    if (!strcasecmp(ext, "json")) return "file-json";
    if (!strcasecmp(ext, "png") || !strcasecmp(ext, "jpg") || !strcasecmp(ext, "img") || !strcasecmp(ext, "svg")) return "file-image";
    if (!strcasecmp(ext, "elf") || !strcasecmp(ext, "bin")) return "file-exec";
    if (!strcasecmp(ext, "fnt")) return "file-binary";
    return "file-generic";
}

void app_open_path(const char *path) {
    struct stat st;
    if (vfs_stat(path, &st) < 0) return;
    if (st.type == VN_DIR) { app_launch(APP_FILES, path); return; }
    const char *base = vfs_basename(path);
    if (!strncmp(path, "/bin/", 5) && st.type == VN_FILE) { app_launch(APP_TERMINAL, path); return; }
    const char *icon = icon_for_file(base, st.type);
    if (!strcmp(icon, "file-image") || !strcmp(icon, "file-binary")) { app_launch(APP_FILES, NULL); return; }
    app_launch(APP_EDITOR, path);
}

/* ---- icon cache ---- */
struct icon_entry { char name[48]; int size; struct canvas *img; };
static struct icon_entry icons[160];
static int nicons;
static struct mutex icon_lock = MUTEX_INIT;

struct canvas *icon_get(const char *name, int size) {
    mutex_lock(&icon_lock);
    for (int i = 0; i < nicons; i++) if (icons[i].size == size && !strcmp(icons[i].name, name)) { struct canvas *c = icons[i].img; mutex_unlock(&icon_lock); return c; }
    int src = size <= 32 ? 32 : size <= 64 ? 64 : size <= 128 ? 128 : 256;
    char path[128]; snprintf(path, sizeof path, "/sys/icons/%s-%d.img", name, src);
    struct canvas *img = image_load(path);
    if (!img && src != 128) { snprintf(path, sizeof path, "/sys/icons/%s-128.img", name); img = image_load(path); }
    if (!img && src != 64) { snprintf(path, sizeof path, "/sys/icons/%s-64.img", name); img = image_load(path); }
    if (img && img->w != size) {
        struct canvas *s = canvas_create(size, size);
        gfx_blit_scaled(s, 0, 0, size, size, img);
        canvas_destroy(img);
        img = s;
    }
    if (nicons < (int)ARRAY_LEN(icons)) { strlcpy(icons[nicons].name, name, sizeof icons[nicons].name); icons[nicons].size = size; icons[nicons].img = img; nicons++; }
    mutex_unlock(&icon_lock);
    return img;
}

/* ---- About window ---- */
static void about_thread(void *arg) {
    struct window *w = window_create("", 300, 400, WIN_DIALOG | WIN_UTILITY | WIN_FULL_TITLE);
    struct canvas *c = window_begin_draw(w);
    gfx_fill(c, theme.bg);
    struct canvas *logo = icon_get("logo", 128);
    if (logo) gfx_blit_scaled_tinted(c, (300 - 96) / 2, 44, 96, 96, logo, theme.text);
    font_draw_centered(c, font_ui_title, rect_make(0, 150, 300, 30), theme.text, "inFlux");
    char buf[96];
    snprintf(buf, sizeof buf, "Version %s", INFLUX_VERSION);
    font_draw_centered(c, font_ui, rect_make(0, 182, 300, 20), theme.text_secondary, buf);
    int y = 222;
    char brand[64]; cpu_get_brand(brand, sizeof brand);
    const char *labels[] = { "Processor", "Memory", "Display", "Kernel" };
    char disp[32]; snprintf(disp, sizeof disp, "%d × %d", wm_screen_w(), wm_screen_h());
    char mem[32]; snprintf(mem, sizeof mem, "%lu MB", pmm_total_bytes() >> 20);
    const char *values[] = { brand, mem, disp, "inFlux x86-64 " INFLUX_VERSION };
    for (int i = 0; i < 4; i++) {
        font_draw_aligned(c, font_ui_semibold, rect_make(20, y, 85, 18), theme.text, labels[i], 2);
        font_draw_ellipsized(c, font_ui, 112, y + 1, 170, theme.text_secondary, values[i]);
        y += 22;
    }
    font_draw_centered(c, font_ui_small, rect_make(0, 340, 300, 16), theme.text_tertiary, "Built entirely with Claude by Anthropic");
    font_draw_centered(c, font_ui_small, rect_make(0, 358, 300, 16), theme.text_tertiary, "Icons: Kora  ·  Fonts: Inter, Source Code Pro");
    window_end_draw(w, NULL);
    window_show(w);
    struct event ev;
    while (window_wait_event(w, &ev, -1)) {
        if (ev.type == EV_CLOSE || ev.type == EV_QUIT || (ev.type == EV_KEY_DOWN && ev.key == KEY_ESC)) break;
        if (ev.type == EV_MENU && ev.cmd == CMD_QUIT) break;
    }
    window_destroy(w);
}
void about_show(void) {
    struct process *p = process_create("About inFlux", false);
    thread_create(p, "about", about_thread, NULL);
}

void wm_system_command(int cmd) {
    switch (cmd) {
    case CMD_ABOUT: about_show(); break;
    case CMD_SETTINGS: app_launch(APP_SETTINGS, NULL); break;
    case CMD_TASKMAN: app_launch(APP_TASKMAN, NULL); break;
    case CMD_RESTART: { struct process *p = process_create("Restart", false); extern void power_restart_thread(void *); thread_create(p, "restart", power_restart_thread, NULL); break; }
    case CMD_SHUTDOWN: { struct process *p = process_create("Shut Down", false); extern void power_shutdown_thread(void *); thread_create(p, "shutdown", power_shutdown_thread, NULL); break; }
    }
}

void power_restart(void) {
    vfs_sync();
    kprintf("power: restarting\n");
    cli();
    uint8_t good = 0x02;
    while (good & 0x02) good = inb(0x64);
    outb(0x64, 0xFE);
    /* triple fault fallback */
    struct { uint16_t l; uint64_t b; } PACKED idtr = { 0, 0 };
    __asm__ volatile("lidt %0; int3" :: "m"(idtr));
    for (;;) hlt();
}
void power_shutdown(void) {
    vfs_sync();
    kprintf("power: shutting down\n");
    outw(0x604, 0x2000);    /* QEMU */
    outw(0xB004, 0x2000);   /* Bochs */
    outw(0x4004, 0x3400);   /* VirtualBox */
    cli();
    gfx_fill(screen, RGB(0, 0, 0));
    font_draw_centered(screen, font_ui_large, rect_make(0, screen->h / 2 - 10, screen->w, 20), RGB(200, 200, 200), "It is now safe to turn off your computer.");
    fb_flush_all();
    for (;;) hlt();
}
void power_restart_thread(void *a) { if (dialog_confirm("Restart", "Are you sure you want to restart your computer now?", "Restart", false)) power_restart(); }
void power_shutdown_thread(void *a) { if (dialog_confirm("Shut Down", "Are you sure you want to shut down your computer now?", "Shut Down", false)) power_shutdown(); }
