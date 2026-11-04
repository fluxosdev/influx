/*
 * Saved settings: appearance, wallpaper, clock, time zone, host name, volume
 * and automatic time. Kept as "key=value" lines in the desktop user's home
 * (~/.config/settings.conf), so they persist on an installed system and last
 * for the session on the live CD. prefs_save() is called after each change.
 */
#include "ui.h"
#include "fs.h"
#include "mm.h"
#include "string.h"
#include "net.h"
#include "audio.h"

static const char *prefs_dir(void) { return "/home/flux/.config"; }

static void apply(const char *k, const char *v, bool *dark, int *accent, bool *theme_seen) {
    long n = strtol(v, NULL, 0);
    if (!strcmp(k, "dark")) { *dark = n != 0; *theme_seen = true; }
    else if (!strcmp(k, "accent")) { *accent = (int)n; *theme_seen = true; }
    else if (!strcmp(k, "wallpaper_top")) theme.wallpaper_top = (color_t)strtoul(v, NULL, 16);
    else if (!strcmp(k, "wallpaper_bottom")) theme.wallpaper_bottom = (color_t)strtoul(v, NULL, 16);
    else if (!strcmp(k, "clock_24h")) clock_24h = n != 0;
    else if (!strcmp(k, "tz_offset_minutes")) tz_offset_minutes = CLAMP(n, -12 * 60, 14 * 60);
    else if (!strcmp(k, "hostname")) { if (*v && strlen(v) < sizeof system_hostname) strlcpy(system_hostname, v, sizeof system_hostname); }
    else if (!strcmp(k, "auto_time")) sntp_enabled = n != 0;
    else if (!strcmp(k, "volume")) audio_set_volume((int)n);
}

void prefs_load(void) {
    char path[PATH_MAX]; snprintf(path, sizeof path, "%s/settings.conf", prefs_dir());
    void *data; size_t size;
    if (vfs_read_file(path, &data, &size) < 0) return;
    bool dark = theme.dark, seen = false; int accent = theme.accent_index;
    char *p = data;
    while (*p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        char *eq = strchr(p, '=');
        if (*p != '#' && eq) { *eq = 0; apply(p, eq + 1, &dark, &accent, &seen); }
        if (!nl) break;
        p = nl + 1;
    }
    kfree(data);
    if (seen) theme_apply(dark, accent);
}

void prefs_save(void) {
    vfs_mkdir(prefs_dir());
    char buf[768];
    snprintf(buf, sizeof buf,
             "# inFlux settings, written by System Settings\n"
             "dark=%d\naccent=%d\nwallpaper_top=%08x\nwallpaper_bottom=%08x\nclock_24h=%d\n"
             "tz_offset_minutes=%ld\nhostname=%s\nauto_time=%d\nvolume=%d\n",
             theme.dark, theme.accent_index, theme.wallpaper_top, theme.wallpaper_bottom, clock_24h,
             (long)tz_offset_minutes, system_hostname, sntp_enabled, audio_volume());
    char path[PATH_MAX]; snprintf(path, sizeof path, "%s/settings.conf", prefs_dir());
    int r = vfs_write_file(path, buf, strlen(buf));
    if (r < 0) kprintf("prefs: cannot save %s (%d)\n", path, r);
}
