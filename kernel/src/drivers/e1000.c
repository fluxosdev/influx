/*
 * Intel PRO/1000 ("em") Ethernet driver.
 *
 * Covers the 8254x and 82574 controllers (QEMU's "e1000" and "e1000e") and
 * the LAN controller built into the Intel 8-series (Lynx Point) PCH, whose
 * PHY is the I217 (I217-LM, I217-V, and the I218-LM2/V2 on the same
 * platform). They share one programming model: a 128 KiB register BAR,
 * legacy 16-byte receive and transmit descriptors in host memory, the
 * station address in RAL0/RAH0 and the PHY on the MDIO bus behind MDIC.
 *
 * What the PCH parts need on top, following Linux's e1000e (ich8lan.c):
 *   - The PHY is shared with the Management Engine. MDIO access and resets
 *     are done while holding the software flag in EXTCNF_CTRL.
 *   - After a cold boot or an OS that left it in S0ix/Sx, the MAC-PHY link
 *     may sit in SMBus mode with the PHY unreachable over MDIO. Forcing
 *     SMBus and toggling the LANPHYPC value bit power-cycles the PHY back to
 *     PCIe mode. Unlike Linux, which gates the hardware's automatic PHY
 *     configuration and then loads the NVM's PHY settings itself, the gate
 *     is left open so the hardware configures the PHY after the toggle.
 *   - A handful of reserved bits the hardware expects (TARC, TXDCTL,
 *     CTRL_EXT), descriptor ECC, NFS filtering off and the I217 beacon
 *     duration fix for packet loss.
 * The PHY itself is not reset (Linux resets MAC and PHY together): the
 * firmware's PHY configuration and any link it already has are kept, and
 * auto-negotiation is only restarted when the PHY was powered down. The
 * I217 link-up workarounds for 10 Mbit half duplex, EMI tuning and LTR are
 * not implemented.
 *
 * Interrupts use MSI when offered (82574, PCH) or the INTx line routed
 * through ACPI _PRT (QEMU's 8254x); the handler reads ICR (which clears and
 * deasserts it) and wakes the network thread, which drains the receive ring.
 * Without an interrupt the adapter is polled.
 */
#include "net.h"
#include "arch.h"
#include "io.h"
#include "mm.h"
#include "string.h"

/* ---- registers ---- */
#define E_CTRL        0x00000
#define E_STATUS      0x00008
#define E_EERD        0x00014
#define E_CTRL_EXT    0x00018
#define E_MDIC        0x00020
#define E_FEXTNVM4    0x00024
#define E_FEXTNVM     0x00028
#define E_FEXTNVM3    0x0003C
#define E_ICR         0x000C0
#define E_ITR         0x000C4
#define E_IMS         0x000D0
#define E_IMC         0x000D8
#define E_RCTL        0x00100
#define E_TCTL        0x00400
#define E_TIPG        0x00410
#define E_EXTCNF_CTRL 0x00F00
#define E_PHY_CTRL    0x00F10
#define E_PBECCSTS    0x0100C
#define E_RDBAL       0x02800
#define E_RDBAH       0x02804
#define E_RDLEN       0x02808
#define E_RDH         0x02810
#define E_RDT         0x02818
#define E_RDTR        0x02820
#define E_RADV        0x0282C
#define E_KABGTXD     0x03004
#define E_TDBAL       0x03800
#define E_TDBAH       0x03804
#define E_TDLEN       0x03808
#define E_TDH         0x03810
#define E_TDT         0x03818
#define E_TIDV        0x03820
#define E_TXDCTL0     0x03828
#define E_TARC0       0x03840
#define E_TXDCTL1     0x03928
#define E_TARC1       0x03940
#define E_GCR         0x05B00
#define E_GCR2        0x05B64
#define E_RFCTL       0x05008
#define E_MTA         0x05200
#define E_RAL0        0x05400
#define E_RAH0        0x05404
#define E_FWSM        0x05B54

