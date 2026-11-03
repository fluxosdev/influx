#pragma once
#include "acpi.h"
#include "arch.h"
#include "sched.h"
#include "fs.h"            /* errno values */

/* ---- AML objects ---- */

/* the first values are what ObjectType returns */
enum {
    AML_UNINIT = 0, AML_INTEGER, AML_STRING, AML_BUFFER, AML_PACKAGE, AML_FIELD, AML_DEVICE, AML_EVENT,
    AML_METHOD, AML_MUTEX, AML_REGION, AML_POWER, AML_PROCESSOR, AML_THERMAL, AML_BUFFER_FIELD, AML_DDB, AML_DEBUG,
    AML_REFERENCE = 0x20,     /* RefOf / Index / ArgX / LocalX target */
    AML_NAMEREF,              /* a package element naming something that did not exist yet */
};
enum { REF_NAMED, REF_LOCAL, REF_ARG, REF_INDEX, REF_DEBUG, REF_NULL };
enum { FIELD_NORMAL, FIELD_INDEX, FIELD_BANK };

/* operation region address spaces */
#define SPACE_MEMORY 0
#define SPACE_IO     1
#define SPACE_PCI    2
#define SPACE_EC     3
#define SPACE_CMOS   5
#define SPACE_NONE   0xFF   /* an unusable region (DataTableRegion without its table): every access refused */

struct aml_node;
struct aml_obj;
typedef struct aml_obj *(*aml_native_fn)(struct aml_obj **args, int argc);

struct aml_obj {
    int type, refs;
    union {
        uint64_t ival;
        struct { char *s; size_t len; } str;
        struct { uint8_t *b; size_t len; } buf;
        struct { struct aml_obj **e; size_t n; } pkg;
        struct { const uint8_t *code; size_t len; uint8_t flags; aml_native_fn native; } method;
        struct { uint8_t space; uint64_t base, len; struct aml_node *node; bool pci_resolved, limit_warned; uint16_t seg; uint8_t bus, dev, fn; } region;
        struct { int kind; uint8_t flags, attrib; uint64_t bit_off, bit_len; struct aml_node *region, *index, *data, *bank; uint64_t bank_val; } field;
        struct { struct aml_obj *buf; uint64_t bit_off, bit_len; } bfield;
        struct { uint8_t sync; int depth; } mutex;
        struct { uint8_t id; uint32_t pblk; uint8_t pblk_len; } proc;
        struct { int kind; struct aml_node *node; struct aml_obj **slot; struct aml_obj *target; uint64_t index; } ref;
        struct { char *path; struct aml_node *scope; } nameref;
    };
};

/* ---- namespace ---- */
struct aml_node {
    char name[5];
    struct aml_node *parent, *children, *next;
    struct aml_node *alias;       /* Alias(): lookups land on this node instead */
    struct aml_obj *obj;
    struct aml_node *dyn_next;    /* nodes created by a running method, removed when it returns */
};

extern struct aml_node *aml_root;
extern bool aml_int64;            /* DSDT revision >= 2: 64-bit integers */
static inline uint64_t aml_ones(void) { return aml_int64 ? ~0ULL : 0xFFFFFFFFULL; }
static inline uint64_t aml_trunc(uint64_t v) { return aml_int64 ? v : (v & 0xFFFFFFFFULL); }

/* ns.c: objects */
struct aml_obj *obj_new(int type);
struct aml_obj *obj_int(uint64_t v);
struct aml_obj *obj_str(const char *s, size_t len);
struct aml_obj *obj_buf(const void *data, size_t len);   /* data may be NULL (zeroed) */
struct aml_obj *obj_pkg(size_t n);
struct aml_obj *obj_ref_named(struct aml_node *n);
static inline struct aml_obj *obj_get(struct aml_obj *o) { if (o) o->refs++; return o; }
void obj_put(struct aml_obj *o);
struct aml_obj *obj_copy(struct aml_obj *o);              /* deep copy of data objects */
bool obj_to_int(struct aml_obj *o, uint64_t *out);        /* implicit conversion */
struct aml_obj *obj_to_buffer(struct aml_obj *o);
struct aml_obj *obj_to_string(struct aml_obj *o);         /* implicit (hex) conversion */
const char *obj_type_name(int type);

/* ns.c: namespace */
void ns_init(void);
struct aml_node *ns_child(struct aml_node *parent, const char *seg);
struct aml_node *ns_add(struct aml_node *parent, const char *seg);
void ns_remove(struct aml_node *n);
struct aml_node *ns_lookup(struct aml_node *scope, const char *path);   /* "\\_SB.PCI0", "_S5", "^^FOO"; no search */
struct aml_node *ns_lookup_search(struct aml_node *scope, const char *path);   /* AML rules: a lone segment searches upward */
static inline struct aml_node *ns_resolve_alias(struct aml_node *n) { int k = 0; while (n && n->alias && k++ < 8) n = n->alias; return n; }
void ns_path(struct aml_node *n, char *out, size_t size);
void ns_walk(struct aml_node *start, bool (*fn)(struct aml_node *n, int depth, void *ctx), void *ctx);
int ns_node_count(void);
bool ns_hid_matches(struct aml_node *dev, const char *eisa);              /* _HID or _CID */

