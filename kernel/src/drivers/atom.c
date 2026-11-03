/*
 * AtomBIOS command-table interpreter, ported from the Linux radeon driver.
 * Copyright 2008 Advanced Micro Devices, Inc. (MIT licence, see atom.h).
 * Author: Stanislaw Skowronek
 */
#include "atom.h"
#include "atom_port.h"

#define ATOM_BIOS_MAGIC     0xAA55
#define ATOM_ATI_MAGIC_PTR  0x30
#define ATOM_ATI_MAGIC      " 761295520"
#define ATOM_ROM_TABLE_PTR  0x48
#define ATOM_ROM_MAGIC_PTR  4
#define ATOM_ROM_MSG_PTR    0x10
#define ATOM_ROM_CMD_PTR    0x1E
#define ATOM_ROM_DATA_PTR   0x20
#define ATOM_DATA_IIO_PTR   0x32

#define ATOM_CT_SIZE_PTR    0
#define ATOM_CT_WS_PTR      4
#define ATOM_CT_PS_PTR      5
#define ATOM_CT_PS_MASK     0x7F
#define ATOM_CT_CODE_PTR    6

#define ATOM_OP_CNT         123
#define ATOM_OP_EOT         91
#define ATOM_CASE_MAGIC     0x63
#define ATOM_CASE_END       0x5A5A

enum { ARG_REG, ARG_PS, ARG_WS, ARG_FB, ARG_ID, ARG_IMM, ARG_PLL, ARG_MC };
enum { SRC_DWORD, SRC_WORD0, SRC_WORD8, SRC_WORD16, SRC_BYTE0, SRC_BYTE8, SRC_BYTE16, SRC_BYTE24 };
#define WS_QUOTIENT   0x40
#define WS_REMAINDER  0x41
#define WS_DATAPTR    0x42
#define WS_SHIFT      0x43
#define WS_OR_MASK    0x44
#define WS_AND_MASK   0x45
#define WS_FB_WINDOW  0x46
#define WS_ATTRIBUTES 0x47
#define WS_REGPTR     0x48
enum { IIO_NOP, IIO_START, IIO_READ, IIO_WRITE, IIO_CLEAR, IIO_SET, IIO_MOVE_INDEX, IIO_MOVE_ATTR, IIO_MOVE_DATA, IIO_END };
#define IO_MM    0
#define IO_PCI   1
#define IO_SYSIO 2
#define IO_IIO   0x80
enum { COND_ABOVE, COND_ABOVEOREQUAL, COND_ALWAYS, COND_BELOW, COND_BELOWOREQUAL, COND_EQUAL, COND_NOTEQUAL };
enum { PORT_ATI, PORT_PCI, PORT_SYSIO };
enum { UNIT_MICROSEC, UNIT_MILLISEC };

#define MAX_DEPTH 32
#define MAX_EXEC_MS 5000
#define MIN_PS_DWORDS 16

struct exec {
    struct atom_ctx *ctx;
    uint32_t *ps, *ws;
    int ps_size, ws_size, ps_shift;
    uint32_t start;
    uint32_t last_jump; uint64_t last_jump_ms;
    bool abort;
};

static const uint32_t arg_mask[8] = { 0xFFFFFFFF, 0x0000FFFF, 0x00FFFF00, 0xFFFF0000, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000 };
static const int arg_shift[8] = { 0, 0, 8, 16, 0, 8, 16, 24 };
/* destination alignment field -> source alignment encoding */
static const int dst_to_src[8][4] = { { 0, 0, 0, 0 }, { 1, 2, 3, 0 }, { 1, 2, 3, 0 }, { 1, 2, 3, 0 }, { 4, 5, 6, 7 }, { 4, 5, 6, 7 }, { 4, 5, 6, 7 }, { 4, 5, 6, 7 } };
static const int def_dst[8] = { 0, 0, 1, 2, 0, 1, 2, 3 };

/* ---- ROM access ---- */
uint8_t atom_u8(const struct atom_ctx *ctx, uint32_t off) { return off < ctx->bios_len ? ctx->bios[off] : 0; }
uint16_t atom_u16(const struct atom_ctx *ctx, uint32_t off) { return (uint16_t)(atom_u8(ctx, off) | atom_u8(ctx, off + 1) << 8); }
uint32_t atom_u32(const struct atom_ctx *ctx, uint32_t off) { return atom_u16(ctx, off) | (uint32_t)atom_u16(ctx, off + 2) << 16; }
/* reads made while executing: running off the image is a fault */
static uint8_t U8(struct exec *e, uint32_t off) { if (off >= e->ctx->bios_len) { e->ctx->fault = true; e->abort = true; return 0; } return e->ctx->bios[off]; }
static uint16_t U16(struct exec *e, uint32_t off) { return (uint16_t)(U8(e, off) | U8(e, off + 1) << 8); }
static uint32_t U32(struct exec *e, uint32_t off) { return U16(e, off) | (uint32_t)U16(e, off + 2) << 16; }

