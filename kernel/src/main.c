#include "random.h"
#include "power.h"
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

struct blockdev;
struct blockdev *blockdev_find(const char *name);
int blockdev_read(struct blockdev *d, uint64_t lba, uint32_t count, void *buf);
int btrfs_mkfs(struct blockdev *d, const char *label);
struct vnode *btrfs_mount(struct blockdev *d);
int btrfs_unmount(struct vnode *root);

/* selftest-btrfs: format the first disk with inFlux's own mkfs.btrfs and run `systest posix` on it.
 * Only a disk whose first 128 KiB (partition tables, the btrfs superblock) are all zeros is touched. */
static int selftest_btrfs(struct file *stdio[3]) {
    struct blockdev *d = blockdev_find("sda");
    if (!d) { kprintf("selftest-btrfs: no disk sda\n"); return -1; }
    uint8_t *buf = kmalloc(128 * 1024);
    bool blank = buf && blockdev_read(d, 0, 256, buf) == 0;
    for (int i = 0; blank && i < 128 * 1024; i++) if (buf[i]) blank = false;
    kfree(buf);
    if (!blank) { kprintf("selftest-btrfs: sda is not blank; not formatting it\n"); return -1; }
    int r = btrfs_mkfs(d, "selftest");
    struct vnode *root = r == 0 ? btrfs_mount(d) : NULL;
    if (!root || vfs_mount("/mnt/selftest-btrfs", root) < 0) { kprintf("selftest-btrfs: cannot format or mount sda (%d)\n", r); return -1; }
    char *argv[] = { "systest", "posix", "/mnt/selftest-btrfs", NULL };
    int pid = process_spawn("/bin/systest", 3, argv, stdio, "/");
    int status = -1;
    if (pid > 0) process_wait(pid, &status);
    vfs_unmount("/mnt/selftest-btrfs");
    if (btrfs_unmount(root) < 0) { kprintf("selftest-btrfs: write-back on unmount failed\n"); return -1; }
    return pid > 0 ? status : -1;
}

int selftest_install(const char *next_cmdline);
int selftest_next_stage(const char *next_cmdline);
int selftest_update_image(int nfiles, const char *const *names, const char *const *contents);

static int run_systest(struct file *stdio[3], char *a1, char *a2) {
    char *argv[] = { "systest", a1, a2, NULL };
    int pid = process_spawn("/bin/systest", a2 ? 3 : a1 ? 2 : 1, argv, stdio, "/");
    int status = -1;
    if (pid > 0) process_wait(pid, &status);
    return pid > 0 ? status : -1;
}

/* make test-persist (tests/run-persist.sh): install, then two boots from the disk */
static int selftest_persist(struct file *stdio[3]) {
    bool v1 = kernel_cmdline_has("selftest-v1");   /* the install is turned into the first layout, which must move */
    if (kernel_cmdline_has("selftest-install")) return selftest_install(v1 ? "selftest selftest-v1 selftest-persist-1" : "selftest selftest-persist-1");
    if (kernel_cmdline_has("selftest-persist-1")) {
        if (v1) { char *b; size_t n; bool ok = vfs_read_file("/home/flux/from-v1", (void **)&b, &n) == 0; if (ok) kfree(b);
                  kprintf("%s the first layout's home directory is at /home/flux\n", ok ? "PASS" : "FAIL"); if (!ok) return 1; }
        int st = run_systest(stdio, "posix", "/home/flux");   /* the POSIX file checks on the root volume */
        struct vnode *gcc = vfs_lookup("/usr/bin/gcc");       /* the developer tools package: build on the disk */
        if (gcc) { vnode_unref(gcc); if (st == 0) st = run_systest(stdio, "devtools", NULL); }
        if (st == 0) st = run_systest(stdio, "persist", "write");
        /* a system update: one new file, one the user left alone, one the user changed */
        static const char *const names[] = { "./etc/persist-image-added", "./etc/hosts", "./etc/motd" };
        static const char *const contents[] = { "added by the update\n", "127.0.0.1 localhost updated\n", "the update's motd\n" };
        if (st == 0) st = selftest_update_image(3, names, contents);
        if (st == 0) st = selftest_next_stage("selftest selftest-persist-2");
        return st;
    }
    if (kernel_cmdline_has("selftest-persist-2")) {
        int st = run_systest(stdio, "persist", "check");
        if (st == 0) st = selftest_next_stage("");   /* done: the disk boots normally from now on */
        return st;
    }
    return 0;
}

