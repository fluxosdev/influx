/* NVMe driver: admin + one I/O queue pair, polled completions. */
#include "blockdev.h"
#include "fs.h"
#include "arch.h"
#include "mm.h"
#include "string.h"
#include "sched.h"
#include "io.h"

struct nvme_cmd { uint8_t opc, flags; uint16_t cid; uint32_t nsid; uint64_t res, mptr, prp1, prp2; uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15; } PACKED;
struct nvme_cpl { uint32_t dw0, dw1; uint16_t sqhd, sqid, cid, status; } PACKED;

struct nvme_queue { struct nvme_cmd *sq; struct nvme_cpl *cq; uint16_t sq_tail, cq_head, size, id; uint8_t phase; volatile uint32_t *sq_db, *cq_db; };
struct nvme_ctrl {
    volatile uint8_t *regs; uint32_t stride;
    struct nvme_queue admin, io;
    uint16_t cid;
    char model[41], serial[21];
    uint32_t max_transfer;   /* bytes */
};
struct nvme_ns { struct nvme_ctrl *c; uint32_t nsid; uint32_t lba_size; uint64_t nlba; };

#define REG32(c, off) (*(volatile uint32_t *)((c)->regs + (off)))
#define REG64(c, off) (*(volatile uint64_t *)((c)->regs + (off)))
static int nctrls;

static void queue_init(struct nvme_ctrl *c, struct nvme_queue *q, uint16_t id, uint16_t size) {
    q->sq = P2V(pmm_alloc_page()); q->cq = P2V(pmm_alloc_page());
    q->size = size; q->id = id; q->phase = 1; q->sq_tail = 0; q->cq_head = 0;
    q->sq_db = (volatile uint32_t *)(c->regs + 0x1000 + (2 * id) * c->stride);
    q->cq_db = (volatile uint32_t *)(c->regs + 0x1000 + (2 * id + 1) * c->stride);
}

static int submit(struct nvme_ctrl *c, struct nvme_queue *q, struct nvme_cmd *cmd, uint32_t *dw0) {
    cmd->cid = c->cid++;
    q->sq[q->sq_tail] = *cmd;
    q->sq_tail = (q->sq_tail + 1) % q->size;
    *q->sq_db = q->sq_tail;
    for (int i = 0; i < 10000000; i++) {
        volatile struct nvme_cpl *cpl = &q->cq[q->cq_head];
        if ((cpl->status & 1) == q->phase) {
            uint16_t status = cpl->status >> 1;
            if (dw0) *dw0 = cpl->dw0;
            q->cq_head = (q->cq_head + 1) % q->size;
            if (q->cq_head == 0) q->phase ^= 1;
            *q->cq_db = q->cq_head;
            if (status) { kprintf("nvme: command %02x failed, status %04x\n", cmd->opc, status); return -EIO; }
            return 0;
        }
        if (i > 1000) thread_yield(); else pause();
    }
    kprintf("nvme: command %02x timed out\n", cmd->opc);
    return -EIO;
}

static int nvme_rw(struct nvme_ns *ns, uint64_t lba, uint32_t count, void *buf, bool write) {
    struct nvme_ctrl *c = ns->c;
    uint8_t *p = buf;
    uint64_t *prp_list = NULL;
    while (count) {
        uint32_t max_lbas = c->max_transfer / ns->lba_size;
        uint32_t n = MIN(count, max_lbas);
        /* keep each transfer within what a single PRP list page can describe */
        size_t bytes = (size_t)n * ns->lba_size;
        uint64_t phys = V2P(p);
        struct nvme_cmd cmd = { 0 };
        cmd.opc = write ? 1 : 2; cmd.nsid = ns->nsid;
        cmd.prp1 = phys;
        size_t first = PAGE_SIZE - (phys & 0xFFF);
        if (bytes > first) {
            size_t rest = bytes - first;
            uint64_t p2 = (phys & ~0xFFFULL) + PAGE_SIZE;
            if (rest <= PAGE_SIZE) cmd.prp2 = p2;
            else {
                if (!prp_list) prp_list = P2V(pmm_alloc_page());
                size_t pages = (rest + PAGE_SIZE - 1) / PAGE_SIZE;
                if (pages > 512) { n = (uint32_t)((first + 511 * PAGE_SIZE) / ns->lba_size); bytes = (size_t)n * ns->lba_size; rest = bytes - first; pages = (rest + PAGE_SIZE - 1) / PAGE_SIZE; }
                for (size_t i = 0; i < pages; i++) prp_list[i] = p2 + i * PAGE_SIZE;
                cmd.prp2 = V2P(prp_list);
            }
        }
        cmd.cdw10 = (uint32_t)lba; cmd.cdw11 = (uint32_t)(lba >> 32); cmd.cdw12 = n - 1;
        int r = submit(c, &c->io, &cmd, NULL);
        if (r < 0) { if (prp_list) pmm_free_page(V2P(prp_list)); return r; }
        lba += n; p += bytes; count -= n;
    }
    if (prp_list) pmm_free_page(V2P(prp_list));
    return 0;
}
static int nvme_read(struct blockdev *d, uint64_t lba, uint32_t count, void *buf) { return nvme_rw(d->priv, lba, count, buf, false); }
static int nvme_write(struct blockdev *d, uint64_t lba, uint32_t count, const void *buf) { return nvme_rw(d->priv, lba, count, (void *)buf, true); }

