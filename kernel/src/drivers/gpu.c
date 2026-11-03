#include "gpu.h"
#include "arch.h"
#include "string.h"

static struct gpu_device *gpus[4];
static int ngpus;
static struct gpu_device software_gpu;

void gpu_register(struct gpu_device *g) { if (ngpus < 4) gpus[ngpus++] = g; }
int gpu_count(void) { return ngpus; }
struct gpu_device *gpu_at(int i) { return i >= 0 && i < ngpus ? gpus[i] : NULL; }
struct gpu_device *gpu_primary(void) {
    for (int i = 0; i < ngpus; i++) if (gpus[i]->scanout_verified) return gpus[i];
    for (int i = 0; i < ngpus; i++) if (strcmp(gpus[i]->driver, "software")) return gpus[i];
    return &software_gpu;
}

void gpu_init(void) {
    /* software fallback: whatever framebuffer the bootloader handed us, drawn by the CPU */
    strcpy(software_gpu.driver, "software");
    strcpy(software_gpu.chip, "CPU");
    bool found_vga = false;
    for (int i = 0; i < pci_device_count(); i++) {
        struct pci_device *d = pci_device_at(i);
        if (d->class_code == 0x03 && !found_vga) {
            found_vga = true;
            snprintf(software_gpu.name, sizeof software_gpu.name, "Software Rasterizer on %04x:%04x", d->vendor, d->device);
            snprintf(software_gpu.bus, sizeof software_gpu.bus, "PCI %02x:%02x.%d", d->bus, d->slot, d->func);
        }
    }
    if (!found_vga) strcpy(software_gpu.name, "Software Rasterizer (linear framebuffer)");
    radeon_init();
    intel_gfx_init();
    bochs_init();
    gpu_register(&software_gpu);
    for (int i = 0; i < ngpus; i++) kprintf("gpu: %s [%s] %s%s\n", gpus[i]->name, gpus[i]->driver, gpus[i]->vram_bytes ? "" : "", gpus[i]->scanout_verified ? " (scanout)" : "");
}

int gpu_list_modes(struct display_mode *out, int max) {
    static const struct display_mode common[] = { { 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 1280, 800 }, { 1366, 768 }, { 1440, 900 }, { 1600, 900 }, { 1680, 1050 }, { 1920, 1080 }, { 1920, 1200 }, { 2560, 1440 } };
    struct gpu_device *g = gpu_primary();
    if (g && g->list_modes) return g->list_modes(g, out, max);
    int n = 0;
    for (size_t i = 0; i < ARRAY_LEN(common) && n < max; i++) {
        if (g && g->modeset && (uint64_t)common[i].w * common[i].h * 4 > g->vram_bytes) continue;
        out[n++] = common[i];
    }
    return n;
}