/* boot option 'selftest' (CI): run /bin/systest with its output on the serial port, then power off */
static void selftest_thread(void *arg) {
    thread_sleep_ms(kernel_cmdline_has("selftest-net") ? 8000 : 500);   /* DHCP first for the network checks */
    struct file *con;
    if (vfs_open("/dev/console", O_RDWR, &con) < 0) return;
    struct file *stdio[3] = { con, con, con };
    char *argv[] = { "systest", kernel_cmdline_has("selftest-net") ? "net" : NULL, NULL, NULL };
    if (kernel_cmdline_has("selftest-fs")) { argv[1] = "fs"; argv[2] = "/mnt/sda"; }   /* a test volume as the first disk */
    int argc = argv[2] ? 3 : argv[1] ? 2 : 1;
    int pid = process_spawn("/bin/systest", argc, argv, stdio, "/");
    int status = -1;
    if (pid > 0) process_wait(pid, &status);
    if (pid > 0 && status == 0 && kernel_cmdline_has("selftest-btrfs")) status = selftest_btrfs(stdio);
    if (pid > 0 && status == 0) status = selftest_persist(stdio);
    if (pid > 0 && status == 0 && kernel_cmdline_has("selftest-devtools")) {   /* a DEVTOOLS=1 image: gcc, sh, make */
        char *dargv[] = { "systest", "devtools", NULL };
        int dpid = process_spawn("/bin/systest", 2, dargv, stdio, "/");
        status = -1;
        if (dpid > 0) process_wait(dpid, &status);
    }
    kprintf("SELFTEST: %s (status %d)\n", pid > 0 && status == 0 ? "PASSED" : "FAILED", pid > 0 ? status : pid);
    vfs_close(con);
    vfs_sync();   /* an installed system's root is a disk now */
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
    sysroot_boot_done();     /* no root switch once the desktop runs */
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
    /* the module named "initrd" (or the first) is the system image; others are packages */
    for (uint64_t i = 0; module_request.response && i < module_request.response->module_count; i++) {
        struct limine_file *m = module_request.response->modules[i];
#if LIMINE_API_REVISION >= 3
        const char *name = m->string ? m->string : "";
#else
        const char *name = m->cmdline ? m->cmdline : "";
#endif
        if (!boot_info.initrd && (!strcmp(name, "initrd") || !name[0] || i == module_request.response->module_count - 1)) {
            boot_info.initrd = m->address; boot_info.initrd_size = m->size;
            boot_info.boot_media = m->media_type;
            memcpy(boot_info.boot_disk_guid, &m->gpt_disk_uuid, 16);
        } else if (boot_info.npackages < 4) {
            struct boot_package *bp = &boot_info.packages[boot_info.npackages++];
            bp->data = m->address; bp->size = m->size;
            strlcpy(bp->name, name[0] ? name : "package", sizeof bp->name);
        }
    }
    kprintf("boot: hhdm=%lx kernel phys=%lx virt=%lx initrd=%p (%lu KiB)\n", hhdm_offset,
            boot_info.kernel_phys, boot_info.kernel_virt, boot_info.initrd, boot_info.initrd_size / 1024);
    { const uint8_t *g = boot_info.boot_disk_guid;
      kprintf("boot: from %s, disk GUID %02x%02x%02x%02x-...-%02x%02x%02x%02x\n", boot_info.boot_media == 1 ? "optical media" : boot_info.boot_media == 2 ? "the network" : "a disk", g[0], g[1], g[2], g[3], g[12], g[13], g[14], g[15]); }

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
    cpupower_init();
    sti();
    smp_init();
    vfs_init();
    devices_init();
    if (boot_info.initrd) initrd_unpack(boot_info.initrd, boot_info.initrd_size, "/");
    for (int i = 0; i < boot_info.npackages; i++) {   /* a live system: packages over the system image */
        kprintf("initrd: package '%s' (%lu KiB)\n", boot_info.packages[i].name, boot_info.packages[i].size / 1024);
        initrd_unpack(boot_info.packages[i].data, boot_info.packages[i].size, "/");
    }
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
