#pragma once
#include "kernel.h"

/* display modes offered by Settings */
struct display_mode { int w, h; };

/* A display adapter known to the kernel. The software device is always
 * present; a hardware driver can add page flipping and a hardware cursor. */
struct gpu_device {
    char name[64];            /* marketing name, e.g. "ATI Radeon HD 4870" */
    char driver[24];          /* "radeon", "software" */
    char chip[16];            /* "RV770" */
    char bus[16];             /* "PCI 01:00.0" */
    uint64_t vram_bytes;      /* 0 if unknown */
    uint64_t mmio_base, vram_base;
    char bios_message[96];    /* AtomBIOS bootup message */
    char bios_part[32];       /* "113-..." part number */
    uint32_t engine_clock_khz, memory_clock_khz;
    int stream_processors;
    bool scanout_verified;    /* the bootloader framebuffer lives in this GPU's VRAM */
    bool hw_cursor, page_flip, accelerated, modeset;
    uint64_t fb_phys; int fb_pitch, fb_width, fb_height;   /* current scanout after set_mode */
    bool (*set_mode)(struct gpu_device *g, int w, int h);
    int (*list_modes)(struct gpu_device *g, struct display_mode *out, int max);   /* optional: the monitor's modes */
    /* hardware cursor (64x64 ARGB, straight alpha) */
    bool (*set_cursor)(struct gpu_device *g, const uint32_t *argb, int w, int h, int hot_x, int hot_y);
    void (*move_cursor)(struct gpu_device *g, int x, int y, bool visible);
    /* double-buffered scanout: returns the CPU pointer to the back buffer; flip() makes it visible */
    uint8_t *(*back_buffer)(struct gpu_device *g, int *pitch);
    void (*flip)(struct gpu_device *g);
    void *priv;
};

void gpu_init(void);                       /* registers the software device and probes drivers */
void gpu_register(struct gpu_device *g);
int gpu_count(void);
struct gpu_device *gpu_at(int i);
struct gpu_device *gpu_primary(void);      /* the device driving the framebuffer */
void radeon_init(void);
void bochs_init(void);

int gpu_list_modes(struct display_mode *out, int max);
