/*
 * Laptop power information through ACPI: control-method batteries
 * (PNP0C0A: _STA, _BIX or _BIF for capacities, _BST for the state), the AC
 * adapter (ACPI0003: _PSR) and the display backlight (an ACPI video output
 * with _BCL/_BCM/_BQC, as firmware provides for laptop panels). Batteries are
 * re-read every 30 seconds and when firmware sends Notify 0x80/0x81; the
 * brightness keys arrive as Notify 0x86/0x87 on the video output.
 */
#include "acpi_internal.h"
#include "power.h"
#include "string.h"
#include "mm.h"

#define MAX_BAT 2
static struct aml_node *bat_nodes[MAX_BAT], *ac_node, *bl_node;
static int nbat;
static int bl_levels[101], nbl;     /* _BCL's brightness levels (the first two entries, AC/battery defaults, dropped) */
static struct power_status cached;
static uint64_t cached_at;
static struct mutex lock = MUTEX_INIT;
static bool scanned;

static bool walk_find(struct aml_node *n, int depth, void *ctx) {
    if (!n->obj || n->obj->type != AML_DEVICE) {
        /* a backlight is the device that holds _BCM */
        if (!bl_node && !strcmp(n->name, "_BCM") && n->parent) bl_node = n->parent;
        return true;
    }
    if (ns_hid_matches(n, "PNP0C0A") && nbat < MAX_BAT) bat_nodes[nbat++] = n;
    else if (ns_hid_matches(n, "ACPI0003") && !ac_node) ac_node = n;
    return true;
}

static bool present(struct aml_node *dev) {
    uint64_t sta;
    if (aml_eval_int(dev, "_STA", &sta) < 0) return true;   /* no _STA: present */
    return (sta & 0x10) != 0;                               /* battery present bit */
}

static bool pkg_int(struct aml_obj *pkg, size_t i, uint64_t *out) {
    if (!pkg || pkg->type != AML_PACKAGE || i >= pkg->pkg.n) return false;
    struct aml_obj *v = aml_value_of(pkg->pkg.e[i]);
    bool ok = obj_to_int(v, out);
    obj_put(v);
    return ok;
}

static void scan(void) {
    if (scanned || !acpi_ready()) return;
    aml_lock();
    ns_walk(aml_root, walk_find, NULL);
    if (bl_node) {
        struct aml_obj *bcl = NULL;
        if (aml_eval_path(bl_node, "_BCL", NULL, 0, &bcl) == 0 && bcl && bcl->type == AML_PACKAGE) {
            for (size_t i = 2; i < bcl->pkg.n && nbl < 101; i++) { uint64_t v; if (pkg_int(bcl, i, &v) && v <= 100) bl_levels[nbl++] = (int)v; }
            /* ascending, without duplicates */
            for (int i = 0; i < nbl; i++) for (int j = i + 1; j < nbl; j++) if (bl_levels[j] < bl_levels[i]) { int t = bl_levels[i]; bl_levels[i] = bl_levels[j]; bl_levels[j] = t; }
            int k = 0; for (int i = 0; i < nbl; i++) if (!k || bl_levels[i] != bl_levels[k - 1]) bl_levels[k++] = bl_levels[i];
            nbl = k;
        }
        obj_put(bcl);
        if (nbl < 2) bl_node = NULL;
    }
    aml_unlock();
    scanned = true;
    char path[96];
    for (int i = 0; i < nbat; i++) { ns_path(bat_nodes[i], path, sizeof path); kprintf("power: battery %s\n", path); }
    if (ac_node) { ns_path(ac_node, path, sizeof path); kprintf("power: AC adapter %s\n", path); }
    if (bl_node) { ns_path(bl_node, path, sizeof path); kprintf("power: backlight %s (%d levels, %d..%d%%)\n", path, nbl, bl_levels[0], bl_levels[nbl - 1]); }
}