/* aml.c: interpreter (all entry points take the interpreter lock) */
void aml_init(void);              /* empty namespace with the predefined scopes, \_OSI, \_OS, \_REV */
int aml_load_table(const void *table, size_t len, const char *what);
int aml_eval(struct aml_node *n, struct aml_obj **args, int argc, struct aml_obj **result);
int aml_eval_path(struct aml_node *scope, const char *path, struct aml_obj **args, int argc, struct aml_obj **result);
int aml_eval_int(struct aml_node *scope, const char *path, uint64_t *out);
struct aml_obj *aml_value_of(struct aml_obj *o);          /* follows references, reads fields */
void aml_lock(void);
void aml_unlock(void);
extern int aml_errors;

/* region.c: operation regions, fields, embedded controller */
uint64_t region_read(struct aml_obj *rgn, uint64_t off, int width);
void region_write(struct aml_obj *rgn, uint64_t off, int width, uint64_t v);
struct aml_obj *field_read(struct aml_obj *f);
void field_write(struct aml_obj *f, struct aml_obj *v);
struct aml_obj *bfield_read(struct aml_obj *f);
void bfield_write(struct aml_obj *f, struct aml_obj *v);
uint64_t gas_read(const struct acpi_gas *g);
void gas_write(const struct acpi_gas *g, uint64_t v);
bool ec_init(void);
int ec_gpe(void);                 /* EC's GPE number or -1 */
void ec_handle_events(void);
const char *ec_describe(void);

/* pm.c */
void acpi_notify(struct aml_node *n, uint64_t value);
bool acpi_gl_acquire(uint32_t timeout_ms);   /* FACS global lock, nests; true if held (or there is none) */
void acpi_gl_release(void);
uint64_t acpi_timer_100ns(void);
void acpi_stall_us(uint64_t us);

/* FADT */
struct acpi_fadt {
    char sig[4]; uint32_t len; uint8_t rev, csum; char oem[6]; char oemtbl[8]; uint32_t oemrev, creator, creatorrev;
    uint32_t firmware_ctrl, dsdt;
    uint8_t reserved0, pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t acpi_enable, acpi_disable, s4bios_req, pstate_cnt;
    uint32_t pm1a_evt, pm1b_evt, pm1a_cnt, pm1b_cnt, pm2_cnt, pm_tmr, gpe0, gpe1;
    uint8_t pm1_evt_len, pm1_cnt_len, pm2_cnt_len, pm_tmr_len, gpe0_len, gpe1_len, gpe1_base, cst_cnt;
    uint16_t p_lvl2_lat, p_lvl3_lat, flush_size, flush_stride;
    uint8_t duty_offset, duty_width, day_alrm, mon_alrm, century;
    uint16_t iapc_boot_arch;
    uint8_t reserved1;
    uint32_t flags;
    struct acpi_gas reset_reg;
    uint8_t reset_value;
    uint16_t arm_boot_arch;
    uint8_t minor_rev;
    uint64_t x_firmware_ctrl, x_dsdt;
    struct acpi_gas x_pm1a_evt, x_pm1b_evt, x_pm1a_cnt, x_pm1b_cnt, x_pm2_cnt, x_pm_tmr, x_gpe0, x_gpe1;
    struct acpi_gas sleep_control, sleep_status;
} PACKED;
#define FADT_PWR_BUTTON    (1u << 4)    /* set: power button is a control-method device */
#define FADT_SLP_BUTTON    (1u << 5)
#define FADT_TMR_VAL_EXT   (1u << 8)    /* PM timer counts 32 bits (else 24) */
#define FADT_RESET_REG_SUP (1u << 10)
#define FADT_HW_REDUCED    (1u << 20)

/* arch/acpi.c */
#define ACPI_MAX_TABLE (16u << 20)     /* sanity bound on a table's length */
#define MAX_IOAPICS 8
struct ioapic_info { uint64_t addr; uint32_t gsi_base; };
extern struct ioapic_info ioapics[MAX_IOAPICS];   /* from the MADT; madt.ioapic_* is the first */
extern int nioapics;
struct acpi_fadt *acpi_fadt(void);
void *acpi_table_at(int i, char sig[4]);       /* i-th table in the RSDT/XSDT, NULL past the end */
void *acpi_map_phys(uint64_t phys, size_t len);
void *acpi_ecam(uint16_t seg, uint8_t bus);    /* the bus's MCFG/ECAM window (1 MiB, mapped on first use) or NULL */

/* arch/pci.c (not in arch.h) */
uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
void pci_write8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint8_t v);
bool pci_irq_share_gsi(uint32_t gsi, bool level, bool active_low, irq_handler_t h, void *ctx);   /* the SCI joins the INTx dispatchers */
