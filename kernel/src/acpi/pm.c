/*
 * ACPI power management. acpi_namespace_init() loads the DSDT and SSDTs,
 * switches the chipset into ACPI mode, runs _REG, _STA/_INI and _PIC, and
 * starts the SCI: the interrupt handler masks whatever fired (fixed power
 * button, GPEs) and a worker thread runs the AML for it (_Lxx/_Exx, EC
 * queries) before unmasking. Power-off goes through \_PTS and \_S5, reboot
 * through the FADT reset register.
 */
#include "acpi_internal.h"
#include "power.h"
#include "io.h"
#include "mm.h"
#include "string.h"

#define PM1_PWRBTN   (1u << 8)
#define PM1_SLPBTN   (1u << 9)
#define PM1_WAK_STS  (1u << 15)
#define PM1_SCI_EN   (1u << 0)
#define PM1_SLP_TYP  (7u << 10)
#define PM1_SLP_EN   (1u << 13)

#define MAX_GPE_BYTES 64          /* status bytes per block: 512 GPEs */

static struct acpi_fadt *fadt;
static bool ready, hw_reduced, sci_running;
static struct acpi_gas pm1a_sts, pm1b_sts, pm1a_en, pm1b_en, pm1a_cnt, pm1b_cnt;
static struct acpi_gas gpe_blk[2];
static int gpe_bytes[2], gpe_first[2];      /* status bytes per block, number of its first GPE */
static uint8_t gpe_edge[2][MAX_GPE_BYTES], gpe_enabled[2][MAX_GPE_BYTES];
static volatile uint8_t gpe_pending[2][MAX_GPE_BYTES];
static volatile uint32_t pending_fixed;
static spinlock_t sci_lock = SPINLOCK_INIT;   /* pending_*, gpe_enabled and the GPE registers: SCI handler vs worker */
static struct aml_node **gpe_method;        /* indexed by GPE number */
static int gpe_count, gpe_runtime;
static uint32_t sci_gsi;
static struct semaphore sci_sem;
static void (*power_button_fn)(void);
static int nssdt;

bool acpi_ready(void) { return ready; }
void acpi_set_power_button_handler(void (*fn)(void)) { power_button_fn = fn; }

uint64_t acpi_timer_100ns(void) {
    uint64_t hz = cpu_tsc_hz();
    return hz ? (uint64_t)((__uint128_t)rdtsc() * 10000000 / hz) : time_ms() * 10000;
}
void acpi_stall_us(uint64_t us) {
    uint64_t hz = cpu_tsc_hz();
    if (!hz) { for (uint64_t i = 0; i < us; i++) io_wait(); return; }
    uint64_t end = rdtsc() + us * (hz / 1000000);
    while (rdtsc() < end) pause();
}

/* ---- fixed registers ---- */
static struct acpi_gas fadt_reg(const struct acpi_gas *x, size_t xoff, uint32_t port, int bytes) {
    struct acpi_gas g = { SPACE_IO, bytes * 8, 0, 0, port };
    if (fadt->len >= xoff + sizeof(struct acpi_gas) && x->address) { g = *x; g.access_size = 0; if (!g.bit_width) g.bit_width = bytes * 8; }
    return g;
}
#define FADT_REG(name, legacy, bytes) fadt_reg(&fadt->name, offsetof(struct acpi_fadt, name), fadt->legacy, bytes)

static void setup_registers(void) {
    int half = fadt->pm1_evt_len / 2; if (!half) half = 2;
    pm1a_sts = FADT_REG(x_pm1a_evt, pm1a_evt, fadt->pm1_evt_len);
    pm1b_sts = FADT_REG(x_pm1b_evt, pm1b_evt, fadt->pm1_evt_len);
    pm1a_sts.bit_width = pm1b_sts.bit_width = half * 8;
    pm1a_en = pm1a_sts; pm1a_en.address += pm1a_sts.address ? half : 0;
    pm1b_en = pm1b_sts; pm1b_en.address += pm1b_sts.address ? half : 0;
    pm1a_cnt = FADT_REG(x_pm1a_cnt, pm1a_cnt, fadt->pm1_cnt_len ? fadt->pm1_cnt_len : 2);
    pm1b_cnt = FADT_REG(x_pm1b_cnt, pm1b_cnt, fadt->pm1_cnt_len ? fadt->pm1_cnt_len : 2);
    gpe_blk[0] = FADT_REG(x_gpe0, gpe0, 1); gpe_bytes[0] = MIN(fadt->gpe0_len / 2, MAX_GPE_BYTES); gpe_first[0] = 0;
    gpe_blk[1] = FADT_REG(x_gpe1, gpe1, 1); gpe_bytes[1] = MIN(fadt->gpe1_len / 2, MAX_GPE_BYTES); gpe_first[1] = fadt->gpe1_base;
    for (int b = 0; b < 2; b++) if (!gpe_blk[b].address) gpe_bytes[b] = 0;
}