#define CTRL_FD            (1u << 0)
#define CTRL_GIO_MASTER_DISABLE (1u << 2)
#define CTRL_LRST          (1u << 3)
#define CTRL_ASDE          (1u << 5)
#define CTRL_SLU           (1u << 6)
#define CTRL_ILOS          (1u << 7)
#define CTRL_FRCSPD        (1u << 11)
#define CTRL_FRCDPX        (1u << 12)
#define CTRL_LANPHYPC_OVERRIDE (1u << 16)
#define CTRL_LANPHYPC_VALUE    (1u << 17)
#define CTRL_MEHE          (1u << 19)
#define CTRL_RST           (1u << 26)
#define CTRL_VME           (1u << 30)
#define CTRL_PHY_RST       (1u << 31)
#define STATUS_FD          (1u << 0)
#define STATUS_LU          (1u << 1)
#define STATUS_LAN_INIT_DONE (1u << 9)
#define STATUS_GIO_MASTER_ENABLE (1u << 19)
#define CTRL_EXT_LPCD      (1u << 2)
#define CTRL_EXT_FORCE_SMBUS (1u << 11)
#define CTRL_EXT_RO_DIS    (1u << 17)
#define CTRL_EXT_PHYPDEN   (1u << 20)
#define EXTCNF_SWFLAG      (1u << 5)
#define FWSM_RSPCIPHY      (1u << 6)   /* set: the ME allows PHY resets */
#define FWSM_FW_VALID      (1u << 15)  /* ME firmware present */
#define FEXTNVM_SW_CONFIG  (1u << 27)
#define FEXTNVM3_PHY_CFG_COUNTER_MASK  (3u << 26)
#define FEXTNVM3_PHY_CFG_COUNTER_50MS  (2u << 26)
#define FEXTNVM4_BEACON_DURATION_MASK  7u
#define FEXTNVM4_BEACON_DURATION_8US   7u
#define PHY_CTRL_D0A_LPLU  (1u << 1)
#define PHY_CTRL_GBE_DISABLE (1u << 6)
#define PBECCSTS_ECC_ENABLE (1u << 16)
#define KABGTXD_BGSQLBIAS  0x00050000u
#define RFCTL_NFSW_DIS     (1u << 6)
#define RFCTL_NFSR_DIS     (1u << 7)
#define GCR_NO_SNOOP_ALL   0x3Fu
#define GCR_L1_ACT_WITHOUT_L0S_RX (1u << 27)
#define TXDCTL_WTHRESH     0x003F0000u
#define TXDCTL_PTHRESH     0x0000003Fu
#define TXDCTL_FULL_WB     0x01010000u  /* GRAN = descriptors, WTHRESH = 1 */
#define TXDCTL_PREFETCH    0x0000001Fu
#define TXDCTL_COUNT_DESC  0x00400000u

#define ICR_TXDW   (1u << 0)
#define ICR_LSC    (1u << 2)
#define ICR_RXDMT0 (1u << 4)
#define ICR_RXO    (1u << 6)
#define ICR_RXT0   (1u << 7)
#define IMS_BITS   (ICR_TXDW | ICR_LSC | ICR_RXDMT0 | ICR_RXO | ICR_RXT0)

#define RCTL_EN    (1u << 1)
#define RCTL_BAM   (1u << 15)
#define RCTL_SECRC (1u << 26)    /* strip the frame check sequence */
#define TCTL_EN    (1u << 1)
#define TCTL_PSP   (1u << 3)     /* pad short frames */
#define TCTL_CT    (0x0Fu << 4)
#define TCTL_COLD  (0x3Fu << 12)
#define TCTL_RTLC  (1u << 24)
#define TIPG_COPPER ((6u << 20) | (8u << 10) | 8u)

#define MDIC_OP_WRITE (1u << 26)
#define MDIC_OP_READ  (2u << 26)
#define MDIC_READY    (1u << 28)
#define MDIC_ERROR    (1u << 30)
#define RAH_AV        (1u << 31)

/* PHY registers: standard MII, plus the HV/I217 paged ones as page << 5 | reg */
#define MII_BMCR     0
#define MII_BMSR     1
#define MII_PHYID1   2
#define MII_PHYID2   3
#define BMCR_ANRESTART (1u << 9)
#define BMCR_PDOWN     (1u << 11)
#define BMCR_ANENABLE  (1u << 12)
#define PHY_REG(page, reg) (((page) << 5) | (reg))
#define HV_OEM_BITS    PHY_REG(768, 25)
#define CV_SMB_CTRL    PHY_REG(769, 23)
#define HV_OEM_LPLU        0x0004
#define HV_OEM_GBE_DIS     0x0040
#define HV_OEM_RESTART_AN  0x0400
#define CV_SMB_FORCE_SMBUS 0x0001
#define PHY_PAGE_SELECT 0x1F

/* ---- descriptors ---- */
struct rx_desc { uint64_t addr; uint16_t len, csum; uint8_t status, errors; uint16_t special; } PACKED;
struct tx_desc { uint64_t addr; uint16_t len; uint8_t cso, cmd, status, css; uint16_t special; } PACKED;
#define RXD_DD   0x01
#define RXD_EOP  0x02
#define RXE_FRAME 0x97          /* CRC, symbol, sequence, carrier extension, RX data errors */
#define TXD_EOP  0x01
#define TXD_IFCS 0x02
#define TXD_RS   0x08
#define TXD_DD   0x01

