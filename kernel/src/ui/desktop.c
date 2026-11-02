#include "ui.h"
#include "fs.h"
#include "string.h"

void desktop_main(void *arg) {
    fb_boot_progress("Loading fonts");
    fonts_init();
    fb_boot_progress("Preparing home directory");
    vfs_mkdir("/home"); vfs_mkdir("/home/flux");
    const char *dirs[] = { "/home/flux/Desktop", "/home/flux/Documents", "/home/flux/Downloads", "/home/flux/Pictures", "/home/flux/.Trash", "/mnt", "/tmp" };
    for (size_t i = 0; i < ARRAY_LEN(dirs); i++) vfs_mkdir(dirs[i]);
    fb_boot_progress("Starting window server");
    input_init();
    ps2_init();
    wm_init();
    fb_boot_progress("Starting Files");
    app_launch(APP_FILES, NULL);
    thread_sleep_ms(50);
    current_thread->proc->name[0] = 0; strcpy(current_thread->proc->name, "kernel");
    strcpy(current_thread->name, "WindowServer");
    wm_run();
}