static uint32_t pm1_read(bool enable) {
    return gas_read(enable ? &pm1a_en : &pm1a_sts) | gas_read(enable ? &pm1b_en : &pm1b_sts);
}
static void pm1_write(bool enable, uint32_t v) {
    gas_write(enable ? &pm1a_en : &pm1a_sts, v);
    gas_write(enable ? &pm1b_en : &pm1b_sts, v);
}

static uint8_t gpe_read(int b, int i, bool enable) {
    struct acpi_gas g = gpe_blk[b]; g.address += i + (enable ? gpe_bytes[b] : 0); g.bit_width = 8;
    return gas_read(&g);
}
static void gpe_write(int b, int i, bool enable, uint8_t v) {
    struct acpi_gas g = gpe_blk[b]; g.address += i + (enable ? gpe_bytes[b] : 0); g.bit_width = 8;
    gas_write(&g, v);
}
static bool gpe_locate(int gpe, int *b, int *i) {
    for (*b = 0; *b < 2; (*b)++) {
        int rel = gpe - gpe_first[*b];
        if (gpe_bytes[*b] && rel >= 0 && rel < gpe_bytes[*b] * 8) { *i = rel / 8; return true; }
    }
    return false;
}

/* ---- FACS global lock: AML (Acquire(\_GL), fields with LockRule) vs. SMM firmware ---- */
#define PM1_GBL_RLS (1u << 2)
static volatile uint32_t *gl_word;
static bool gl_probed;
static int gl_depth;

static volatile uint32_t *facs_lock(void) {
    if (gl_probed) return gl_word;
    gl_probed = true;
    if (!fadt || hw_reduced) return NULL;
    uint64_t phys = fadt->len >= offsetof(struct acpi_fadt, x_firmware_ctrl) + 8 && fadt->x_firmware_ctrl ? fadt->x_firmware_ctrl : fadt->firmware_ctrl;
    if (!phys) return NULL;
    uint8_t *facs = acpi_map_phys(phys, 64);
    if (memcmp(facs, "FACS", 4) || *(uint32_t *)(facs + 4) < 24) { kprintf("acpi: bad FACS, no global lock\n"); return NULL; }
    return gl_word = (volatile uint32_t *)(facs + 16);
}

/* spec 5.2.10.1: set owned; if it already was, set pending and wait for the
 * firmware to release (spinning here instead of waiting for GBL_STS) */
bool acpi_gl_acquire(uint32_t timeout_ms) {
    volatile uint32_t *w = facs_lock();
    if (!w) return true;
    if (gl_depth) { gl_depth++; return true; }
    for (uint64_t spins = 0;; spins++) {
        uint32_t old, new;
        do { old = *w; new = (old & ~1u) | 2 | ((old >> 1) & 1); } while (!__sync_bool_compare_and_swap(w, old, new));
        if (!(old & 2)) { gl_depth = 1; return true; }
        if (spins >= (uint64_t)timeout_ms * 20) break;
        acpi_stall_us(50);
    }
    static bool warned;
    if (!warned) { warned = true; kprintf("acpi: firmware holds the global lock, timed out\n"); }
    return false;
}

void acpi_gl_release(void) {
    if (!gl_depth || --gl_depth) return;
    volatile uint32_t *w = facs_lock();
    if (!w) return;
    uint32_t old;
    do old = *w; while (!__sync_bool_compare_and_swap(w, old, old & ~3u));
    if (old & 1) {   /* firmware is waiting for it */
        gas_write(&pm1a_cnt, gas_read(&pm1a_cnt) | PM1_GBL_RLS);
        if (pm1b_cnt.address) gas_write(&pm1b_cnt, gas_read(&pm1b_cnt) | PM1_GBL_RLS);
    }
}

