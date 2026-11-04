#pragma once
#include <stdint.h>

static inline void outb(uint16_t port, uint8_t v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port)); }
static inline void outw(uint16_t port, uint16_t v) { __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(port)); }
static inline void outl(uint16_t port, uint32_t v) { __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"(port)); }
static inline uint8_t inb(uint16_t port) { uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline uint16_t inw(uint16_t port) { uint16_t v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline uint32_t inl(uint16_t port) { uint32_t v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void io_wait(void) { outb(0x80, 0); }
/* string I/O: one instruction for a whole buffer (a hypervisor handles it in a single exit) */
static inline void insw(uint16_t port, void *buf, uint64_t count) { __asm__ volatile("rep insw" : "+D"(buf), "+c"(count) : "d"(port) : "memory"); }
static inline void outsw(uint16_t port, const void *buf, uint64_t count) { __asm__ volatile("rep outsw" : "+S"(buf), "+c"(count) : "d"(port) : "memory"); }

static inline uint64_t rdmsr(uint32_t msr) { uint32_t lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr)); return ((uint64_t)hi << 32) | lo; }
static inline void wrmsr(uint32_t msr, uint64_t v) { __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32))); }
int rdmsr_safe(uint32_t msr, uint64_t *out);   /* 0, or -1 if the MSR faulted (#GP) */
int wrmsr_safe(uint32_t msr, uint64_t v);
static inline uint64_t rdtsc(void) { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return ((uint64_t)hi << 32) | lo; }

static inline void cli(void) { __asm__ volatile("cli"); }
static inline void sti(void) { __asm__ volatile("sti"); }
static inline void hlt(void) { __asm__ volatile("hlt"); }
static inline void pause(void) { __asm__ volatile("pause"); }
static inline uint64_t read_rflags(void) { uint64_t f; __asm__ volatile("pushfq; popq %0" : "=r"(f)); return f; }
static inline bool interrupts_enabled(void) { return (read_rflags() & 0x200) != 0; }
static inline uint64_t irq_save(void) { uint64_t f = read_rflags(); cli(); return f; }
static inline void irq_restore(uint64_t f) { if (f & 0x200) sti(); }

static inline uint64_t read_cr0(void) { uint64_t v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v; }
static inline void write_cr0(uint64_t v) { __asm__ volatile("mov %0, %%cr0" :: "r"(v) : "memory"); }
static inline uint64_t read_cr2(void) { uint64_t v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v; }
static inline uint64_t read_cr3(void) { uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline void write_cr3(uint64_t v) { __asm__ volatile("mov %0, %%cr3" :: "r"(v) : "memory"); }
static inline uint64_t read_cr4(void) { uint64_t v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v; }
static inline void write_cr4(uint64_t v) { __asm__ volatile("mov %0, %%cr4" :: "r"(v) : "memory"); }
static inline void invlpg(uint64_t addr) { __asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory"); }

static inline uint32_t mmio_read32(uint64_t addr) { return *(volatile uint32_t *)addr; }
static inline void mmio_write32(uint64_t addr, uint32_t v) { *(volatile uint32_t *)addr = v; }
static inline uint64_t mmio_read64(uint64_t addr) { return *(volatile uint64_t *)addr; }
static inline void mmio_write64(uint64_t addr, uint64_t v) { *(volatile uint64_t *)addr = v; }
