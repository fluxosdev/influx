/* AHCI (SATA) driver: polling, one command slot, LBA48 DMA reads/writes. */
#include "blockdev.h"
#include "fs.h"
#include "arch.h"
#include "mm.h"
#include "string.h"
#include "sched.h"
#include "io.h"

struct hba_port {
    uint32_t clb, clbu, fb, fbu, is, ie, cmd, res0, tfd, sig, ssts, sctl, serr, sact, ci, sntf, fbs;
    uint32_t res1[11], vendor[4];
} PACKED;
struct hba_mem {
    uint32_t cap, ghc, is, pi, vs, ccc_ctl, ccc_pts, em_loc, em_ctl, cap2, bohc;
    uint8_t res[0xA0 - 0x2C], vendor[0x100 - 0xA0];
    struct hba_port ports[32];
} PACKED;
struct cmd_header { uint8_t cfl : 5, a : 1, w : 1, p : 1; uint8_t r : 1, b : 1, c : 1, res0 : 1, pmp : 4; uint16_t prdtl; volatile uint32_t prdbc; uint32_t ctba, ctbau, res1[4]; } PACKED;
struct prdt_entry { uint32_t dba, dbau, res0; uint32_t dbc : 22, res1 : 9, i : 1; } PACKED;
struct cmd_table { uint8_t cfis[64], acmd[16], res[48]; struct prdt_entry prdt[16]; } PACKED;
struct fis_h2d { uint8_t type, pmport : 4, res0 : 3, c : 1, command, featurel, lba0, lba1, lba2, device, lba3, lba4, lba5, featureh, countl, counth, icc, control, res1[4]; } PACKED;

struct ahci_port { volatile struct hba_port *regs; struct cmd_header *clb; struct cmd_table *ctb; uint8_t *fb; int index; };
static volatile struct hba_mem *hba;
static int ndisks;

#define PORT_CMD_ST  (1 << 0)
#define PORT_CMD_FRE (1 << 4)
#define PORT_CMD_FR  (1 << 14)
#define PORT_CMD_CR  (1 << 15)
#define ATA_READ_DMA_EXT  0x25
#define ATA_WRITE_DMA_EXT 0x35
#define ATA_IDENTIFY      0xEC
#define ATA_FLUSH_EXT     0xEA

static void port_stop(volatile struct hba_port *p) {
    p->cmd &= ~PORT_CMD_ST;
    p->cmd &= ~PORT_CMD_FRE;
    for (int i = 0; i < 100000 && (p->cmd & (PORT_CMD_FR | PORT_CMD_CR)); i++) pause();
}
static void port_start(volatile struct hba_port *p) {
    while (p->cmd & PORT_CMD_CR) pause();
    p->cmd |= PORT_CMD_FRE;
    p->cmd |= PORT_CMD_ST;
}

static int ahci_cmd(struct ahci_port *ap, uint8_t cmd, uint64_t lba, uint16_t count, void *buf, size_t bytes, bool write) {
    volatile struct hba_port *p = ap->regs;
    for (int i = 0; i < 1000000 && (p->tfd & 0x88); i++) pause();   /* BSY | DRQ */
    if (p->tfd & 0x88) return -EIO;
    struct cmd_header *h = &ap->clb[0];
    h->cfl = sizeof(struct fis_h2d) / 4; h->w = write ? 1 : 0; h->p = 0; h->c = 1;
    struct cmd_table *t = ap->ctb;
    memset(t, 0, sizeof *t);
    int nprd = 0;
    uint64_t phys = V2P(buf);
    size_t left = bytes;
    while (left && nprd < 16) {
        size_t chunk = MIN(left, (size_t)(4 * 1024 * 1024 - 4096));
        t->prdt[nprd].dba = (uint32_t)phys; t->prdt[nprd].dbau = (uint32_t)(phys >> 32);
        t->prdt[nprd].dbc = chunk - 1; t->prdt[nprd].i = 0;
        phys += chunk; left -= chunk; nprd++;
    }
    h->prdtl = nprd;
    h->prdbc = 0;
    struct fis_h2d *f = (struct fis_h2d *)t->cfis;
    f->type = 0x27; f->c = 1; f->command = cmd;
    f->lba0 = lba; f->lba1 = lba >> 8; f->lba2 = lba >> 16; f->device = 1 << 6;
    f->lba3 = lba >> 24; f->lba4 = lba >> 32; f->lba5 = lba >> 40;
    f->countl = count & 0xFF; f->counth = count >> 8;
    p->is = 0xFFFFFFFF;
    p->ci = 1;
    for (int i = 0; i < 5000000; i++) {
        if (!(p->ci & 1)) break;
        if (p->is & (1u << 30)) return -EIO;   /* task file error */
        if (i > 1000) thread_yield(); else pause();
    }
    if (p->ci & 1) return -EIO;
    if (p->is & (1u << 30)) return -EIO;
    return 0;
}

