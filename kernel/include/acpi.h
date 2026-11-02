#pragma once
#include "kernel.h"

/*
 * ACPI beyond the static tables: the AML interpreter and namespace
 * (acpi/aml.c, acpi/ns.c), operation regions and the embedded controller
 * (acpi/region.c), power management -- ACPI mode, SCI, GPEs, power button,
 * S5 and reset (acpi/pm.c) -- and PCI interrupt routing through _PRT
 * (acpi/irq.c). The table parsing that runs at early boot is in arch/acpi.c.
 */

/* Generic Address Structure */
struct acpi_gas { uint8_t space, bit_width, bit_offset, access_size; uint64_t address; } PACKED;

void acpi_namespace_init(void);     /* load DSDT + SSDTs, enter ACPI mode, run _STA/_INI, start the SCI */
bool acpi_ready(void);              /* namespace loaded */
void acpi_shutdown(void);           /* enter S5; returns only if the firmware ignored it */
void acpi_reboot(void);             /* FADT reset register; returns if unsupported */
void acpi_set_power_button_handler(void (*fn)(void));

/* PCI INTx routing (pin 1..4 = INTA..INTD) */
struct acpi_irq_route { uint32_t gsi; bool level, active_low; };
bool acpi_pci_route(uint8_t bus, uint8_t slot, uint8_t func, uint8_t pin, struct acpi_irq_route *out);

/* shell helpers: output goes through a line callback */
typedef void (*acpi_print_fn)(void *ctx, const char *s);
void acpi_print_info(acpi_print_fn out, void *ctx);
int acpi_print_tree(const char *path, acpi_print_fn out, void *ctx);
int acpi_print_eval(const char *path, int argc, const uint64_t *args, acpi_print_fn out, void *ctx);
void acpi_print_irqs(acpi_print_fn out, void *ctx);