static uint32_t iio_execute(struct atom_ctx *ctx, uint32_t base, uint32_t index, uint32_t data) {
    uint32_t temp = 0xCDCDCDCD;
    for (int guard = 0; guard < 4096; guard++) {
        uint8_t op = atom_u8(ctx, base);
        #define WIDTH_MASK(n) (0xFFFFFFFFu >> (32 - (n)))
        switch (op) {
        case IIO_NOP: base++; break;
        case IIO_READ: temp = ctx->card->ioreg_read(ctx->card->priv, atom_u16(ctx, base + 1)); base += 3; break;
        case IIO_WRITE: ctx->card->ioreg_write(ctx->card->priv, atom_u16(ctx, base + 1), temp); base += 3; break;
        case IIO_CLEAR: temp &= ~(WIDTH_MASK(atom_u8(ctx, base + 1)) << atom_u8(ctx, base + 2)); base += 3; break;
        case IIO_SET: temp |= WIDTH_MASK(atom_u8(ctx, base + 1)) << atom_u8(ctx, base + 2); base += 3; break;
        case IIO_MOVE_INDEX: case IIO_MOVE_DATA: case IIO_MOVE_ATTR: {
            uint32_t src = op == IIO_MOVE_INDEX ? index : op == IIO_MOVE_DATA ? data : ctx->io_attr;
            uint32_t m = WIDTH_MASK(atom_u8(ctx, base + 1));
            temp &= ~(m << atom_u8(ctx, base + 3));
            temp |= ((src >> atom_u8(ctx, base + 2)) & m) << atom_u8(ctx, base + 3);
            base += 4; break; }
        case IIO_END: return temp;
        default: ATOM_LOG("unknown IIO opcode %u\n", op); ctx->fault = true; return 0;
        }
        #undef WIDTH_MASK
    }
    ctx->fault = true;
    return 0;
}