/* ---- bring-up ---- */
static void enable_acpi_mode(void) {
    if (hw_reduced) return;
    if (gas_read(&pm1a_cnt) & PM1_SCI_EN) { kprintf("acpi: already in ACPI mode\n"); return; }
    if (!fadt->smi_cmd || !fadt->acpi_enable) { kprintf("acpi: no way to enter ACPI mode\n"); return; }
    outb(fadt->smi_cmd, fadt->acpi_enable);
    for (int i = 0; i < 300; i++) {
        if (gas_read(&pm1a_cnt) & PM1_SCI_EN) { kprintf("acpi: entered ACPI mode\n"); return; }
        thread_sleep_ms(10);
    }
    kprintf("acpi: firmware did not enter ACPI mode\n");
}

static bool container(struct aml_obj *o) {
    return o && (o->type == AML_DEVICE || o->type == AML_PROCESSOR || o->type == AML_THERMAL || o->type == AML_POWER);
}

/* _REG(space, 1) for the address spaces that always work (as ACPICA does) */
static bool reg_cb(struct aml_node *n, int depth, void *ctx) {
    if (n->obj && n->obj->type == AML_METHOD && !memcmp(n->name, "_REG", 4)) {
        uint32_t spaces = 0;
        for (struct aml_node *c = n->parent->children; c; c = c->next)
            if (c->obj && c->obj->type == AML_REGION && c->obj->region.space < 32) spaces |= 1u << c->obj->region.space;
        static const int always[] = { SPACE_MEMORY, SPACE_IO, SPACE_PCI };
        for (size_t i = 0; i < ARRAY_LEN(always); i++) {
            if (!(spaces & (1u << always[i]))) continue;
            struct aml_obj *args[2] = { obj_int(always[i]), obj_int(1) };
            aml_eval(n, args, 2, NULL);
            obj_put(args[0]); obj_put(args[1]);
        }
        return false;
    }
    return container(n->obj);
}

static int ini_count;
static bool ini_cb(struct aml_node *n, int depth, void *ctx) {
    if (!container(n->obj)) return false;
    uint64_t sta = 0xF;
    if (ns_child(n, "_STA") && aml_eval_int(n, "_STA", &sta) < 0) sta = 0xF;
    if (!(sta & 1)) return (sta & 8) != 0;   /* absent but functional: children may still exist */
    if (n != ctx && ns_child(n, "_INI")) { aml_eval_path(n, "_INI", NULL, 0, NULL); ini_count++; }
    return true;
}

static void call_pic(int mode) {
    if (!ns_lookup(aml_root, "\\_PIC")) return;
    struct aml_obj *arg = obj_int(mode);
    aml_eval_path(aml_root, "\\_PIC", &arg, 1, NULL);
    obj_put(arg);
}

/* ---- SCI ---- */
static void sci_irq(struct regs *r, void *ctx) {
    bool any = false;
    spin_lock(&sci_lock);
    uint32_t hit = pm1_read(false) & pm1_read(true) & (PM1_PWRBTN | PM1_SLPBTN);
    if (hit) { pm1_write(false, hit); pending_fixed |= hit; any = true; }
    for (int b = 0; b < 2; b++)
        for (int i = 0; i < gpe_bytes[b]; i++) {
            uint8_t s = gpe_read(b, i, false) & gpe_enabled[b][i];
            if (!s) continue;
            gpe_enabled[b][i] &= ~s;   /* masked until the worker has run the handler */
            gpe_write(b, i, true, gpe_enabled[b][i]);
            if (s & gpe_edge[b][i]) gpe_write(b, i, false, s & gpe_edge[b][i]);
            gpe_pending[b][i] |= s;
            any = true;
        }
    spin_unlock(&sci_lock);
    if (any) sem_post(&sci_sem);   /* the shared dispatcher in arch/pci.c sends the EOI */
}

static void power_button_pressed(void) {
    kprintf("acpi: power button pressed\n");
    if (power_button_fn) power_button_fn();
}

/* Notify() from AML: a control-method power button reports 0x80 */
void acpi_notify(struct aml_node *n, uint64_t value) {
    if (value == 0x80 && ns_hid_matches(n, "PNP0C0C")) { power_button_pressed(); return; }
    if (power_notify(n, value)) return;
    char path[96]; ns_path(n, path, sizeof path);
    kprintf("acpi: Notify(%s, 0x%lx)\n", path, value);
}