static int ahci_read(struct blockdev *d, uint64_t lba, uint32_t count, void *buf) {
    struct ahci_port *ap = d->priv; uint8_t *out = buf;
    while (count) {
        uint32_t n = MIN(count, 128u);
        int r = ahci_cmd(ap, ATA_READ_DMA_EXT, lba, n, out, n * 512, false);
        if (r < 0) return r;
        lba += n; out += n * 512; count -= n;
    }
    return 0;
}
static int ahci_write(struct blockdev *d, uint64_t lba, uint32_t count, const void *buf) {
    struct ahci_port *ap = d->priv; const uint8_t *in = buf;
    while (count) {
        uint32_t n = MIN(count, 128u);
        int r = ahci_cmd(ap, ATA_WRITE_DMA_EXT, lba, n, (void *)in, n * 512, true);
        if (r < 0) return r;
        lba += n; in += n * 512; count -= n;
    }
    return 0;
}

static void ata_string(const uint16_t *w, int words, char *out) {
    for (int i = 0; i < words; i++) { out[i * 2] = w[i] >> 8; out[i * 2 + 1] = w[i] & 0xFF; }
    out[words * 2] = 0;
    int l = words * 2; while (l > 0 && out[l - 1] == ' ') out[--l] = 0;
}

static void init_port(int i) {
    volatile struct hba_port *p = &hba->ports[i];
    uint32_t ssts = p->ssts;
    if ((ssts & 0xF) != 3 || ((ssts >> 8) & 0xF) != 1) return;   /* no device / not active */
    if (p->sig != 0x00000101) { kprintf("ahci: port %d: non-SATA device (sig %08x), skipped\n", i, p->sig); return; }
    struct ahci_port *ap = kzalloc(sizeof *ap);
    ap->regs = p; ap->index = i;
    port_stop(p);
    uint64_t clb = pmm_alloc_page(), fb = pmm_alloc_page(), ctb = pmm_alloc_page();
    ap->clb = P2V(clb); ap->fb = P2V(fb); ap->ctb = P2V(ctb);
    p->clb = (uint32_t)clb; p->clbu = (uint32_t)(clb >> 32);
    p->fb = (uint32_t)fb; p->fbu = (uint32_t)(fb >> 32);
    ap->clb[0].ctba = (uint32_t)ctb; ap->clb[0].ctbau = (uint32_t)(ctb >> 32);
    p->serr = 0xFFFFFFFF; p->is = 0xFFFFFFFF; p->ie = 0;
    port_start(p);
    uint16_t *id = kmalloc(512);
    if (ahci_cmd(ap, ATA_IDENTIFY, 0, 0, id, 512, false) < 0) { kprintf("ahci: port %d: identify failed\n", i); kfree(id); return; }
    struct blockdev *d = kzalloc(sizeof *d);
    snprintf(d->name, sizeof d->name, "sd%c", 'a' + ndisks++);
    ata_string(id + 27, 20, d->model);
    uint64_t sectors = *(uint64_t *)(id + 100);
    if (!sectors) sectors = *(uint32_t *)(id + 60);
    d->sectors = sectors; d->sector_size = 512;
    d->read = ahci_read; d->write = ahci_write; d->priv = ap; d->bus = "SATA";
    kfree(id);
    blockdev_register(d);
}

void ahci_init(void) {
    for (int i = 0; i < pci_device_count(); i++) {
        struct pci_device *pd = pci_device_at(i);
        if (pd->class_code != 0x01 || pd->subclass != 0x06) continue;
        pci_enable_bus_master(pd);
        uint64_t abar = pd->bar_addr[5];
        if (!abar) continue;
        hba = vmm_map_mmio(abar, 0x1100);
        hba->ghc |= (1u << 31);   /* AHCI enable */
        hba->ghc &= ~(1u << 1);   /* interrupts off; we poll */
        uint32_t pi = hba->pi;
        kprintf("ahci: controller %04x:%04x at %lx, ports %08x, version %x.%x\n", pd->vendor, pd->device, abar, pi, hba->vs >> 16, hba->vs & 0xFFFF);
        for (int port = 0; port < 32; port++) if (pi & (1u << port)) init_port(port);
    }
}
