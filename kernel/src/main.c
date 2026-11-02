#include "kernel.h"
#include "arch.h"
#include "mm.h"
#include "sched.h"
#include "fs.h"
#include "string.h"
#include "io.h"
#include <limine.h>

__attribute__((used, section(".limine_requests_start"))) static volatile LIMINE_REQUESTS_START_MARKER;
__attribute__((used, section(".limine_requests"))) static volatile LIMINE_BASE_REVISION(3);
__attribute__((used, section(".limine_requests"))) volatile struct limine_framebuffer_request framebuffer_request = { .id = LIMINE_FRAMEBUFFER_REQUEST, .revision = 0 };
__attribute__((used, section(".limine_requests"))) volatile struct limine_memmap_request memmap_request = { .id = LIMINE_MEMMAP_REQUEST, .revision = 0 };
__attribute__((used, section(".limine_requests"))) volatile struct limine_hhdm_request hhdm_request = { .id = LIMINE_HHDM_REQUEST, .revision = 0 };
__attribute__((used, section(".limine_requests"))) volatile struct limine_module_request module_request = { .id = LIMINE_MODULE_REQUEST, .revision = 0 };
__attribute__((used, section(".limine_requests"))) volatile struct limine_rsdp_request rsdp_request = { .id = LIMINE_RSDP_REQUEST, .revision = 0 };
__attribute__((used, section(".limine_requests"))) volatile struct limine_kernel_address_request kaddr_request = { .id = LIMINE_KERNEL_ADDRESS_REQUEST, .revision = 0 };
__attribute__((used, section(".limine_requests_end"))) static volatile LIMINE_REQUESTS_END_MARKER;

uint64_t hhdm_offset;
struct boot_info boot_info;
char system_hostname[64] = "influx";

void desktop_main(void *arg);
void drivers_init(void);
void fb_init(void);
void fb_boot_splash(void);
void storage_init(void);

static void init_thread(void *arg) {
    kprintf("init: running as thread %d\n", current_thread->tid);
    pci_init();
    drivers_init();
    storage_init();
    syscall_init();
    desktop_main(NULL);
}

void kmain(void) {
    serial_init();
    kprintf("\n\033[36;1minFlux\033[0m %s booting...\n", INFLUX_VERSION);
    if (!LIMINE_BASE_REVISION_SUPPORTED) panic("limine base revision not supported");
    hhdm_offset = hhdm_request.response->offset;
    boot_info.kernel_phys = kaddr_request.response->physical_base;
    boot_info.kernel_virt = kaddr_request.response->virtual_base;
    boot_info.rsdp = rsdp_request.response ? (uint64_t)rsdp_request.response->address : 0;
    if (boot_info.rsdp >= hhdm_offset) boot_info.rsdp -= hhdm_offset;
    if (module_request.response && module_request.response->module_count > 0) {
        struct limine_file *m = module_request.response->modules[0];
        boot_info.initrd = m->address; boot_info.initrd_size = m->size;
    }
    kprintf("boot: hhdm=%lx kernel phys=%lx virt=%lx initrd=%p (%lu KiB)\n", hhdm_offset,
            boot_info.kernel_phys, boot_info.kernel_virt, boot_info.initrd, boot_info.initrd_size / 1024);

    gdt_init();
    idt_init();
    cpu_init();
    pmm_init();
    vmm_init();
    heap_init();
    fb_init();
    fb_boot_splash();
    acpi_init(boot_info.rsdp);
    lapic_init();
    ioapic_init();
    pit_init();
    sched_init();
    timer_init();
    sti();
    vfs_init();
    if (boot_info.initrd) initrd_unpack(boot_info.initrd, boot_info.initrd_size, "/");
    /* the initrd module memory is bootloader-reclaimable; we copied it, leave it */

    char brand[64]; cpu_get_brand(brand, sizeof brand);
    kprintf("cpu: %s\n", brand);
    kprintf("mem: %lu MiB total, %lu MiB free\n", pmm_total_bytes() >> 20, pmm_free_bytes() >> 20);

    thread_create(kernel_process, "init", init_thread, NULL);
    sched_start();
}