static void copy_trim(char *dst, const uint8_t *src, int n) { memcpy(dst, src, n); dst[n] = 0; while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == 0)) dst[--n] = 0; }

static void init_ctrl(struct pci_device *pd) {
    pci_enable_bus_master(pd);
    uint64_t bar = pd->bar_addr[0];
    struct nvme_ctrl *c = kzalloc(sizeof *c);
    c->regs = vmm_map_mmio(bar, 0x4000);
    uint64_t cap = REG64(c, 0x00);
    c->stride = 4 << ((cap >> 32) & 0xF);
    uint32_t mqes = (cap & 0xFFFF) + 1;
    uint32_t vs = REG32(c, 0x08);
    /* disable */
    REG32(c, 0x14) = REG32(c, 0x14) & ~1u;
    for (int i = 0; i < 1000000 && (REG32(c, 0x1C) & 1); i++) pause();
    queue_init(c, &c->admin, 0, 32);
    REG32(c, 0x24) = (31 << 16) | 31;   /* AQA: sizes - 1 */
    REG64(c, 0x28) = V2P(c->admin.sq);
    REG64(c, 0x30) = V2P(c->admin.cq);
    REG32(c, 0x0C) = 0xFFFFFFFF;        /* INTMS: mask all interrupts, we poll */
    /* CC: IOCQES=4 (16B), IOSQES=6 (64B), MPS=0 (4K), CSS=0 NVM, EN=1 */
    REG32(c, 0x14) = (4 << 20) | (6 << 16) | 1;
    for (int i = 0; i < 1000000 && !(REG32(c, 0x1C) & 1); i++) pause();
    if (!(REG32(c, 0x1C) & 1)) { kprintf("nvme: controller failed to become ready\n"); return; }
    /* identify controller */
    uint8_t *idbuf = P2V(pmm_alloc_page());
    struct nvme_cmd cmd = { 0 };
    cmd.opc = 0x06; cmd.prp1 = V2P(idbuf); cmd.cdw10 = 1;
    if (submit(c, &c->admin, &cmd, NULL) < 0) return;
    copy_trim(c->serial, idbuf + 4, 20); copy_trim(c->model, idbuf + 24, 40);
    uint8_t mdts = idbuf[77];
    c->max_transfer = mdts ? (1u << mdts) * PAGE_SIZE : 1024 * 1024;
    if (c->max_transfer > 512 * PAGE_SIZE) c->max_transfer = 512 * PAGE_SIZE;
    uint32_t nn = *(uint32_t *)(idbuf + 516);
    kprintf("nvme: %s (serial %s), NVMe %u.%u, %u namespace(s)\n", c->model, c->serial, vs >> 16, (vs >> 8) & 0xFF, nn);
    /* I/O queues */
    uint16_t qsize = MIN(64u, mqes);
    queue_init(c, &c->io, 1, qsize);
    memset(&cmd, 0, sizeof cmd);
    cmd.opc = 0x05; cmd.prp1 = V2P(c->io.cq); cmd.cdw10 = ((qsize - 1) << 16) | 1; cmd.cdw11 = 1;   /* physically contiguous, no interrupts */
    if (submit(c, &c->admin, &cmd, NULL) < 0) return;
    memset(&cmd, 0, sizeof cmd);
    cmd.opc = 0x01; cmd.prp1 = V2P(c->io.sq); cmd.cdw10 = ((qsize - 1) << 16) | 1; cmd.cdw11 = (1 << 16) | 1;
    if (submit(c, &c->admin, &cmd, NULL) < 0) return;
    /* namespaces */
    for (uint32_t nsid = 1; nsid <= nn && nsid <= 4; nsid++) {
        memset(&cmd, 0, sizeof cmd);
        cmd.opc = 0x06; cmd.prp1 = V2P(idbuf); cmd.nsid = nsid; cmd.cdw10 = 0;
        if (submit(c, &c->admin, &cmd, NULL) < 0) continue;
        uint64_t nsze = *(uint64_t *)idbuf;
        if (!nsze) continue;
        uint8_t flbas = idbuf[26] & 0xF;
        uint8_t lbads = idbuf[128 + flbas * 4 + 2];
        struct nvme_ns *ns = kzalloc(sizeof *ns);
        ns->c = c; ns->nsid = nsid; ns->lba_size = 1u << lbads; ns->nlba = nsze;
        struct blockdev *d = kzalloc(sizeof *d);
        snprintf(d->name, sizeof d->name, "nvme%dn%u", nctrls, nsid);
        strlcpy(d->model, c->model, sizeof d->model);
        d->sectors = nsze; d->sector_size = ns->lba_size;
        d->read = nvme_read; d->write = nvme_write; d->priv = ns; d->bus = "NVMe";
        blockdev_register(d);
    }
    pmm_free_page(V2P(idbuf));
    nctrls++;
}

void nvme_init(void) {
    for (int i = 0; i < pci_device_count(); i++) {
        struct pci_device *pd = pci_device_at(i);
        if (pd->class_code == 0x01 && pd->subclass == 0x08) init_ctrl(pd);
    }
}
