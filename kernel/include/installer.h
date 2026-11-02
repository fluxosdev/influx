#pragma once
#include "blockdev.h"
typedef void (*install_progress_fn)(void *ctx, int percent, const char *message);
int install_run(struct blockdev *disk, install_progress_fn progress, void *ctx);   /* 0 on success */
bool install_is_installed_system(void);
int display_persist_resolution(int w, int h);