#define NRX 256
#define NTX 128
#define BUFSZ 2048

enum em_type { EM_8254X, EM_82574, EM_PCH_LPT };

struct em {
    struct netif netif;
    struct pci_device *pd;
    volatile uint32_t *regs;
    enum em_type type;
    int phy_addr;
    uint32_t phy_id;
    struct rx_desc *rx; struct tx_desc *tx;
    uint8_t *rx_buf, *tx_buf;
    uint64_t rx_buf_phys, tx_buf_phys;
    unsigned rx_next, tx_tail;
    bool rx_discard;              /* dropping the rest of an oversized frame */
    bool irq;
    volatile uint32_t icr_pending;
    uint64_t irqs;
    bool link_up;
};

static int ncards;

static inline uint32_t rd(struct em *e, uint32_t reg) { return e->regs[reg / 4]; }
static inline void wr(struct em *e, uint32_t reg, uint32_t v) { e->regs[reg / 4] = v; }
static inline void flush(struct em *e) { (void)rd(e, E_STATUS); }
static void udelay(uint32_t us) {
    uint64_t hz = cpu_tsc_hz() ? cpu_tsc_hz() : 2000000000ull;
    uint64_t end = rdtsc() + hz / 1000000 * us;
    while (rdtsc() < end) pause();
}
static void msleep(uint32_t ms) { thread_sleep_ms(ms); }

/* ---- PCH: software flag (MDIO/reset ownership shared with the ME) ---- */
static bool swflag_acquire(struct em *e) {
    if (e->type != EM_PCH_LPT) return true;
    uint32_t v = 0;
    for (int i = 0; i < 100; i++) { v = rd(e, E_EXTCNF_CTRL); if (!(v & EXTCNF_SWFLAG)) break; msleep(1); }
    if (v & EXTCNF_SWFLAG) { kprintf("em: software flag held by firmware (EXTCNF_CTRL %08x)\n", v); return false; }
    wr(e, E_EXTCNF_CTRL, v | EXTCNF_SWFLAG);
    for (int i = 0; i < 1000; i++) {
        if (rd(e, E_EXTCNF_CTRL) & EXTCNF_SWFLAG) return true;
        msleep(1);
    }
    kprintf("em: firmware did not grant the software flag (FWSM %08x)\n", rd(e, E_FWSM));
    wr(e, E_EXTCNF_CTRL, rd(e, E_EXTCNF_CTRL) & ~EXTCNF_SWFLAG);
    return false;
}
static void swflag_release(struct em *e) {
    if (e->type == EM_PCH_LPT) wr(e, E_EXTCNF_CTRL, rd(e, E_EXTCNF_CTRL) & ~EXTCNF_SWFLAG);
}
/* true while the ME forbids PHY resets (LANPHYPC toggles) */
static bool phy_reset_blocked(struct em *e) {
    for (int i = 0; i < 30; i++) {
        if (rd(e, E_FWSM) & FWSM_RSPCIPHY) return false;
        msleep(10);
    }
    return true;
}

/* ---- MDIO ---- */
static bool mdic(struct em *e, int phy, int reg, bool write, uint16_t *val) {
    uint32_t cmd = ((uint32_t)(reg & 0x1F) << 16) | ((uint32_t)phy << 21) | (write ? MDIC_OP_WRITE | *val : MDIC_OP_READ);
    wr(e, E_MDIC, cmd);
    for (int i = 0; i < 1920; i++) {
        udelay(50);
        uint32_t v = rd(e, E_MDIC);
        if (!(v & MDIC_READY)) continue;
        if ((v & MDIC_ERROR) || ((v >> 16) & 0x1F) != (uint32_t)(reg & 0x1F)) return false;
        if (!write) *val = v & 0xFFFF;
        if (e->type == EM_PCH_LPT) udelay(100);   /* the PCH PHY needs a breather between accesses */
        return true;
    }
    return false;
}
/* a PHY register, paged on the PCH's HV-style PHY (pages >= 768 live at address 1) */
static bool phy_rw(struct em *e, uint32_t preg, bool write, uint16_t *val) {
    int page = preg >> 5, reg = preg & 0x1F, addr = e->phy_addr;
    if (e->type == EM_PCH_LPT && page) {
        if (page >= 768) addr = 1;
        uint16_t sel = (uint16_t)((page == 768 ? 0 : page) << 5);
        if (reg > 0xF && !mdic(e, 1, PHY_PAGE_SELECT, true, &sel)) return false;
    }
    return mdic(e, addr, reg, write, val);
}
static bool phy_read(struct em *e, uint32_t reg, uint16_t *v) { return phy_rw(e, reg, false, v); }
static bool phy_write(struct em *e, uint32_t reg, uint16_t v) { return phy_rw(e, reg, true, &v); }