static uint32_t get_src_int(struct exec *e, uint8_t attr, uint32_t *ptr, uint32_t *saved) {
    struct atom_ctx *c = e->ctx;
    uint32_t idx, val = 0xCDCDCDCD, align = (attr >> 3) & 7, arg = attr & 7;
    switch (arg) {
    case ARG_REG:
        idx = U16(e, *ptr); *ptr += 2;
        idx += c->reg_block;
        switch (c->io_mode) {
        case IO_MM: val = c->card->reg_read(c->card->priv, idx); break;
        case IO_PCI: ATOM_LOG("PCI registers are not implemented\n"); return 0;
        case IO_SYSIO: ATOM_LOG("SYSIO registers are not implemented\n"); return 0;
        default:
            if (!(c->io_mode & 0x80)) { ATOM_LOG("bad IO mode\n"); return 0; }
            if (!c->iio[c->io_mode & 0x7F]) { ATOM_LOG("undefined indirect IO read method %d\n", c->io_mode & 0x7F); return 0; }
            val = iio_execute(c, c->iio[c->io_mode & 0x7F], idx, 0);
        }
        break;
    case ARG_PS:
        idx = U8(e, *ptr); (*ptr)++;
        if ((int)idx < e->ps_size) val = e->ps[idx]; else ATOM_LOG("PS index out of range: %u > %d\n", idx, e->ps_size);
        break;
    case ARG_WS:
        idx = U8(e, *ptr); (*ptr)++;
        switch (idx) {
        case WS_QUOTIENT: val = c->divmul[0]; break;
        case WS_REMAINDER: val = c->divmul[1]; break;
        case WS_DATAPTR: val = c->data_block; break;
        case WS_SHIFT: val = c->shift; break;
        case WS_OR_MASK: val = 1u << c->shift; break;
        case WS_AND_MASK: val = ~(1u << c->shift); break;
        case WS_FB_WINDOW: val = c->fb_base; break;
        case WS_ATTRIBUTES: val = c->io_attr; break;
        case WS_REGPTR: val = c->reg_block; break;
        default: if ((int)idx < e->ws_size) val = e->ws[idx]; else ATOM_LOG("WS index out of range: %u > %d\n", idx, e->ws_size);
        }
        break;
    case ARG_ID:
        idx = U16(e, *ptr); *ptr += 2;
        /* data beyond the image reads as zero (Linux reads whatever follows its copy); only code fetches fault */
        if ((uint64_t)idx + c->data_block + 4 > c->bios_len) { ATOM_LOG("data read beyond the image at %x\n", idx + c->data_block); val = 0; }
        else val = atom_u32(c, idx + c->data_block);
        break;
    case ARG_FB:
        idx = U8(e, *ptr); (*ptr)++;
        if ((uint64_t)c->fb_base + idx * 4 + 4 > c->scratch_bytes) { ATOM_LOG("fb read beyond scratch region: %u vs. %u\n", c->fb_base + idx * 4, c->scratch_bytes); val = 0; }
        else val = c->scratch[c->fb_base / 4 + idx];
        break;
    case ARG_IMM:
        switch (align) {
        case SRC_DWORD: val = U32(e, *ptr); *ptr += 4; return val;
        case SRC_WORD0: case SRC_WORD8: case SRC_WORD16: val = U16(e, *ptr); *ptr += 2; return val;
        default: val = U8(e, *ptr); (*ptr)++; return val;
        }
    case ARG_PLL:
        idx = U8(e, *ptr); (*ptr)++;
        val = c->card->pll_read(c->card->priv, idx);
        break;
    case ARG_MC:
        idx = U8(e, *ptr); (*ptr)++;
        val = c->card->mc_read(c->card->priv, idx);
        break;
    }
    if (saved) *saved = val;
    val &= arg_mask[align];
    val >>= arg_shift[align];
    return val;
}
static void skip_src_int(struct exec *e, uint8_t attr, uint32_t *ptr) {
    uint32_t align = (attr >> 3) & 7, arg = attr & 7;
    switch (arg) {
    case ARG_REG: case ARG_ID: *ptr += 2; break;
    case ARG_PLL: case ARG_MC: case ARG_PS: case ARG_WS: case ARG_FB: (*ptr)++; break;
    case ARG_IMM:
        if (align == SRC_DWORD) *ptr += 4;
        else if (align <= SRC_WORD16) *ptr += 2;
        else (*ptr)++;
        break;
    }
}
static uint32_t get_src(struct exec *e, uint8_t attr, uint32_t *ptr) { return get_src_int(e, attr, ptr, 0); }
static uint32_t get_src_direct(struct exec *e, uint8_t align, uint32_t *ptr) {
    uint32_t val = 0xCDCDCDCD;
    if (align == SRC_DWORD) { val = U32(e, *ptr); *ptr += 4; }
    else if (align <= SRC_WORD16) { val = U16(e, *ptr); *ptr += 2; }
    else { val = U8(e, *ptr); (*ptr)++; }
    return val;
}
static uint32_t get_dst(struct exec *e, int arg, uint8_t attr, uint32_t *ptr, uint32_t *saved) {
    return get_src_int(e, (uint8_t)(arg | dst_to_src[(attr >> 3) & 7][(attr >> 6) & 3] << 3), ptr, saved);
}
static void skip_dst(struct exec *e, int arg, uint8_t attr, uint32_t *ptr) {
    skip_src_int(e, (uint8_t)(arg | dst_to_src[(attr >> 3) & 7][(attr >> 6) & 3] << 3), ptr);
}
static void put_dst(struct exec *e, int arg, uint8_t attr, uint32_t *ptr, uint32_t val, uint32_t saved) {
    struct atom_ctx *c = e->ctx;
    uint32_t align = dst_to_src[(attr >> 3) & 7][(attr >> 6) & 3], idx;
    val <<= arg_shift[align];
    val &= arg_mask[align];
    saved &= ~arg_mask[align];
    val |= saved;
    switch (arg) {
    case ARG_REG:
        idx = U16(e, *ptr); *ptr += 2;
        idx += c->reg_block;
        switch (c->io_mode) {
        case IO_MM: c->card->reg_write(c->card->priv, idx, idx == 0 ? val << 2 : val); break;   /* MM_INDEX takes a byte address */
        case IO_PCI: ATOM_LOG("PCI registers are not implemented\n"); return;
        case IO_SYSIO: ATOM_LOG("SYSIO registers are not implemented\n"); return;
        default:
            if (!(c->io_mode & 0x80)) { ATOM_LOG("bad IO mode\n"); return; }
            /* as in Linux, writes index the IIO programs with the 0x80 bit kept */
            if (!c->iio[c->io_mode & 0xFF]) { ATOM_LOG("undefined indirect IO write method %d\n", c->io_mode & 0x7F); return; }
            iio_execute(c, c->iio[c->io_mode & 0xFF], idx, val);
        }
        break;
    case ARG_PS:
        idx = U8(e, *ptr); (*ptr)++;
        if ((int)idx >= e->ps_size) { ATOM_LOG("PS index out of range: %u > %d\n", idx, e->ps_size); return; }
        e->ps[idx] = val;
        break;
    case ARG_WS:
        idx = U8(e, *ptr); (*ptr)++;
        switch (idx) {
        case WS_QUOTIENT: c->divmul[0] = val; break;
        case WS_REMAINDER: c->divmul[1] = val; break;
        case WS_DATAPTR: c->data_block = (uint16_t)val; break;
        case WS_SHIFT: c->shift = (uint8_t)val; break;
        case WS_OR_MASK: case WS_AND_MASK: break;
        case WS_FB_WINDOW: c->fb_base = val; break;
        case WS_ATTRIBUTES: c->io_attr = (uint16_t)val; break;
        case WS_REGPTR: c->reg_block = (uint16_t)val; break;
        default:
            if ((int)idx >= e->ws_size) { ATOM_LOG("WS index out of range: %u > %d\n", idx, e->ws_size); return; }
            e->ws[idx] = val;
        }
        break;
    case ARG_FB:
        idx = U8(e, *ptr); (*ptr)++;
        if ((uint64_t)c->fb_base + idx * 4 + 4 > c->scratch_bytes) ATOM_LOG("fb write beyond scratch region: %u vs. %u\n", c->fb_base + idx * 4, c->scratch_bytes);
        else c->scratch[c->fb_base / 4 + idx] = val;
        break;
    case ARG_PLL:
        idx = U8(e, *ptr); (*ptr)++;
        c->card->pll_write(c->card->priv, idx, val);
        break;
    case ARG_MC:
        idx = U8(e, *ptr); (*ptr)++;
        c->card->mc_write(c->card->priv, idx, val);
        break;
    }
}

