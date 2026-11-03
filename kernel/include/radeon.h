#pragma once
/* ATI Radeon R700 driver internals shared by radeon.c (probe, scanout, cursor)
 * and radeon_kms.c (AtomBIOS mode setting). */
#include "gpu.h"
#include "arch.h"

struct radeon_kms;

struct radeon {
    struct gpu_device gpu;
    struct pci_device *pd;
    volatile uint32_t *mmio; uint64_t mmio_phys, mmio_len;
    uint16_t io_port;                    /* PCI I/O BAR (AtomBIOS indirect I/O), 0 if none */
    uint8_t *vram; uint64_t vram_phys, aper_len;
    uint64_t fb_start_gpu;               /* MC address of VRAM start */
    uint32_t scan_gpu_addr, pitch_px, width, height;
    uint64_t scan_off;                   /* VRAM offset of the bootloader framebuffer */
    uint64_t buf_off[2]; int back;       /* page-flip buffers (VRAM offsets); back = index being drawn */
    uint64_t cursor_off;
    uint32_t cursor_img[64 * 64];        /* premultiplied copy, rewritten when VRAM is laid out anew */
    int cursor_hot_x, cursor_hot_y; bool cursor_valid;
    uint8_t *rom; size_t rom_len;        /* copy of the video BIOS */
    bool is_rs880, dce32, igp_vram_unknown;
    int crtc;                            /* which CRTC scans out the framebuffer */
    struct radeon_kms *kms;              /* AtomBIOS mode setting, NULL if unavailable */
};

#define CRTC2_OFFSET 0x0800
uint32_t radeon_rreg(struct radeon *r, uint32_t reg);
void radeon_wreg(struct radeon *r, uint32_t reg, uint32_t v);
void radeon_udelay(uint32_t us);
uint32_t radeon_crtc_reg(struct radeon *r, uint32_t reg);
/* program the primary surface address of the scanout CRTC */
void radeon_set_surface_address(struct radeon *r, uint64_t gpu_addr);
/* rewrite the cursor image at cursor_off and its registers */
void radeon_cursor_upload(struct radeon *r);

/* set up mode setting for the output the firmware lit; enables gpu.modeset on success */
void radeon_kms_init(struct radeon *r);
