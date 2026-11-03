#include "arch.h"
#include "string.h"

struct tss {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3, iopb;
} PACKED;

static uint64_t gdt[7];
static struct tss tss ALIGNED(16);
static uint8_t ist_stacks[3][16384] ALIGNED(16);   /* IST1-3: #DF, NMI, #MC get a known-good stack */
static struct { uint16_t limit; uint64_t base; } PACKED gdtr;

static uint64_t make_desc(uint8_t access, uint8_t flags) {
    /* base = 0, limit = 0xFFFFF for flat segments */
    return 0xFFFFULL | ((uint64_t)access << 40) | ((uint64_t)(flags | 0xF) << 48);
}

void gdt_init(void) {
    gdt[0] = 0;
    gdt[1] = make_desc(0x9A, 0x20);   /* kernel code: present, ring0, exec/read, long mode */
    gdt[2] = make_desc(0x92, 0x00);   /* kernel data */
    gdt[3] = make_desc(0xF2, 0x00);   /* user data (0x18) */
    gdt[4] = make_desc(0xFA, 0x20);   /* user code (0x20) */
    /* TSS descriptor (16 bytes) at 0x28 */
    memset(&tss, 0, sizeof tss);
    tss.iopb = sizeof tss;
    for (int i = 0; i < 3; i++) tss.ist[i] = (uint64_t)(ist_stacks[i] + sizeof ist_stacks[i]);
    uint64_t base = (uint64_t)&tss, limit = sizeof tss - 1;
    gdt[5] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40) |
             (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    gdt[6] = base >> 32;
    gdtr.limit = sizeof gdt - 1;
    gdtr.base = (uint64_t)gdt;
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
}

void tss_set_kernel_stack(uint64_t rsp) { tss.rsp0 = rsp; }