/* ---- opcodes ---- */
static int execute_table(struct atom_ctx *ctx, int index, uint32_t *params, int params_size);

/* the read-modify-write ALU operations share their operand decoding */
enum { ALU_ADD, ALU_AND, ALU_OR, ALU_SUB, ALU_XOR };
static void op_alu(struct exec *e, uint32_t *ptr, int arg, int kind) {
    uint8_t attr = U8(e, (*ptr)++);
    uint32_t saved, dptr = *ptr;
    uint32_t dst = get_dst(e, arg, attr, ptr, &saved);
    uint32_t src = get_src(e, attr, ptr);
    switch (kind) {
    case ALU_ADD: dst += src; break;
    case ALU_AND: dst &= src; break;
    case ALU_OR: dst |= src; break;
    case ALU_SUB: dst -= src; break;
    case ALU_XOR: dst ^= src; break;
    }
    put_dst(e, arg, attr, &dptr, dst, saved);
}
static void op_add(struct exec *e, uint32_t *ptr, int arg) { op_alu(e, ptr, arg, ALU_ADD); }
static void op_and(struct exec *e, uint32_t *ptr, int arg) { op_alu(e, ptr, arg, ALU_AND); }
static void op_or(struct exec *e, uint32_t *ptr, int arg) { op_alu(e, ptr, arg, ALU_OR); }
static void op_sub(struct exec *e, uint32_t *ptr, int arg) { op_alu(e, ptr, arg, ALU_SUB); }
static void op_xor(struct exec *e, uint32_t *ptr, int arg) { op_alu(e, ptr, arg, ALU_XOR); }
static void op_beep(struct exec *e, uint32_t *ptr, int arg) { ATOM_LOG("ATOM BIOS beeped!\n"); }
static void op_calltable(struct exec *e, uint32_t *ptr, int arg) {
    int idx = U8(e, (*ptr)++);
    if (atom_u16(e->ctx, e->ctx->cmd_table + 4 + 2 * idx) && execute_table(e->ctx, idx, e->ps + e->ps_shift, e->ps_size - e->ps_shift)) e->abort = true;
}
static void op_clear(struct exec *e, uint32_t *ptr, int arg) {
    uint8_t attr = U8(e, (*ptr)++);
    uint32_t saved, dptr = *ptr;
    attr &= 0x38;
    attr |= def_dst[attr >> 3] << 6;
    get_dst(e, arg, attr, ptr, &saved);
    put_dst(e, arg, attr, &dptr, 0, saved);
}
static void op_compare(struct exec *e, uint32_t *ptr, int arg) {
    uint8_t attr = U8(e, (*ptr)++);
    uint32_t dst = get_dst(e, arg, attr, ptr, 0), src = get_src(e, attr, ptr);
    e->ctx->cs_equal = dst == src;
    e->ctx->cs_above = dst > src;
}
static void op_delay(struct exec *e, uint32_t *ptr, int arg) {
    unsigned count = U8(e, (*ptr)++);
    e->ctx->card->delay_us(e->ctx->card->priv, arg == UNIT_MICROSEC ? count : count * 1000);
}
static void op_div(struct exec *e, uint32_t *ptr, int arg) {
    uint8_t attr = U8(e, (*ptr)++);
    uint32_t dst = get_dst(e, arg, attr, ptr, 0), src = get_src(e, attr, ptr);
    e->ctx->divmul[0] = src ? dst / src : 0;
    e->ctx->divmul[1] = src ? dst % src : 0;
}
static void op_eot(struct exec *e, uint32_t *ptr, int arg) {}
static void op_jump(struct exec *e, uint32_t *ptr, int arg) {
    struct atom_ctx *c = e->ctx;
    int execute = 0;
    uint32_t target = U16(e, *ptr);
    *ptr += 2;
    switch (arg) {
    case COND_ABOVE: execute = c->cs_above; break;
    case COND_ABOVEOREQUAL: execute = c->cs_above || c->cs_equal; break;
    case COND_ALWAYS: execute = 1; break;
    case COND_BELOW: execute = !(c->cs_above || c->cs_equal); break;
    case COND_BELOWOREQUAL: execute = !c->cs_above; break;
    case COND_EQUAL: execute = c->cs_equal; break;
    case COND_NOTEQUAL: execute = !c->cs_equal; break;
    }
    if (!execute) return;
    /* a table spinning on the same jump for 5 s (polling a register that never changes) is aborted */
    uint64_t now = c->card->now_ms(c->card->priv);
    if (e->last_jump == e->start + target) {
        if (now - e->last_jump_ms > 5000) { ATOM_LOG("table stuck in a loop for more than 5 s, aborting\n"); e->abort = true; }
    } else { e->last_jump = e->start + target; e->last_jump_ms = now; }
    *ptr = e->start + target;
}
static void op_mask(struct exec *e, uint32_t *ptr, int arg) {
    uint8_t attr = U8(e, (*ptr)++);
    uint32_t saved, dptr = *ptr;
    uint32_t dst = get_dst(e, arg, attr, ptr, &saved);
    uint32_t mask = get_src_direct(e, (attr >> 3) & 7, ptr);
    uint32_t src = get_src(e, attr, ptr);
    dst &= mask;
    dst |= src;
    put_dst(e, arg, attr, &dptr, dst, saved);
}
static void op_move(struct exec *e, uint32_t *ptr, int arg) {
    uint8_t attr = U8(e, (*ptr)++);
    uint32_t saved, dptr = *ptr;
    if (((attr >> 3) & 7) != SRC_DWORD) get_dst(e, arg, attr, ptr, &saved);
    else { skip_dst(e, arg, attr, ptr); saved = 0xCDCDCDCD; }
    uint32_t src = get_src(e, attr, ptr);
    put_dst(e, arg, attr, &dptr, src, saved);
}
static void op_mul(struct exec *e, uint32_t *ptr, int arg) {
    uint8_t attr = U8(e, (*ptr)++);
    uint32_t dst = get_dst(e, arg, attr, ptr, 0), src = get_src(e, attr, ptr);
    e->ctx->divmul[0] = dst * src;
}
static void op_nop(struct exec *e, uint32_t *ptr, int arg) {}
static void op_postcard(struct exec *e, uint32_t *ptr, int arg) { (*ptr)++; }
static void op_unimplemented(struct exec *e, uint32_t *ptr, int arg) { ATOM_LOG("unimplemented opcode\n"); e->ctx->fault = true; e->abort = true; }
static void op_debug(struct exec *e, uint32_t *ptr, int arg) { (*ptr)++; }
static void op_processds(struct exec *e, uint32_t *ptr, int arg) { *ptr += U16(e, *ptr) + 2u; }
static void op_setdatablock(struct exec *e, uint32_t *ptr, int arg) {
    int idx = U8(e, (*ptr)++);
    if (!idx) e->ctx->data_block = 0;
    else if (idx == 255) e->ctx->data_block = (uint16_t)e->start;
    else e->ctx->data_block = U16(e, e->ctx->data_table + 4 + 2 * idx);
}
static void op_setfbbase(struct exec *e, uint32_t *ptr, int arg) {
    uint8_t attr = U8(e, (*ptr)++);
    e->ctx->fb_base = get_src(e, attr, ptr);
}
static void op_setport(struct exec *e, uint32_t *ptr, int arg) {
    switch (arg) {
    case PORT_ATI: { int port = U16(e, *ptr); e->ctx->io_mode = port ? IO_IIO | port : IO_MM; *ptr += 2; break; }
    case PORT_PCI: e->ctx->io_mode = IO_PCI; (*ptr)++; break;
    case PORT_SYSIO: e->ctx->io_mode = IO_SYSIO; (*ptr)++; break;
    }
}
static void op_setregblock(struct exec *e, uint32_t *ptr, int arg) { e->ctx->reg_block = U16(e, *ptr); *ptr += 2; }
static void op_shift_left(struct exec *e, uint32_t *ptr, int arg) {
    uint8_t attr = U8(e, (*ptr)++);
    uint32_t saved, dptr = *ptr;
    attr &= 0x38;
    attr |= def_dst[attr >> 3] << 6;
    uint32_t dst = get_dst(e, arg, attr, ptr, &saved);
    uint8_t shift = (uint8_t)get_src_direct(e, SRC_BYTE0, ptr);
    put_dst(e, arg, attr, &dptr, dst << shift, saved);
}
static void op_shift_right(struct exec *e, uint32_t *ptr, int arg) {
    uint8_t attr = U8(e, (*ptr)++);
    uint32_t saved, dptr = *ptr;
    attr &= 0x38;
    attr |= def_dst[attr >> 3] << 6;
    uint32_t dst = get_dst(e, arg, attr, ptr, &saved);
    uint8_t shift = (uint8_t)get_src_direct(e, SRC_BYTE0, ptr);
    put_dst(e, arg, attr, &dptr, dst >> shift, saved);
}
static void op_shl(struct exec *e, uint32_t *ptr, int arg) {
    uint8_t attr = U8(e, (*ptr)++);
    uint32_t saved, dptr = *ptr;
    uint32_t dst_align = dst_to_src[(attr >> 3) & 7][(attr >> 6) & 3];
    get_dst(e, arg, attr, ptr, &saved);
    uint32_t dst = saved;   /* the shift needs the full value */
    uint8_t shift = (uint8_t)get_src(e, attr, ptr);
    dst <<= shift;
    dst &= arg_mask[dst_align];
    dst >>= arg_shift[dst_align];
    put_dst(e, arg, attr, &dptr, dst, saved);
}
static void op_shr(struct exec *e, uint32_t *ptr, int arg) {
    uint8_t attr = U8(e, (*ptr)++);
    uint32_t saved, dptr = *ptr;
    uint32_t dst_align = dst_to_src[(attr >> 3) & 7][(attr >> 6) & 3];
    get_dst(e, arg, attr, ptr, &saved);
    uint32_t dst = saved;
    uint8_t shift = (uint8_t)get_src(e, attr, ptr);
    dst >>= shift;
    dst &= arg_mask[dst_align];
    dst >>= arg_shift[dst_align];
    put_dst(e, arg, attr, &dptr, dst, saved);
}
static void op_switch(struct exec *e, uint32_t *ptr, int arg) {
    uint8_t attr = U8(e, (*ptr)++);
    uint32_t src = get_src(e, attr, ptr);
    while (!e->abort && U16(e, *ptr) != ATOM_CASE_END) {
        if (U8(e, *ptr) != ATOM_CASE_MAGIC) { ATOM_LOG("bad case\n"); e->abort = true; e->ctx->fault = true; return; }
        (*ptr)++;
        uint32_t val = get_src(e, (attr & 0x38) | ARG_IMM, ptr);
        uint32_t target = U16(e, *ptr);
        if (val == src) { *ptr = e->start + target; return; }
        *ptr += 2;
    }
    *ptr += 2;
}
static void op_test(struct exec *e, uint32_t *ptr, int arg) {
    uint8_t attr = U8(e, (*ptr)++);
    uint32_t dst = get_dst(e, arg, attr, ptr, 0), src = get_src(e, attr, ptr);
    e->ctx->cs_equal = (dst & src) == 0;
}

