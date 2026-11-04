#include "random.h"
#include "kernel.h"
#include "arch.h"
#include "mm.h"
#include "sched.h"
#include "fs.h"
#include "string.h"
#include "io.h"
#include "simd.h"
#include "acpi.h"
#include "input.h"
#include "usb.h"
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
void fb_boot_error(const char *msg);
void storage_init(void);
void net_init(void);
void fb_boot_progress(const char *msg);

/* boot option 'selftest' (CI): run /bin/systest with its output on the serial port, then power off */
static void selftest_thread(void *arg) {
    thread_sleep_ms(kernel_cmdline_has("selftest-net") ? 8000 : 500);   /* DHCP first for the network checks */
    struct file *con;
    if (vfs_open("/dev/console", O_RDWR, &con) < 0) return;
    struct file *stdio[3] = { con, con, con };
    char *argv[] = { "systest", kernel_cmdline_has("selftest-net") ? "net" : NULL, NULL };
    int pid = process_spawn("/bin/systest", argv[1] ? 2 : 1, argv, stdio, "/");
    int status = -1;
    if (pid > 0) process_wait(pid, &status);
    kprintf("SELFTEST: %s (status %d)\n", pid > 0 && status == 0 ? "PASSED" : "FAILED", pid > 0 ? status : pid);
    vfs_close(con);
    thread_sleep_ms(200);
    acpi_shutdown();
}

static void init_thread(void *arg) {
    kprintf("init: running as thread %d\n", current_thread->tid);
    pci_init();
    fb_boot_progress("Loading ACPI");
    acpi_namespace_init();   /* before the drivers: they route PCI interrupts through _PRT */
    input_init();            /* USB keyboards and mice deliver events from here on */
    drivers_init();
    storage_init();
    usb_init();              /* after the disks: a USB stick does not take /home from an installed disk */
    crashlog_init();         /* the installed system's EFI partition is mounted by now */
    net_init();
    syscall_init();
    vfs_mkdir("/tmp"); vfs_mkdir("/etc");
    if (kernel_cmdline_has("selftest")) thread_create(kernel_process, "selftest", selftest_thread, NULL);
    desktop_main(NULL);
}

void syscall_init_cpu(void);
void devices_init(void);

void kmain(void) {
    percpu_init(&cpus[0], 0);   /* before anything reads current_thread */
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
    syscall_init_cpu();
    pmm_init();
    vmm_init();
    heap_init();
    fb_init();
    fb_boot_splash();
    if (!cpu_feat.avx) {   /* checked before anything built with -mavx can run */
        fb_boot_error("inFlux needs a processor with AVX (Intel Sandy Bridge / AMD Bulldozer or newer).");
        panic("CPU does not support AVX (or XSAVE)");
    }
    acpi_init(boot_info.rsdp);
    lapic_init();
    ioapic_init();
    pit_init();
    sched_init();
    timer_init();
    random_init();
    sti();
    smp_init();
    vfs_init();
    devices_init();
    if (boot_info.initrd) initrd_unpack(boot_info.initrd, boot_info.initrd_size, "/");
    /* the initrd module memory is bootloader-reclaimable; we copied it, leave it */

    char brand[64]; cpu_get_brand(brand, sizeof brand);
    kprintf("cpu: %s\n", brand);
    char simd[64]; cpu_simd_describe(simd, sizeof simd);
    kprintf("cpu: %s; %s state, %u bytes per thread\n", simd,
            cpu_feat.xsaveopt ? "XSAVEOPT" : cpu_feat.xsave ? "XSAVE" : "FXSAVE", fpu_state_size);
    kprintf("mem: %lu MiB total, %lu MiB free\n", pmm_total_bytes() >> 20, pmm_free_bytes() >> 20);

    thread_create(kernel_process, "init", init_thread, NULL);
    sched_start();
}
