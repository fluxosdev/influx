/*
 * Bochs/QEMU "standard VGA" and VirtualBox display driver: runtime mode
 * setting through the VBE DISPI interface (I/O ports 0x1CE/0x1CF).
 */
#include "gpu.h"
#include "arch.h"
#include "mm.h"
#include "string.h"
#include "io.h"
#include <limine.h>

extern volatile struct limine_framebuffer_request framebuffer_request;

#define DISPI_INDEX 0x1CE
#define DISPI_DATA  0x1CF
enum { DISPI_ID = 0, DISPI_XRES, DISPI_YRES, DISPI_BPP, DISPI_ENABLE, DISPI_BANK, DISPI_VIRT_WIDTH, DISPI_VIRT_HEIGHT, DISPI_X_OFFSET, DISPI_Y_OFFSET, DISPI_VIDEO_MEMORY_64K };
#define DISPI_ENABLED 0x01
#define DISPI_LFB_ENABLED 0x40
#define DISPI_NOCLEARMEM 0x80

static struct gpu_device bochs_gpu;
static uint64_t lfb_phys;

static void dispi_write(uint16_t idx, uint16_t v) { outw(DISPI_INDEX, idx); outw(DISPI_DATA, v); }
static uint16_t dispi_read(uint16_t idx) { outw(DISPI_INDEX, idx); return inw(DISPI_DATA); }

static bool bochs_set_mode(struct gpu_device *g, int w, int h) {
    if (w < 320 || h < 200 || w > 4096 || h > 4096) return false;
    if ((uint64_t)w * h * 4 > g->vram_bytes) return false;
    dispi_write(DISPI_ENABLE, 0);
    dispi_write(DISPI_XRES, w);
    dispi_write(DISPI_YRES, h);
    dispi_write(DISPI_BPP, 32);
    dispi_write(DISPI_VIRT_WIDTH, w);
    dispi_write(DISPI_VIRT_HEIGHT, h);
    dispi_write(DISPI_X_OFFSET, 0);
    dispi_write(DISPI_Y_OFFSET, 0);
    dispi_write(DISPI_ENABLE, DISPI_ENABLED | DISPI_LFB_ENABLED | DISPI_NOCLEARMEM);
    /* the device may adjust the geometry (widths are rounded to a multiple of 8); read back the truth */
    int rw = dispi_read(DISPI_XRES), rh = dispi_read(DISPI_YRES), vw = dispi_read(DISPI_VIRT_WIDTH);
    if (rw < 320 || rh < 200) return false;
    if (vw < rw) vw = rw;
    g->fb_phys = lfb_phys; g->fb_pitch = vw * 4; g->fb_width = rw; g->fb_height = rh;
    return true;
}

void bochs_init(void) {
    for (int i = 0; i < pci_device_count(); i++) {
        struct pci_device *pd = pci_device_at(i);
        bool qemu = pd->vendor == 0x1234 && pd->device == 0x1111;
        bool vbox = pd->vendor == 0x80EE && pd->device == 0xBEEF;
        if (!qemu && !vbox) continue;
        uint16_t id = dispi_read(DISPI_ID);
        if ((id & 0xFFF0) != 0xB0C0) { kprintf("bochs: DISPI not responding (id %04x)\n", id); return; }
        struct gpu_device *g = &bochs_gpu;
        snprintf(g->name, sizeof g->name, qemu ? "QEMU Standard VGA" : "VirtualBox VGA");
        strcpy(g->driver, "bochs"); snprintf(g->chip, sizeof g->chip, "DISPI %x", id & 0xF);
        snprintf(g->bus, sizeof g->bus, "PCI %02x:%02x.%d", pd->bus, pd->slot, pd->func);
        lfb_phys = pd->bar_addr[0];
        g->vram_base = lfb_phys; g->mmio_base = pd->bar_addr[2];
        g->vram_bytes = (uint64_t)dispi_read(DISPI_VIDEO_MEMORY_64K) << 16;
        if (!g->vram_bytes) g->vram_bytes = 16 << 20;
        g->modeset = true; g->set_mode = bochs_set_mode;
        struct limine_framebuffer *fb = framebuffer_request.response && framebuffer_request.response->framebuffer_count ? framebuffer_request.response->framebuffers[0] : NULL;
        uint64_t fb_phys = fb ? (uint64_t)fb->address - hhdm_offset : 0;
        g->scanout_verified = fb && fb_phys >= lfb_phys && fb_phys < lfb_phys + g->vram_bytes;
        if (fb) { g->fb_phys = fb_phys; g->fb_pitch = fb->pitch; g->fb_width = fb->width; g->fb_height = fb->height; }
        kprintf("bochs: %s, DISPI id %04x, %lu MiB VRAM, LFB at %lx%s\n", g->name, id, g->vram_bytes >> 20, lfb_phys, g->scanout_verified ? " (scanout)" : "");
        gpu_register(g);
        return;
    }
}