typedef void (*op_fn)(struct exec *, uint32_t *, int);
#define SIX(fn) { fn, ARG_REG }, { fn, ARG_PS }, { fn, ARG_WS }, { fn, ARG_FB }, { fn, ARG_PLL }, { fn, ARG_MC }
static const struct { op_fn fn; int arg; } opcodes[ATOM_OP_CNT] = {
    { 0, 0 },
    SIX(op_move), SIX(op_and), SIX(op_or), SIX(op_shift_left), SIX(op_shift_right), SIX(op_mul), SIX(op_div), SIX(op_add), SIX(op_sub),
    { op_setport, PORT_ATI }, { op_setport, PORT_PCI }, { op_setport, PORT_SYSIO }, { op_setregblock, 0 }, { op_setfbbase, 0 },
    SIX(op_compare),
    { op_switch, 0 },
    { op_jump, COND_ALWAYS }, { op_jump, COND_EQUAL }, { op_jump, COND_BELOW }, { op_jump, COND_ABOVE },
    { op_jump, COND_BELOWOREQUAL }, { op_jump, COND_ABOVEOREQUAL }, { op_jump, COND_NOTEQUAL },
    SIX(op_test),
    { op_delay, UNIT_MILLISEC }, { op_delay, UNIT_MICROSEC }, { op_calltable, 0 }, { op_unimplemented, 0 } /* repeat */,
    SIX(op_clear),
    { op_nop, 0 }, { op_eot, 0 },
    SIX(op_mask),
    { op_postcard, 0 }, { op_beep, 0 }, { op_unimplemented, 0 } /* savereg */, { op_unimplemented, 0 } /* restorereg */, { op_setdatablock, 0 },
    SIX(op_xor), SIX(op_shl), SIX(op_shr),
    { op_debug, 0 }, { op_processds, 0 },
};
#undef SIX

