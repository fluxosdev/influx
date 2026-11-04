#include "ui.h"
#include "fs.h"
#include "string.h"
#include "acpi.h"

void power_button_pressed(void);

static void crash_notice(void *arg) {
    thread_sleep_ms(1500);
    char msg[PATH_MAX + 160];
    const char *name = strrchr(crashlog_previous, '/');
    snprintf(msg, sizeof msg, "A kernel panic report is in ~/Crash Reports (%s).", name ? name + 1 : crashlog_previous);
    notify_post("inFlux restarted after a problem", msg);
}

void desktop_main(void *arg) {
    fb_boot_progress("Loading fonts");
    fonts_init();
    fb_boot_progress("Preparing home directory");
    vfs_mkdir("/home"); vfs_mkdir("/home/flux");
    const char *dirs[] = { "/home/flux/Desktop", "/home/flux/Documents", "/home/flux/Downloads", "/home/flux/Pictures", "/home/flux/.Trash", "/mnt", "/tmp" };
    for (size_t i = 0; i < ARRAY_LEN(dirs); i++) vfs_mkdir(dirs[i]);
    fb_boot_progress("Starting window server");
    ps2_init();
    wm_init();
    acpi_set_power_button_handler(power_button_pressed);
    fb_boot_progress("Starting Files");
    app_launch(APP_FILES, NULL);
    if (crashlog_previous[0]) thread_create(kernel_process, "crash-notice", crash_notice, NULL);
    thread_sleep_ms(50);
    current_thread->proc->name[0] = 0; strcpy(current_thread->proc->name, "kernel");
    strcpy(current_thread->name, "WindowServer");
    wm_run();
}
