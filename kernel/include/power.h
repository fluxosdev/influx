#pragma once
#include "kernel.h"
/* Laptop power through ACPI (acpi/power.c) and processor power management (arch/cpupower.c) */
struct power_status {
    bool battery_present, charging, discharging, critical, ac_known, on_ac;
    int batteries, percent;           /* percent -1: unknown */
    int minutes_left;                 /* while discharging, 0 if unknown */
    uint64_t charge_now, charge_full; /* firmware units (mWh or mAh) */
};
bool power_status(struct power_status *out);   /* false without a battery; cached for 30 s */
void power_battery_changed(void);              /* re-read at the next power_status() */
bool backlight_available(void);
int backlight_get(void);                       /* percent, -1 if unknown */
int backlight_set(int percent);                /* the level used, or -errno */
void backlight_step(int dir);
struct aml_node;
bool power_notify(struct aml_node *n, uint64_t value);
void cpupower_init(void);                      /* each CPU: idle states and frequency scaling */
void cpupower_tick(void);                      /* each CPU's timer: the frequency governor */
void cpu_idle_wait(void);                      /* this CPU has nothing to run (interrupts off on entry, on on return) */
const char *cpupower_describe(void);
void notify_post(const char *title, const char *body);
static inline int abs_diff(int a, int b) { return a > b ? a - b : b - a; }