static int execute_table(struct atom_ctx *ctx, int index, uint32_t *params, int params_size) {
    uint32_t base = atom_u16(ctx, ctx->cmd_table + 4 + 2 * index);
    if (!base || base + ATOM_CT_CODE_PTR > ctx->bios_len) return -1;
    if (ctx->depth >= MAX_DEPTH) { ATOM_LOG("table call depth exceeded\n"); return -1; }
    int ws = atom_u8(ctx, base + ATOM_CT_WS_PTR), ps = atom_u8(ctx, base + ATOM_CT_PS_PTR) & ATOM_CT_PS_MASK;
    struct exec e = { .ctx = ctx, .ps = params, .ps_size = params_size, .ps_shift = ps / 4, .start = base };
    if (ws) {
        e.ws = ATOM_MALLOC((size_t)ws * 4);
        if (!e.ws) return -1;
        ATOM_MEMSET(e.ws, 0, (size_t)ws * 4);
        e.ws_size = ws;
    }
    ctx->depth++;
    uint32_t ptr = base + ATOM_CT_CODE_PTR;
    int ret = 0;
    for (;;) {
        uint8_t op = U8(&e, ptr++);
        if (e.abort) { ATOM_LOG("aborted table %d at %04x (base %04x)\n", index, ptr - 1, base); ret = -1; break; }
        if (op == 0 || op >= ATOM_OP_CNT || !opcodes[op].fn) { ATOM_LOG("bad opcode %u in table %d at %04x\n", op, index, ptr - 1); ctx->fault = true; ret = -1; break; }
        if (!(++ctx->ops & 15) && ctx->card->now_ms(ctx->card->priv) - ctx->t0_ms > MAX_EXEC_MS) { ATOM_LOG("table %d ran for more than %d ms, aborting\n", index, MAX_EXEC_MS); ret = -1; break; }
        opcodes[op].fn(&e, &ptr, opcodes[op].arg);
        if (op == ATOM_OP_EOT) break;
    }
    if (e.abort && !ret) ret = -1;
    ctx->depth--;
    if (e.ws) ATOM_FREE(e.ws);
    return ret;
}

