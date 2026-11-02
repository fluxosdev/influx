#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

#define PAGE_SIZE 4096ULL
#define PAGE_ALIGN_DOWN(x) ((uint64_t)(x) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_UP(x)   (((uint64_t)(x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define ALIGN_UP(x, a)     (((uint64_t)(x) + ((a) - 1)) & ~((uint64_t)(a) - 1))
#define ALIGN_DOWN(x, a)   ((uint64_t)(x) & ~((uint64_t)(a) - 1))
#define ARRAY_LEN(a)       (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b)          ((a) < (b) ? (a) : (b))
#define MAX(a, b)          ((a) > (b) ? (a) : (b))
#define CLAMP(x, lo, hi)   ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
#define UNUSED(x)          (void)(x)
#define PACKED             __attribute__((packed))
#define ALIGNED(n)         __attribute__((aligned(n)))
#define NORETURN           __attribute__((noreturn))
#define likely(x)          __builtin_expect(!!(x), 1)
#define unlikely(x)        __builtin_expect(!!(x), 0)

#define KERNEL_VIRT_BASE 0xffffffff80000000ULL
#define USER_STACK_TOP   0x00007ffffffff000ULL
#define USER_STACK_SIZE  (256 * 1024)
#define USER_HEAP_BASE   0x0000100000000000ULL

#define INFLUX_VERSION "0.1.0"

extern uint64_t hhdm_offset;
static inline void *P2V(uint64_t p) { return (void *)(p + hhdm_offset); }
static inline uint64_t V2P(const void *v) { return (uint64_t)v - hhdm_offset; }

void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void kvprintf(const char *fmt, va_list ap);
void NORETURN panic(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define ASSERT(c) do { if (unlikely(!(c))) panic("assertion failed: %s (%s:%d)", #c, __FILE__, __LINE__); } while (0)

/* System-wide timekeeping (milliseconds since boot) */
extern volatile uint64_t system_ticks;
uint64_t time_ms(void);
uint64_t time_unix(void);           /* seconds since epoch (UTC) */
extern int64_t tz_offset_minutes;   /* user-configurable timezone offset */

/* Boot info exported by main.c */
struct boot_info {
    void *initrd;
    uint64_t initrd_size;
    uint64_t rsdp;
    uint64_t kernel_phys, kernel_virt;
    uint64_t mem_total;          /* bytes of usable RAM */
};
extern struct boot_info boot_info;
extern char system_hostname[64];
