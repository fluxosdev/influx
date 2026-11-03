#include "arch.h"
#include "string.h"
#include "mm.h"
#include "io.h"

struct tss {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3, iopb;
} PACKED;

#define IST_SIZE 16384
/* one GDT and TSS per CPU: a TSS descriptor is marked busy once loaded */
static uint64_t gdt[MAX_CPUS][7];
static struct tss tss[MAX_CPUS] ALIGNED(16);
static uint8_t bsp_ist_stacks[3][IST_SIZE] ALIGNED(16);   /* IST1-3: #DF, NMI, #MC get a known-good stack */

static uint64_t make_desc(uint8_t access, uint8_t flags) {
    /* base = 0, limit = 0xFFFFF for flat segments */
    return 0xFFFFULL | ((uint64_t)access << 40) | ((uint64_t)(flags | 0xF) << 48);
}

void gdt_init(void) {
    int id = this_cpu()->id;
    uint64_t *g = gdt[id];
    struct tss *ts = &tss[id];
    g[0] = 0;
    g[1] = make_desc(0x9A, 0x20);   /* kernel code: present, ring0, exec/read, long mode */
    g[2] = make_desc(0x92, 0x00);   /* kernel data */
    g[3] = make_desc(0xF2, 0x00);   /* user data (0x18) */
    g[4] = make_desc(0xFA, 0x20);   /* user code (0x20) */
    /* TSS descriptor (16 bytes) at 0x28 */
    memset(ts, 0, sizeof *ts);
    ts->iopb = sizeof *ts;
    for (int i = 0; i < 3; i++) {
        uint8_t *st = id == 0 ? bsp_ist_stacks[i] : kmalloc(IST_SIZE);
        ts->ist[i] = ((uint64_t)st + IST_SIZE) & ~0xFULL;
    }
    uint64_t base = (uint64_t)ts, limit = sizeof *ts - 1;
    g[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40) |
           (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    g[6] = base >> 32;
    struct { uint16_t limit; uint64_t base; } PACKED gdtr = { sizeof gdt[id] - 1, (uint64_t)g };
    /* reloading GS zeroes its base: put the per-CPU pointer back afterwards */
    uint64_t gsbase = rdmsr(0xC0000101);
    __asm__ volatile(
        "lgdt %0\n"
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw $0x10, %%ax\n"
        "movw %%ax, %%ds\n movw %%ax, %%es\n movw %%ax, %%ss\n"
        "xorw %%ax, %%ax\n movw %%ax, %%fs\n movw %%ax, %%gs\n"
        "movw $0x28, %%ax\n ltr %%ax\n"
        :: "m"(gdtr) : "rax", "memory");
    wrmsr(0xC0000101, gsbase);
}

void tss_set_kernel_stack(uint64_t rsp) { tss[this_cpu()->id].rsp0 = rsp; }