static bool phy_id_ok(struct em *e) {
    for (int tries = 0; tries < 2; tries++) {
        uint16_t id1, id2;
        if (!mdic(e, e->phy_addr, MII_PHYID1, false, &id1) || id1 == 0xFFFF) continue;
        if (!mdic(e, e->phy_addr, MII_PHYID2, false, &id2) || id2 == 0xFFFF) continue;
        e->phy_id = (uint32_t)id1 << 16 | id2;
        return e->phy_id != 0;
    }
    return false;
}

/* PCH, software flag held: is the PHY reachable over MDIO? If so and no ME
 * firmware runs, take both the PHY and the MAC out of forced SMBus mode. */
static bool pch_phy_accessible(struct em *e) {
    if (!phy_id_ok(e)) return false;
    if (!(rd(e, E_FWSM) & FWSM_FW_VALID)) {
        uint16_t v;
        if (phy_read(e, CV_SMB_CTRL, &v) && (v & CV_SMB_FORCE_SMBUS)) phy_write(e, CV_SMB_CTRL, v & ~CV_SMB_FORCE_SMBUS);
        wr(e, E_CTRL_EXT, rd(e, E_CTRL_EXT) & ~CTRL_EXT_FORCE_SMBUS);
    }
    return true;
}

/* power-cycle the PHY through LANPHYPC, which puts the MAC-PHY interconnect back in PCIe mode */
static void pch_toggle_lanphypc(struct em *e) {
    uint32_t v = rd(e, E_FEXTNVM3);
    wr(e, E_FEXTNVM3, (v & ~FEXTNVM3_PHY_CFG_COUNTER_MASK) | FEXTNVM3_PHY_CFG_COUNTER_50MS);
    v = rd(e, E_CTRL);
    v |= CTRL_LANPHYPC_OVERRIDE;
    v &= ~CTRL_LANPHYPC_VALUE;
    wr(e, E_CTRL, v); flush(e);
    udelay(20);
    v &= ~CTRL_LANPHYPC_OVERRIDE;
    wr(e, E_CTRL, v); flush(e);
    for (int i = 0; i < 20 && !(rd(e, E_CTRL_EXT) & CTRL_EXT_LPCD); i++) msleep(5);
    msleep(30);
}

/* e1000_init_phy_workarounds_pchlan without the PHY reset */
static bool pch_phy_init(struct em *e) {
    if (!swflag_acquire(e)) return false;
    bool ok = pch_phy_accessible(e);
    if (!ok) {
        kprintf("em: PHY not reachable over MDIO, trying SMBus mode\n");
        wr(e, E_CTRL_EXT, rd(e, E_CTRL_EXT) | CTRL_EXT_FORCE_SMBUS);
        msleep(50);
        ok = pch_phy_accessible(e);
    }
    if (!ok) {
        if (phy_reset_blocked(e)) kprintf("em: LANPHYPC toggle blocked by the ME\n");
        else {
            kprintf("em: toggling LANPHYPC to power the PHY up\n");
            pch_toggle_lanphypc(e);
            ok = pch_phy_accessible(e);
            if (!ok) {
                wr(e, E_CTRL_EXT, rd(e, E_CTRL_EXT) & ~CTRL_EXT_FORCE_SMBUS);
                ok = pch_phy_accessible(e);
            }
        }
    }
    swflag_release(e);
    if (!ok) kprintf("em: PHY still unreachable (CTRL_EXT %08x, FWSM %08x); the link may not come up\n", rd(e, E_CTRL_EXT), rd(e, E_FWSM));
    return ok;
}

/* ---- reset and setup ---- */
static void em_reset(struct em *e) {
    wr(e, E_IMC, 0xFFFFFFFF);
    wr(e, E_RCTL, 0);
    wr(e, E_TCTL, TCTL_PSP);
    flush(e);
    msleep(10);
    if (e->type != EM_8254X) {
        /* let outstanding DMA finish before the reset (e1000e_disable_pcie_master) */
        wr(e, E_CTRL, rd(e, E_CTRL) | CTRL_GIO_MASTER_DISABLE);
        for (int i = 0; i < 800 && (rd(e, E_STATUS) & STATUS_GIO_MASTER_ENABLE); i++) udelay(100);
    }
    /* MAC reset only: the PHY keeps the firmware's configuration and link.
     * On the PCH the reset needs the software flag (it clears it, too). */
    bool flag = swflag_acquire(e);
    wr(e, E_CTRL, rd(e, E_CTRL) | CTRL_RST);
    msleep(e->type == EM_PCH_LPT ? 20 : 10);
    for (int i = 0; i < 100 && (rd(e, E_CTRL) & CTRL_RST); i++) msleep(1);
    if (flag && (rd(e, E_EXTCNF_CTRL) & EXTCNF_SWFLAG)) swflag_release(e);
    wr(e, E_IMC, 0xFFFFFFFF);
    (void)rd(e, E_ICR);
    if (e->type == EM_PCH_LPT) wr(e, E_KABGTXD, rd(e, E_KABGTXD) | KABGTXD_BGSQLBIAS);
}