static bool run_gpe(int gpe) {
    if (gpe == ec_gpe()) ec_handle_events();
    if (gpe < gpe_count && gpe_method[gpe] && aml_eval(gpe_method[gpe], NULL, 0, NULL) < 0) {
        kprintf("acpi: GPE %d handler failed, leaving it disabled\n", gpe);   /* a failing level GPE would fire forever */
        return false;
    }
    return true;
}

static void acpi_worker(void *arg) {
    for (;;) {
        sem_wait(&sci_sem);
        spin_lock(&sci_lock);
        uint32_t fixed = pending_fixed; pending_fixed = 0;
        spin_unlock(&sci_lock);
        if (fixed & PM1_PWRBTN) power_button_pressed();
        if (fixed & PM1_SLPBTN) kprintf("acpi: sleep button pressed (sleep states are not supported)\n");
        for (int b = 0; b < 2; b++)
            for (int i = 0; i < gpe_bytes[b]; i++) {
                spin_lock(&sci_lock);
                uint8_t s = gpe_pending[b][i]; gpe_pending[b][i] = 0;
                spin_unlock(&sci_lock);
                for (int bit = 0; bit < 8; bit++) {
                    if (!(s & (1 << bit))) continue;
                    bool ok = run_gpe(gpe_first[b] + i * 8 + bit);
                    spin_lock(&sci_lock);
                    if (!(gpe_edge[b][i] & (1 << bit))) gpe_write(b, i, false, 1 << bit);   /* level: clear after the handler */
                    if (ok) gpe_enabled[b][i] |= 1 << bit;
                    gpe_write(b, i, true, gpe_enabled[b][i]);
                    spin_unlock(&sci_lock);
                }
            }
    }
}

static bool prw_cb(struct aml_node *n, int depth, void *ctx) {
    bool *wake = ctx;
    if (!container(n->obj)) return false;
    struct aml_obj *prw;
    if (ns_child(n, "_PRW") && aml_eval_path(n, "_PRW", NULL, 0, &prw) == 0) {
        if (prw->type == AML_PACKAGE && prw->pkg.n >= 1) {
            struct aml_obj *e = aml_value_of(prw->pkg.e[0]);
            if (e->type == AML_INTEGER && e->ival < (uint64_t)gpe_count) wake[e->ival] = true;
            obj_put(e);
        }
        obj_put(prw);
    }
    return true;
}

static void sci_init(void) {
    if (hw_reduced || !fadt->sci_int) return;
    /* GPEs: mask and clear everything, then enable the runtime ones that have handlers */
    for (int b = 0; b < 2; b++) gpe_count = MAX(gpe_count, gpe_first[b] + gpe_bytes[b] * 8);
    gpe_method = kzalloc((gpe_count + 1) * sizeof *gpe_method);
    bool *wake = kzalloc(gpe_count + 1);
    for (int b = 0; b < 2; b++)
        for (int i = 0; i < gpe_bytes[b]; i++) { gpe_write(b, i, true, 0); gpe_write(b, i, false, 0xFF); }
    ns_walk(ns_lookup(aml_root, "\\_SB_"), prw_cb, wake);
    struct aml_node *gpe_scope = ns_lookup(aml_root, "\\_GPE");
    for (int g = 0; g < gpe_count && g < 256; g++) {
        char name[8]; struct aml_node *m = NULL; bool edge = false;
        snprintf(name, sizeof name, "_L%02X", g);
        if (gpe_scope && !(m = ns_child(gpe_scope, name))) { snprintf(name, sizeof name, "_E%02X", g); m = ns_child(gpe_scope, name); edge = m != NULL; }
        bool is_ec = g == ec_gpe();
        if (is_ec && !m) edge = true;
        if ((!m || wake[g]) && !is_ec) continue;
        int b, i;
        if (!gpe_locate(g, &b, &i)) continue;
        gpe_method[g] = m;
        if (edge) gpe_edge[b][i] |= 1 << ((g - gpe_first[b]) % 8);
        gpe_enabled[b][i] |= 1 << ((g - gpe_first[b]) % 8);
        gpe_runtime++;
    }
    kfree(wake);
    sem_init(&sci_sem, 0);
    thread_create(kernel_process, "acpi", acpi_worker, NULL);

    /* fixed events: only the power button */
    pm1_write(false, 0xFFFF);
    pm1_write(true, (fadt->flags & FADT_PWR_BUTTON) ? 0 : PM1_PWRBTN);

    /* the SCI is an ISA IRQ, shareable, level triggered and active low unless the MADT overrides it */
    uint32_t irq = fadt->sci_int;
    uint16_t fl = 0;
    sci_gsi = irq;
    if (irq < 16) { sci_gsi = madt.iso_src[irq]; fl = madt.iso_flags[irq]; }
    bool active_low = (fl & 3) != 1, level = ((fl >> 2) & 3) != 1;
    /* PCI INTx lines may land on the same GSI: share one vector and dispatcher */
    if (!pci_irq_share_gsi(sci_gsi, level, active_low, sci_irq, NULL)) { kprintf("acpi: cannot route SCI GSI %u\n", sci_gsi); return; }
    for (int b = 0; b < 2; b++) for (int i = 0; i < gpe_bytes[b]; i++) gpe_write(b, i, true, gpe_enabled[b][i]);
    sci_running = true;
    kprintf("acpi: SCI on GSI %u (%s, active %s), %d runtime GPEs\n", sci_gsi, level ? "level" : "edge", active_low ? "low" : "high", gpe_runtime);
}

