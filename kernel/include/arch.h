#pragma once
#include "kernel.h"

/* Register frame pushed by the interrupt stubs (see isr.S) */
struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
} PACKED;

typedef void (*irq_handler_t)(struct regs *r, void *ctx);

#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_DS   (0x18 | 3)
#define USER_CS   (0x20 | 3)
#define TSS_SEL   0x28

#define IRQ_BASE      0x20
#define IRQ_TIMER     0
#define IRQ_KEYBOARD  1
#define IRQ_MOUSE     12
#define VEC_LAPIC_TIMER 0x40
#define VEC_SPURIOUS    0xFF

void gdt_init(void);
void tss_set_kernel_stack(uint64_t rsp);
void idt_init(void);
void idt_set_gate(uint8_t vec, void *handler, uint8_t ist, uint8_t dpl);
void irq_register(uint8_t vec, irq_handler_t h, void *ctx);
int irq_alloc_vector(void);        /* a free vector for a device interrupt, 0 if none left */
void cpu_init(void);
void cpu_get_brand(char *out, size_t n);
uint64_t cpu_tsc_hz(void);

/* ACPI */
void acpi_init(uint64_t rsdp);
void *acpi_find_table(const char *sig);
struct madt_info {
    uint64_t lapic_addr;
    uint64_t ioapic_addr;
    uint32_t ioapic_gsi_base;
    uint32_t iso_src[16];   /* legacy irq -> gsi */
    uint16_t iso_flags[16];
    int cpu_count;
};
extern struct madt_info madt;

/* APIC */
void lapic_init(void);
void lapic_eoi(void);
void lapic_timer_start(uint32_t hz);
uint32_t lapic_id(void);
void ioapic_init(void);
void ioapic_route_irq(uint8_t irq, uint8_t vector);
bool ioapic_route_gsi(uint32_t gsi, uint8_t vector, bool level, bool active_low);
void ioapic_mask_irq(uint8_t irq, bool mask);

/* Legacy timers / clocks */
void pit_init(void);
void pit_sleep_ms(uint32_t ms);    /* busy wait, early boot only */
void timer_init(void);             /* starts the scheduling tick */
struct rtc_time { int year, month, day, hour, minute, second; };
void rtc_read(struct rtc_time *t);
uint64_t rtc_to_unix(const struct rtc_time *t);
void unix_to_datetime(uint64_t t, struct rtc_time *out);
int day_of_week(int y, int m, int d);

void serial_init(void);
void serial_putc(char c);
void serial_write(const char *s, size_t n);

/* PCI */
struct pci_device {
    uint8_t bus, slot, func;
    uint16_t vendor, device;
    uint8_t class_code, subclass, prog_if, irq_line;
    uint8_t header_type, secondary_bus;   /* secondary bus: PCI-PCI bridges (header type 1) */
    uint32_t bar[6];
    uint64_t bar_addr[6];
    bool bar_is_mmio[6];
};
void pci_init(void);
int pci_device_count(void);
struct pci_device *pci_device_at(int i);
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v);
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint16_t v);
void pci_enable_bus_master(struct pci_device *d);
uint8_t pci_find_cap(struct pci_device *d, uint8_t id);   /* capability offset, 0 if absent */
/* Device interrupt: MSI when available and allowed, else INTx routed through
 * ACPI _PRT. The handler runs in interrupt context and must not send the EOI.
 * Returns false if neither works; `how` describes the result. */
bool pci_irq_install(struct pci_device *d, irq_handler_t h, void *ctx, bool allow_msi, char *how, size_t how_len);
const char *pci_class_name(uint8_t cls, uint8_t sub);