int atom_execute(struct atom_ctx *ctx, int index, void *params, size_t bytes) {
    /* tables may read parameter dwords beyond the documented structure (and nested calls shift
     * the window), so give them at least MIN_PS_DWORDS of zeroed space */
    uint32_t ps[64];
    size_t dwords = (bytes + 3) / 4;
    if (dwords > 64) return -1;
    ATOM_MEMSET(ps, 0, sizeof ps);
    if (bytes) ATOM_MEMCPY(ps, params, bytes);
    ctx->data_block = 0; ctx->reg_block = 0; ctx->fb_base = 0; ctx->io_mode = IO_MM;
    ctx->divmul[0] = ctx->divmul[1] = 0;
    ctx->fault = false; ctx->depth = 0;
    ctx->t0_ms = ctx->card->now_ms(ctx->card->priv);
    int r = execute_table(ctx, index, ps, (int)(dwords > MIN_PS_DWORDS ? dwords : MIN_PS_DWORDS));
    if (bytes) ATOM_MEMCPY(params, ps, bytes);
    return r || ctx->fault ? -1 : 0;
}

static const int iio_len[] = { 1, 2, 3, 3, 3, 3, 4, 4, 4, 3 };
static void index_iio(struct atom_ctx *ctx, uint32_t base) {
    ATOM_MEMSET(ctx->iio, 0, sizeof ctx->iio);
    while (base < ctx->bios_len && atom_u8(ctx, base) == IIO_START) {
        ctx->iio[atom_u8(ctx, base + 1)] = (uint16_t)(base + 2);
        base += 2;
        while (base < ctx->bios_len && atom_u8(ctx, base) != IIO_END) {
            uint8_t op = atom_u8(ctx, base);
            if (op >= sizeof iio_len / sizeof *iio_len) return;   /* malformed: stop indexing */
            base += iio_len[op];
        }
        base += 3;
    }
}