/* reserved bits and errata settings from e1000_initialize_hw_bits_ich8lan/_82571 */
static void em_hw_bits(struct em *e) {
    if (e->type == EM_PCH_LPT) {
        wr(e, E_CTRL_EXT, rd(e, E_CTRL_EXT) | (1u << 22) | CTRL_EXT_PHYPDEN | CTRL_EXT_RO_DIS);
        wr(e, E_TXDCTL0, rd(e, E_TXDCTL0) | (1u << 22));
        wr(e, E_TXDCTL1, rd(e, E_TXDCTL1) | (1u << 22));
        wr(e, E_TARC0, rd(e, E_TARC0) | (1u << 23) | (1u << 24) | (1u << 26) | (1u << 27));
        wr(e, E_TARC1, rd(e, E_TARC1) | (1u << 24) | (1u << 26) | (1u << 28) | (1u << 30));
        wr(e, E_RFCTL, rd(e, E_RFCTL) | RFCTL_NFSW_DIS | RFCTL_NFSR_DIS);
        wr(e, E_PBECCSTS, rd(e, E_PBECCSTS) | PBECCSTS_ECC_ENABLE);
        wr(e, E_CTRL, rd(e, E_CTRL) | CTRL_MEHE);
        /* I217 packet loss: beacon duration 8 us */
        wr(e, E_FEXTNVM4, (rd(e, E_FEXTNVM4) & ~FEXTNVM4_BEACON_DURATION_MASK) | FEXTNVM4_BEACON_DURATION_8US);
    }
    if (e->type == EM_82574) {
        /* e1000_initialize_hw_bits_82571 for the 82574/82583; GCR2 bit 0 avoids Tx timeouts */
        wr(e, E_TXDCTL0, rd(e, E_TXDCTL0) | (1u << 22));
        wr(e, E_TXDCTL1, rd(e, E_TXDCTL1) | (1u << 22));
        wr(e, E_TARC0, (rd(e, E_TARC0) & ~(0xFu << 27)) | (1u << 26));
        wr(e, E_CTRL, rd(e, E_CTRL) & ~(1u << 29));
        wr(e, E_CTRL_EXT, (rd(e, E_CTRL_EXT) & ~(1u << 23)) | (1u << 22));
        wr(e, E_GCR, rd(e, E_GCR) | (1u << 22) | GCR_L1_ACT_WITHOUT_L0S_RX);
        wr(e, E_GCR2, rd(e, E_GCR2) | 1);
    }
    /* DMA must snoop the CPU caches: the rings live in ordinary cached memory */
    if (e->type != EM_8254X) wr(e, E_GCR, rd(e, E_GCR) & ~GCR_NO_SNOOP_ALL);
    /* descriptor write-back one at a time (and full prefetch on the PCH) */
    if (e->type == EM_PCH_LPT) {
        for (int q = 0; q < 2; q++) {
            uint32_t reg = q ? E_TXDCTL1 : E_TXDCTL0, t = rd(e, reg);
            t = (t & ~TXDCTL_WTHRESH) | TXDCTL_FULL_WB;
            t = (t & ~TXDCTL_PTHRESH) | TXDCTL_PREFETCH;
            wr(e, reg, t);
        }
    } else if (e->type == EM_82574) {
        wr(e, E_TXDCTL0, (rd(e, E_TXDCTL0) & ~TXDCTL_WTHRESH) | TXDCTL_FULL_WB | TXDCTL_COUNT_DESC);
    }
}

