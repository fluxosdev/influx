#pragma once
#include "kernel.h"
/* System sounds (drivers/sounds.c): fire and forget, skipped while a program is playing audio */
enum { SOUND_NOTIFY, SOUND_ALERT, SOUND_DONE };
void sound_play(int which);
extern bool sounds_enabled;   /* Settings: play sound effects */
