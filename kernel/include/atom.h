/*
 * AtomBIOS command-table interpreter.
 *
 * Ported from the Linux radeon driver (drivers/gpu/drm/radeon/atom.c,
 * atom.h, atom-bits.h):
 *
 * Copyright 2008 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Author: Stanislaw Skowronek
 *
 * Differences from Linux: every ROM access is bounds-checked (a table that
 * strays outside the image aborts instead of reading past it), call depth is
 * limited, parameter space is always at least 16 dwords, and the interpreter
 * has no dependencies beyond the callbacks below so it can be tested on the
 * host against real VBIOS images.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* register access and timing provided by the driver; register numbers are dword indices */
struct atom_card {
    void *priv;
    uint32_t (*reg_read)(void *priv, uint32_t reg);
    void (*reg_write)(void *priv, uint32_t reg, uint32_t v);
    uint32_t (*ioreg_read)(void *priv, uint32_t reg);
    void (*ioreg_write)(void *priv, uint32_t reg, uint32_t v);
    uint32_t (*mc_read)(void *priv, uint32_t reg);
    void (*mc_write)(void *priv, uint32_t reg, uint32_t v);
    uint32_t (*pll_read)(void *priv, uint32_t reg);
    void (*pll_write)(void *priv, uint32_t reg, uint32_t v);
    void (*delay_us)(void *priv, uint32_t us);
    uint64_t (*now_ms)(void *priv);
};

struct atom_ctx {
    const struct atom_card *card;
    const uint8_t *bios; uint32_t bios_len;
    uint32_t cmd_table, data_table;
    uint16_t iio[256];                 /* indirect I/O programs by port id */
    /* interpreter state shared across nested table calls */
    uint16_t data_block, reg_block, io_attr;
    uint32_t fb_base, divmul[2];
    uint8_t shift;
    int cs_equal, cs_above, io_mode;
    uint32_t *scratch; uint32_t scratch_bytes;   /* the "framebuffer" scratch area tables can use */
    int depth;
    bool fault;                        /* the current execution read outside the ROM or hit a bad opcode */
    uint64_t ops;                      /* executed opcodes (statistics) */
    uint64_t t0_ms;                    /* start of the current atom_execute, for its time budget */
    char name[64];                     /* bootup message */
};

bool atom_init(struct atom_ctx *ctx, const struct atom_card *card, const uint8_t *bios, uint32_t len);
void atom_fini(struct atom_ctx *ctx);
/* run command table 'index' with a parameter block of 'bytes' bytes (updated in place); 0 on success */
int atom_execute(struct atom_ctx *ctx, int index, void *params, size_t bytes);
bool atom_cmd_header(struct atom_ctx *ctx, int index, uint8_t *frev, uint8_t *crev);
bool atom_data_header(struct atom_ctx *ctx, int index, uint16_t *size, uint8_t *frev, uint8_t *crev, uint16_t *offset);
/* bounds-checked little-endian reads from the image (0 outside it) */
uint8_t atom_u8(const struct atom_ctx *ctx, uint32_t off);
uint16_t atom_u16(const struct atom_ctx *ctx, uint32_t off);
uint32_t atom_u32(const struct atom_ctx *ctx, uint32_t off);

/* indices into the master command table */
enum {
    ATOM_CMD_ASIC_Init = 0, ATOM_CMD_DIGxEncoderControl = 4, ATOM_CMD_EnableCRTCMemReq = 6, ATOM_CMD_SetPixelClock = 12,
    ATOM_CMD_AdjustDisplayPll = 17, ATOM_CMD_DAC_LoadDetection = 21, ATOM_CMD_LCD1OutputControl = 23,
    ATOM_CMD_DAC1EncoderControl = 24, ATOM_CMD_DAC2EncoderControl = 25, ATOM_CMD_DVOOutputControl = 26,
    ATOM_CMD_TVEncoderControl = 29, ATOM_CMD_EnableScaler = 33, ATOM_CMD_BlankCRTC = 34, ATOM_CMD_EnableCRTC = 35,
    ATOM_CMD_SetCRTC_Timing = 39, ATOM_CMD_SetCRTC_OverScan = 40, ATOM_CMD_SelectCRTC_Source = 42,
    ATOM_CMD_UpdateCRTC_DoubleBufferRegisters = 44, ATOM_CMD_SetCRTC_UsingDTDTiming = 49, ATOM_CMD_LVTMAOutputControl = 51,
    ATOM_CMD_EnableSpreadSpectrumOnPPLL = 65, ATOM_CMD_TMDSAOutputControl = 66, ATOM_CMD_DAC1OutputControl = 68,
    ATOM_CMD_DAC2OutputControl = 69, ATOM_CMD_DIG1EncoderControl = 74, ATOM_CMD_DIG2EncoderControl = 75,
    ATOM_CMD_EnableYUV = 73, ATOM_CMD_UNIPHYTransmitterControl = 76, ATOM_CMD_LVTMATransmitterControl = 77,
};
/* indices into the master data table */
enum {
    ATOM_DATA_FirmwareInfo = 4, ATOM_DATA_GPIO_I2C_Info = 10, ATOM_DATA_VRAM_UsageByFirmware = 11,
    ATOM_DATA_PPLL_SS_Info = 18, ATOM_DATA_Object_Header = 22, ATOM_DATA_IndirectIOAccess = 23,
    ATOM_DATA_IntegratedSystemInfo = 30,
};