/* make sure the PHY is powered and negotiating; leaves an established link alone */
static void em_phy_setup(struct em *e) {
    if (!swflag_acquire(e)) return;
    if (e->type == EM_PCH_LPT) {
        /* full speed in D0: no low-power link-up, gigabit allowed */
        uint32_t pc = rd(e, E_PHY_CTRL);
        if (pc & (PHY_CTRL_D0A_LPLU | PHY_CTRL_GBE_DISABLE)) wr(e, E_PHY_CTRL, pc & ~(PHY_CTRL_D0A_LPLU | PHY_CTRL_GBE_DISABLE));
        /* where software owns the PHY's OEM bits, mirror that into the PHY (e1000_oem_bits_config_ich8lan) */
        uint16_t oem;
        if ((rd(e, E_FEXTNVM) & FEXTNVM_SW_CONFIG) && phy_read(e, HV_OEM_BITS, &oem) && (oem & (HV_OEM_LPLU | HV_OEM_GBE_DIS))) {
            oem &= ~(HV_OEM_LPLU | HV_OEM_GBE_DIS);
            if (rd(e, E_FWSM) & FWSM_RSPCIPHY) oem |= HV_OEM_RESTART_AN;   /* unless the ME blocks PHY resets */
            phy_write(e, HV_OEM_BITS, oem);
        }
    }
    uint16_t bmcr;
    if (!(rd(e, E_STATUS) & STATUS_LU) && phy_read(e, MII_BMCR, &bmcr) && ((bmcr & BMCR_PDOWN) || !(bmcr & BMCR_ANENABLE))) {
        kprintf("em: PHY was %s, restarting auto-negotiation\n", bmcr & BMCR_PDOWN ? "powered down" : "forced");
        phy_write(e, MII_BMCR, (bmcr & ~BMCR_PDOWN) | BMCR_ANENABLE | BMCR_ANRESTART);
    }
    swflag_release(e);
}

static bool em_read_mac(struct em *e, uint8_t *mac) {
    uint32_t lo = rd(e, E_RAL0), hi = rd(e, E_RAH0);
    if (!(hi & RAH_AV) || (!lo && !(hi & 0xFFFF))) return false;
    for (int i = 0; i < 4; i++) mac[i] = lo >> (8 * i);
    mac[4] = hi; mac[5] = hi >> 8;
    return !(mac[0] & 1);
}
static bool em_eeprom_mac(struct em *e, uint8_t *mac) {
    if (e->type == EM_PCH_LPT) return false;   /* the NVM sits in the shared SPI flash; the hardware loads RAL0 from it */
    uint32_t done = e->type == EM_82574 ? 2 : 0x10; int shift = e->type == EM_82574 ? 2 : 8;
    for (int w = 0; w < 3; w++) {
        wr(e, E_EERD, 1 | ((uint32_t)w << shift));
        uint32_t v = 0; int i;
        for (i = 0; i < 10000; i++) { v = rd(e, E_EERD); if (v & done) break; udelay(5); }
        if (i == 10000) return false;
        mac[w * 2] = v >> 16; mac[w * 2 + 1] = v >> 24;
    }
    return !(mac[0] & 1) && (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]);
}

static void em_report_link(struct em *e, bool always) {
    uint32_t s = rd(e, E_STATUS);
    bool up = s & STATUS_LU;
    if (!always && up == e->link_up) return;
    e->link_up = up;
    /* I217 packet loss: Linux re-applies the 8 us beacon duration on every link check */
    if (e->type == EM_PCH_LPT) wr(e, E_FEXTNVM4, (rd(e, E_FEXTNVM4) & ~FEXTNVM4_BEACON_DURATION_MASK) | FEXTNVM4_BEACON_DURATION_8US);
    static const int speeds[] = { 10, 100, 1000, 1000 };
    if (up) kprintf("%s: link up, %d Mbit/s, %s duplex\n", e->netif.name, speeds[(s >> 6) & 3], s & STATUS_FD ? "full" : "half");
    else kprintf("%s: link down\n", e->netif.name);
}

static void em_setup_rings(struct em *e) {
    for (unsigned i = 0; i < NRX; i++) {
        e->rx[i].addr = e->rx_buf_phys + (uint64_t)i * BUFSZ;
        e->rx[i].status = 0;
    }
    for (unsigned i = 0; i < NTX; i++) {
        memset(&e->tx[i], 0, sizeof e->tx[i]);
        e->tx[i].status = TXD_DD;   /* free */
    }
    uint64_t rx_phys = V2P(e->rx), tx_phys = V2P(e->tx);
    wr(e, E_RDBAL, (uint32_t)rx_phys); wr(e, E_RDBAH, (uint32_t)(rx_phys >> 32));
    wr(e, E_RDLEN, NRX * sizeof(struct rx_desc));
    wr(e, E_RDH, 0); wr(e, E_RDT, 0);
    wr(e, E_RDTR, 0); wr(e, E_RADV, 0);
    wr(e, E_TDBAL, (uint32_t)tx_phys); wr(e, E_TDBAH, (uint32_t)(tx_phys >> 32));
    wr(e, E_TDLEN, NTX * sizeof(struct tx_desc));
    wr(e, E_TDH, 0); wr(e, E_TDT, 0);
    wr(e, E_TIDV, 0);
    e->rx_next = 0; e->tx_tail = 0; e->rx_discard = false;
    wr(e, E_TIPG, TIPG_COPPER);
    wr(e, E_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD | TCTL_RTLC);
    wr(e, E_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);   /* 2048-byte buffers, unicast to us and broadcast */
    wr(e, E_RDT, NRX - 1);                           /* hand the hardware every buffer but one */
}