void acpi_namespace_init(void) {
    fadt = acpi_fadt();
    if (!fadt) { kprintf("acpi: no FADT, AML disabled\n"); return; }
    hw_reduced = fadt->len >= offsetof(struct acpi_fadt, flags) + 4 && (fadt->flags & FADT_HW_REDUCED);
    uint64_t dsdt = fadt->len >= offsetof(struct acpi_fadt, x_dsdt) + 8 && fadt->x_dsdt ? fadt->x_dsdt : fadt->dsdt;
    setup_registers();   /* before any AML: releasing the global lock writes PM1 control */
    aml_init();
    uint8_t rev = 0;
    if (dsdt) {
        uint8_t *h = acpi_map_phys(dsdt, 36);
        uint32_t len = *(uint32_t *)(h + 4);
        rev = h[8];
        aml_int64 = rev >= 2;
        if (len >= 36 && len <= ACPI_MAX_TABLE) aml_load_table(acpi_map_phys(dsdt, len), len, "DSDT");
        else kprintf("acpi: DSDT has bad length %u\n", len);
    }
    char sig[5];
    for (int i = 0; acpi_table_at(i, sig); i++) {
        if (memcmp(sig, "SSDT", 4) && memcmp(sig, "PSDT", 4)) continue;
        uint8_t *t = acpi_table_at(i, sig);   /* length already checked when mapped */
        aml_load_table(t, *(uint32_t *)(t + 4), "SSDT");
        nssdt++;
    }
    ready = true;
    kprintf("acpi: namespace: %d objects from the DSDT (revision %u, %d-bit integers) and %d SSDTs, %d AML errors\n",
            ns_node_count(), rev, aml_int64 ? 64 : 32, nssdt, aml_errors);
    enable_acpi_mode();
    ns_walk(aml_root, reg_cb, NULL);
    ec_init();
    struct aml_node *sb = ns_lookup(aml_root, "\\_SB_");
    if (sb && ns_child(sb, "_INI")) aml_eval_path(sb, "_INI", NULL, 0, NULL);
    ns_walk(aml_root, ini_cb, sb);
    kprintf("acpi: ran %d _INI methods\n", ini_count);
    call_pic(1);   /* interrupt model: I/O APIC; _PRT now returns APIC routing */
    sci_init();
}