bool atom_init(struct atom_ctx *ctx, const struct atom_card *card, const uint8_t *bios, uint32_t len) {
    ATOM_MEMSET(ctx, 0, sizeof *ctx);
    ctx->card = card; ctx->bios = bios; ctx->bios_len = len;
    if (atom_u16(ctx, 0) != ATOM_BIOS_MAGIC) { ATOM_LOG("invalid BIOS magic\n"); return false; }
    for (int i = 0; ATOM_ATI_MAGIC[i]; i++) if (atom_u8(ctx, ATOM_ATI_MAGIC_PTR + i) != (uint8_t)ATOM_ATI_MAGIC[i]) { ATOM_LOG("invalid ATI magic\n"); return false; }
    uint32_t base = atom_u16(ctx, ATOM_ROM_TABLE_PTR);
    if (atom_u8(ctx, base + 4) != 'A' || atom_u8(ctx, base + 5) != 'T' || atom_u8(ctx, base + 6) != 'O' || atom_u8(ctx, base + 7) != 'M') { ATOM_LOG("invalid ATOM magic\n"); return false; }
    ctx->cmd_table = atom_u16(ctx, base + ATOM_ROM_CMD_PTR);
    ctx->data_table = atom_u16(ctx, base + ATOM_ROM_DATA_PTR);
    index_iio(ctx, atom_u16(ctx, ctx->data_table + ATOM_DATA_IIO_PTR) + 4);
    /* bootup message: the name string isn't always terminated */
    uint32_t s = atom_u16(ctx, base + ATOM_ROM_MSG_PTR);
    while (atom_u8(ctx, s) == '\n' || atom_u8(ctx, s) == '\r') s++;
    int i;
    for (i = 0; i < (int)sizeof ctx->name - 1; i++) { uint8_t ch = atom_u8(ctx, s + i); if (ch < '.' || ch > 'z') break; ctx->name[i] = (char)ch; }
    ctx->name[i] = 0;
    /* scratch space the tables may use as "framebuffer" memory */
    uint16_t off;
    uint32_t usage = 0;
    if (atom_data_header(ctx, ATOM_DATA_VRAM_UsageByFirmware, 0, 0, 0, &off)) usage = (uint32_t)atom_u16(ctx, off + 4 + 4) * 1024;
    if (!usage) usage = 20 * 1024;
    ctx->scratch = ATOM_MALLOC(usage);
    if (!ctx->scratch) return false;
    ATOM_MEMSET(ctx->scratch, 0, usage);
    ctx->scratch_bytes = usage;
    return true;
}
void atom_fini(struct atom_ctx *ctx) { if (ctx->scratch) ATOM_FREE(ctx->scratch); ctx->scratch = 0; }

bool atom_data_header(struct atom_ctx *ctx, int index, uint16_t *size, uint8_t *frev, uint8_t *crev, uint16_t *offset) {
    uint16_t idx = atom_u16(ctx, ctx->data_table + 4 + 2 * index);
    if (!idx || idx + 4u > ctx->bios_len) return false;
    if (size) *size = atom_u16(ctx, idx);
    if (frev) *frev = atom_u8(ctx, idx + 2);
    if (crev) *crev = atom_u8(ctx, idx + 3);
    *offset = idx;
    return true;
}
bool atom_cmd_header(struct atom_ctx *ctx, int index, uint8_t *frev, uint8_t *crev) {
    uint16_t idx = atom_u16(ctx, ctx->cmd_table + 4 + 2 * index);
    if (!idx || idx + 4u > ctx->bios_len) return false;
    if (frev) *frev = atom_u8(ctx, idx + 2);
    if (crev) *crev = atom_u8(ctx, idx + 3);
    return true;
}