/* ---- data path (called with net_lock held) ---- */
static int em_send(struct netif *n, const void *frame, size_t len) {
    struct em *e = n->priv;
    if (len > ETH_FRAME_MAX) { n->tx_errors++; return -EINVAL; }
    volatile struct tx_desc *d = &e->tx[e->tx_tail];
    for (int i = 0; i < 200000 && !(d->status & TXD_DD); i++) pause();
    if (!(d->status & TXD_DD)) { n->tx_errors++; return -EBUSY; }
    memcpy(e->tx_buf + (size_t)e->tx_tail * BUFSZ, frame, len);
    d->addr = e->tx_buf_phys + (uint64_t)e->tx_tail * BUFSZ;
    d->len = (uint16_t)len;
    d->cso = 0; d->css = 0; d->special = 0;
    d->cmd = TXD_EOP | TXD_IFCS | TXD_RS;
    d->status = 0;
    e->tx_tail = (e->tx_tail + 1) % NTX;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    wr(e, E_TDT, e->tx_tail);
    n->tx_packets++; n->tx_bytes += len;
    return 0;
}

static int em_poll(struct netif *n) {
    struct em *e = n->priv;
    uint32_t icr = e->irq ? __atomic_exchange_n(&e->icr_pending, 0, __ATOMIC_ACQ_REL) : rd(e, E_ICR);
    if (icr & ICR_LSC) em_report_link(e, false);
    if (icr & ICR_RXO) n->rx_dropped++;
    int count = 0;
    int last = -1;
    for (int budget = 0; budget < 64; budget++) {
        volatile struct rx_desc *d = &e->rx[e->rx_next];
        uint8_t status = d->status;
        if (!(status & RXD_DD)) break;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        uint16_t len = d->len; uint8_t errors = d->errors;
        if (!(status & RXD_EOP)) { e->rx_discard = true; n->rx_errors++; }   /* longer than a buffer: never with LPE off */
        else if (e->rx_discard) e->rx_discard = false;
        else if ((errors & RXE_FRAME) || len < 14 || len > ETH_FRAME_MAX + 4) n->rx_errors++;
        else {
            n->rx_packets++; n->rx_bytes += len;
            eth_input(n, e->rx_buf + (size_t)e->rx_next * BUFSZ, len);
            count++;
        }
        d->status = 0;
        last = e->rx_next;
        e->rx_next = (e->rx_next + 1) % NRX;
    }
    if (last >= 0) { __atomic_thread_fence(__ATOMIC_SEQ_CST); wr(e, E_RDT, last); }
    return count;
}

static void em_irq(struct regs *r, void *ctx) {
    struct em *e = ctx;
    uint32_t icr = rd(e, E_ICR);    /* reading clears it and drops the line */
    if (!icr) return;               /* shared line, not us */
    __atomic_fetch_or(&e->icr_pending, icr, __ATOMIC_RELEASE);
    e->irqs++;
    net_kick();
}

/* ---- probe ---- */
static const struct { uint16_t device; enum em_type type; const char *name; } em_ids[] = {
    { 0x100E, EM_8254X,   "Intel 82540EM" },        /* QEMU -device e1000 */
    { 0x100F, EM_8254X,   "Intel 82545EM" },
    { 0x10D3, EM_82574,   "Intel 82574L" },         /* QEMU -device e1000e */
    { 0x10F6, EM_82574,   "Intel 82574LA" },
    { 0x150C, EM_82574,   "Intel 82583V" },
    { 0x153A, EM_PCH_LPT, "Intel I217-LM" },
    { 0x153B, EM_PCH_LPT, "Intel I217-V" },
    { 0x15A0, EM_PCH_LPT, "Intel I218-LM" },
    { 0x15A1, EM_PCH_LPT, "Intel I218-V" },
};

