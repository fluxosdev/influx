#include "ui.h"
#include "fs.h"
#include "string.h"
#include "acpi.h"

void power_button_pressed(void);

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
    acpi_set_power_button_handler(power_button_pressed);
    fb_boot_progress("Starting Files");
    app_launch(APP_FILES, NULL);
    thread_sleep_ms(1500); app_launch(APP_TERMINAL, "uptime; curl -o /tmp/a http://10.0.2.2:8099/mid.bin; curl -o /tmp/b http://10.0.2.2:8099/mid.bin; curl -o /tmp/c http://10.0.2.2:8099/mid.bin; uptime; ls -l /tmp; ifconfig");   /* TEMPORARY test */
    thread_sleep_ms(50);
    current_thread->proc->name[0] = 0; strcpy(current_thread->proc->name, "kernel");
    strcpy(current_thread->name, "WindowServer");
    wm_run();
}