static void read_battery(struct aml_node *b, struct power_status *st) {
    if (!present(b)) return;
    uint64_t full = 0, design = 0, unit = 0, rem = 0, rate = 0, state = 0;
    struct aml_obj *info = NULL, *bst = NULL;
    /* _BIX has a revision field first, so its fields sit one place later than _BIF's */
    if (aml_eval_path(b, "_BIX", NULL, 0, &info) == 0 && info && info->type == AML_PACKAGE) { pkg_int(info, 1, &unit); pkg_int(info, 2, &design); pkg_int(info, 3, &full); }
    else { obj_put(info); info = NULL; if (aml_eval_path(b, "_BIF", NULL, 0, &info) == 0 && info) { pkg_int(info, 0, &unit); pkg_int(info, 1, &design); pkg_int(info, 2, &full); } }
    obj_put(info);
    if (aml_eval_path(b, "_BST", NULL, 0, &bst) == 0 && bst) { pkg_int(bst, 0, &state); pkg_int(bst, 1, &rate); pkg_int(bst, 2, &rem); }
    obj_put(bst);
    const uint64_t unknown = 0xFFFFFFFF;
    if (full == 0 || full == unknown) full = design;
    if (full == 0 || full == unknown || rem == unknown) return;
    st->battery_present = true;
    st->batteries++;
    st->charge_now += rem; st->charge_full += full;
    if (state & 1) st->discharging = true;
    if (state & 2) st->charging = true;
    if (state & 4) st->critical = true;
    if (rate && rate != unknown && (state & 1)) st->minutes_left += (int)(rem * 60 / rate);
    (void)unit;
}

bool power_status(struct power_status *out) {
    mutex_lock(&lock);
    scan();
    if (!cached_at || time_ms() - cached_at > 30000) {
        struct power_status st = { 0 };
        aml_lock();
        for (int i = 0; i < nbat; i++) read_battery(bat_nodes[i], &st);
        uint64_t psr;
        if (ac_node && aml_eval_int(ac_node, "_PSR", &psr) == 0) { st.ac_known = true; st.on_ac = psr != 0; }
        aml_unlock();
        st.percent = st.charge_full ? (int)MIN(100ULL, st.charge_now * 100 / st.charge_full) : -1;
        bool was_low = cached.battery_present && cached.percent > 10;
        cached = st; cached_at = time_ms();
        if (was_low && st.discharging && st.percent >= 0 && st.percent <= 10) {
            char m[96]; snprintf(m, sizeof m, "%d%% left. Connect the computer to power.", st.percent);
            notify_post("Battery low", m);
        }
    }
    *out = cached;
    mutex_unlock(&lock);
    return out->battery_present;
}

void power_battery_changed(void) { __atomic_store_n(&cached_at, 0, __ATOMIC_RELAXED); }   /* lock-free: called under the interpreter lock */

/* ---- backlight ---- */
bool backlight_available(void) { mutex_lock(&lock); scan(); bool r = bl_node != NULL; mutex_unlock(&lock); return r; }

int backlight_get(void) {
    mutex_lock(&lock);
    scan();
    uint64_t v = 0;
    int r = -1;
    if (bl_node) { aml_lock(); if (aml_eval_int(bl_node, "_BQC", &v) == 0 && v <= 100) r = (int)v; aml_unlock(); }
    mutex_unlock(&lock);
    return r;
}

/* percent: the nearest level _BCL offers */
int backlight_set(int percent) {
    mutex_lock(&lock);
    scan();
    if (!bl_node) { mutex_unlock(&lock); return -ENODEV; }
    int best = bl_levels[0];
    for (int i = 0; i < nbl; i++) if (abs_diff(bl_levels[i], percent) < abs_diff(best, percent)) best = bl_levels[i];
    struct aml_obj *arg = obj_int((uint64_t)best);
    aml_lock();
    int r = aml_eval_path(bl_node, "_BCM", &arg, 1, NULL);
    aml_unlock();
    obj_put(arg);
    mutex_unlock(&lock);
    return r < 0 ? r : best;
}

/* one step up or down (the brightness keys) */
void backlight_step(int dir) {
    int cur = backlight_get();
    if (cur < 0) return;
    mutex_lock(&lock);
    int target = cur;
    if (dir > 0) { for (int i = 0; i < nbl; i++) if (bl_levels[i] > cur) { target = bl_levels[i]; break; } }
    else { for (int i = nbl - 1; i >= 0; i--) if (bl_levels[i] < cur) { target = bl_levels[i]; break; } }
    mutex_unlock(&lock);
    if (target != cur) backlight_set(target);
}

static void step_thread(void *arg) { backlight_step((int)(intptr_t)arg); }

/* from acpi_notify: true when handled */
bool power_notify(struct aml_node *n, uint64_t value) {
    if (ns_hid_matches(n, "PNP0C0A") || ns_hid_matches(n, "ACPI0003")) { if (value == 0x80 || value == 0x81) { power_battery_changed(); return true; } }
    /* brightness keys: 0x86 up, 0x87 down, on the output device (or its parent video adapter) */
    if (value == 0x86 || value == 0x87) {   /* not here: the interpreter lock is held, and backlight_step takes ours first */
        thread_create(kernel_process, "backlight", step_thread, (void *)(intptr_t)(value == 0x86 ? 1 : -1));
        return true;
    }
    return false;
}