/* ---- power off / reset ---- */
void acpi_shutdown(void) {
    if (!ready) return;
    struct aml_obj *s5;
    if (aml_eval_path(aml_root, "\\_S5", NULL, 0, &s5) < 0 || s5->type != AML_PACKAGE || !s5->pkg.n) {
        kprintf("acpi: no \\_S5 package, cannot power off\n");
        obj_put(s5);
        return;
    }
    uint64_t typa = 0, typb = 0;
    aml_lock();
    struct aml_obj *a = aml_value_of(s5->pkg.e[0]); obj_to_int(a, &typa); obj_put(a);
    if (s5->pkg.n > 1) { struct aml_obj *b = aml_value_of(s5->pkg.e[1]); obj_to_int(b, &typb); obj_put(b); }
    else typb = typa >> 8;
    aml_unlock();
    obj_put(s5);
    typa &= 7; typb &= 7;
    if (ns_lookup(aml_root, "\\_PTS")) { struct aml_obj *arg = obj_int(5); aml_eval_path(aml_root, "\\_PTS", &arg, 1, NULL); obj_put(arg); }
    kprintf("acpi: entering S5 (SLP_TYP %lu/%lu)\n", typa, typb);
    uint64_t fl = irq_save();
    if (hw_reduced) gas_write(&fadt->sleep_control, (typa << 2) | (1 << 5));
    else {
        for (int b = 0; b < 2; b++) for (int i = 0; i < gpe_bytes[b]; i++) gpe_write(b, i, true, 0);
        pm1_write(false, PM1_WAK_STS);
        uint64_t ca = gas_read(&pm1a_cnt) & ~(PM1_SLP_TYP | PM1_SLP_EN), cb = gas_read(&pm1b_cnt) & ~(PM1_SLP_TYP | PM1_SLP_EN);
        gas_write(&pm1a_cnt, ca | (typa << 10)); gas_write(&pm1b_cnt, cb | (typb << 10));
        gas_write(&pm1a_cnt, ca | (typa << 10) | PM1_SLP_EN); gas_write(&pm1b_cnt, cb | (typb << 10) | PM1_SLP_EN);
    }
    acpi_stall_us(500000);
    irq_restore(fl);
    kprintf("acpi: still running after S5\n");
}

void acpi_reboot(void) {
    if (!fadt || fadt->len < offsetof(struct acpi_fadt, reset_value) + 1 || !(fadt->flags & FADT_RESET_REG_SUP)) return;
    struct acpi_gas *g = &fadt->reset_reg;
    kprintf("acpi: reset register (space %d, 0x%lx) <- 0x%x\n", g->space, g->address, fadt->reset_value);
    if (g->space == SPACE_PCI) {   /* bus 0; device, function, offset packed into the address */
        uint8_t dev = (g->address >> 32) & 0x1F, fn = (g->address >> 16) & 7, off = g->address & 0xFF;
        pci_write8(0, dev, fn, off, fadt->reset_value);
    } else {
        struct acpi_gas r = *g; r.bit_width = 8; r.access_size = 0;
        gas_write(&r, fadt->reset_value);
    }
    acpi_stall_us(500000);
}

/* ---- shell ---- */
static bool sleep_state(int n) { char p[8]; snprintf(p, sizeof p, "\\_S%d", n); return ns_lookup(aml_root, p) != NULL; }

void acpi_print_info(acpi_print_fn out, void *ctx) {
    char line[160];
    char sig[5] = { 0 };
    int len = snprintf(line, sizeof line, "Tables:");
    for (int i = 0; acpi_table_at(i, sig) && len < (int)sizeof line - 6; i++) if (sig[0]) len += snprintf(line + len, sizeof line - len, " %.4s", sig);
    out(ctx, line);
    if (!ready) { out(ctx, "AML namespace: not loaded"); return; }
    snprintf(line, sizeof line, "Namespace: %d objects, DSDT + %d SSDTs, %d-bit integers, %d AML errors", ns_node_count(), nssdt, aml_int64 ? 64 : 32, aml_errors);
    out(ctx, line);
    snprintf(line, sizeof line, "Mode: %s%s", hw_reduced ? "hardware-reduced" : (gas_read(&pm1a_cnt) & PM1_SCI_EN) ? "ACPI (SCI_EN set)" : "legacy", "");
    out(ctx, line);
    if (sci_running) { snprintf(line, sizeof line, "SCI: IRQ %u -> GSI %u, %d runtime GPEs", fadt->sci_int, sci_gsi, gpe_runtime); out(ctx, line); }
    snprintf(line, sizeof line, "Power button: %s", (fadt->flags & FADT_PWR_BUTTON) ? "control method device" : "fixed hardware");
    out(ctx, line);
    len = snprintf(line, sizeof line, "Sleep states:");
    for (int i = 0; i <= 5; i++) if (sleep_state(i)) len += snprintf(line + len, sizeof line - len, " S%d", i);
    out(ctx, line);
    snprintf(line, sizeof line, "Reset register: %s", (fadt->flags & FADT_RESET_REG_SUP) && fadt->reset_reg.address ? "yes" : "no");
    out(ctx, line);
    const char *ec = ec_describe();
    snprintf(line, sizeof line, "Embedded controller: %s", ec ? ec : "none");
    out(ctx, line);
}