static void em_attach(struct pci_device *pd, enum em_type type, const char *model) {
    if (!pd->bar_is_mmio[0] || !pd->bar_addr[0]) { kprintf("em: %s at %02x:%02x.%d: register BAR not assigned\n", model, pd->bus, pd->slot, pd->func); return; }
    struct em *e = kzalloc(sizeof *e);
    if (!e) return;
    e->pd = pd; e->type = type;
    e->phy_addr = type == EM_PCH_LPT ? 2 : 1;
    pci_enable_bus_master(pd);
    e->regs = vmm_map_mmio(pd->bar_addr[0], 0x20000);
    struct netif *n = &e->netif;
    snprintf(n->name, sizeof n->name, "em%d", ncards);
    if (rd(e, E_STATUS) == 0xFFFFFFFF) { kprintf("em: %s: registers read all ones\n", model); kfree(e); return; }

    /* the station address the firmware/NVM loaded, read before the reset in case it does not come back */
    uint8_t mac[ETH_ALEN];
    bool have_mac = em_read_mac(e, mac);
    if (type == EM_PCH_LPT) pch_phy_init(e);
    em_reset(e);
    if (type != EM_PCH_LPT) phy_id_ok(e);
    if (!have_mac) have_mac = em_read_mac(e, mac) || em_eeprom_mac(e, mac);
    if (!have_mac) {
        /* locally administered, derived from the TSC */
        uint64_t t = rdtsc() ^ ((uint64_t)pd->bus << 40) ^ ((uint64_t)pd->slot << 32);
        mac[0] = 0x02; for (int i = 1; i < 6; i++) mac[i] = t >> (8 * i);
        kprintf("%s: no station address in RAL0 or the EEPROM; using a random one\n", n->name);
    }
    memcpy(n->mac, mac, ETH_ALEN);
    wr(e, E_RAL0, mac[0] | mac[1] << 8 | mac[2] << 16 | (uint32_t)mac[3] << 24);
    wr(e, E_RAH0, mac[4] | mac[5] << 8 | RAH_AV);
    for (int i = 0; i < 128; i++) wr(e, E_MTA + i * 4, 0);

    em_hw_bits(e);
    uint32_t ctrl = rd(e, E_CTRL);
    ctrl |= CTRL_SLU | CTRL_ASDE;
    ctrl &= ~(CTRL_LRST | CTRL_PHY_RST | CTRL_ILOS | CTRL_FRCSPD | CTRL_FRCDPX | CTRL_VME);
    wr(e, E_CTRL, ctrl);
    em_phy_setup(e);

    /* rings and buffers: page-aligned, physically contiguous, cache-coherent DMA */
    uint64_t ring_phys = pmm_alloc_pages(2);
    e->rx_buf_phys = pmm_alloc_pages(NRX * BUFSZ / PAGE_SIZE);
    e->tx_buf_phys = pmm_alloc_pages(NTX * BUFSZ / PAGE_SIZE);
    if (!ring_phys || !e->rx_buf_phys || !e->tx_buf_phys) { kprintf("%s: out of memory for DMA rings\n", n->name); return; }
    e->rx = P2V(ring_phys);
    e->tx = P2V(ring_phys + PAGE_SIZE);
    e->rx_buf = P2V(e->rx_buf_phys);
    e->tx_buf = P2V(e->tx_buf_phys);
    em_setup_rings(e);

    char how[64] = "no interrupt line";
    e->irq = pci_irq_install(pd, em_irq, e, true, how, sizeof how);
    wr(e, E_ITR, 195);                 /* at most ~20000 interrupts per second */
    (void)rd(e, E_ICR);
    if (e->irq) wr(e, E_IMS, IMS_BITS);

    strlcpy(n->driver, model, sizeof n->driver);
    n->send = em_send;
    n->poll = em_poll;
    n->irq_driven = e->irq;
    n->priv = e;
    n->up = true;
    n->rx_window = 0;                  /* 255 receive buffers: no need to hold TCP back */
    ncards++;
    char ms[18]; mac_to_str(n->mac, ms);
    kprintf("%s: %s at %02x:%02x.%d, MAC %s, PHY %08x, %s%s\n", n->name, model, pd->bus, pd->slot, pd->func, ms, e->phy_id, e->irq ? "" : "polled: ", how);
    if (type == EM_PCH_LPT) kprintf("%s: management firmware %s\n", n->name, rd(e, E_FWSM) & FWSM_FW_VALID ? "present" : "absent");
    em_report_link(e, true);
    net_register(n);
}

void e1000_init(void) {
    for (int i = 0; i < pci_device_count(); i++) {
        struct pci_device *d = pci_device_at(i);
        if (d->vendor != 0x8086 || d->class_code != 0x02) continue;
        for (size_t k = 0; k < ARRAY_LEN(em_ids); k++)
            if (d->device == em_ids[k].device) em_attach(d, em_ids[k].type, em_ids[k].name);
    }
}
