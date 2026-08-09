/*
 * Generic PCIe Device Interface for Plugin Integration
 *
 * Copyright (c) 2026
 */

#ifndef GENERIC_PCIE_DEV_H
#define GENERIC_PCIE_DEV_H

#include "hw/pci/pci.h"
#include "hw/pci/pcie.h"

#define TYPE_GENERIC_PCIE_DEV "generic-pcie-dev"
OBJECT_DECLARE_SIMPLE_TYPE(GenericPCIEDev, GENERIC_PCIE_DEV)

#define GENERIC_PCIE_MAX_BARS 6

/* BAR Description */
typedef struct QEMUPCIeBarDesc {
    uint64_t size;       /* Size in bytes (must be power of 2, 0 if unused) */
    uint8_t type;        /* 0=MMIO32, 1=MMIO64, 2=MMIO32_PREFETCH, 3=MMIO64_PREFETCH, 4=PIO */
} QEMUPCIeBarDesc;

/* PCIe Device Description provided by plugin or CLI before boot */
typedef struct QEMUPCIeDeviceDesc {
    char name[64];              /* Device instance name, e.g. "nvme0", "pcie_dev0" */
    uint16_t vendor_id;         /* PCI Vendor ID */
    uint16_t device_id;         /* PCI Device ID */
    uint8_t revision_id;        /* PCI Revision ID */
    uint32_t class_code;        /* PCI Class Code (e.g. 0x010802 for NVMe Controller) */
    uint16_t subsys_vendor_id;  /* PCI Subsystem Vendor ID */
    uint16_t subsys_id;         /* PCI Subsystem ID */
    uint8_t int_pin;            /* Legacy interrupt pin: 0=None, 1=INTA, 2=INTB, 3=INTC, 4=INTD */
    uint8_t msi_vectors;        /* Number of MSI vectors (0 = no MSI, 1..32) */
    uint16_t msix_entries;      /* Number of MSI-X entries (0 = no MSI-X) */
    uint8_t msix_bar;           /* BAR index to place MSI-X table/PBA (e.g. 4 or 5) */
    QEMUPCIeBarDesc bars[GENERIC_PCIE_MAX_BARS];
} QEMUPCIeDeviceDesc;

typedef struct GenericPCIEBarState {
    GenericPCIEDev *dev;
    int bar_idx;
} GenericPCIEBarState;

struct GenericPCIEDev {
    PCIDevice pci_dev;

    /* Device Properties / Configuration */
    char *name;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t revision_id;
    uint32_t class_code;
    uint16_t subsys_vendor_id;
    uint16_t subsys_id;
    uint8_t int_pin;
    uint8_t msi_vectors;
    uint16_t msix_entries;
    uint8_t msix_bar;

    /* BAR Config */
    uint64_t bar_size[GENERIC_PCIE_MAX_BARS];
    uint8_t bar_type[GENERIC_PCIE_MAX_BARS];

    /* BAR Memory Regions */
    MemoryRegion bar_mr[GENERIC_PCIE_MAX_BARS];
    GenericPCIEBarState bar_state[GENERIC_PCIE_MAX_BARS];
    char bar_name[GENERIC_PCIE_MAX_BARS][64];
};

/* C API to register device descriptions from plugin before boot */
void qemu_register_generic_pcie_device_desc(const QEMUPCIeDeviceDesc *desc);

/* C API to instantiate all registered generic PCIe devices onto a PCI/PCIe bus */
void qemu_instantiate_generic_pcie_devices(PCIBus *bus);

/* Helpers for DMA & Interrupts from plugin or QEMU side */
void generic_pcie_dev_raise_irq(GenericPCIEDev *s, int level);
void generic_pcie_dev_send_msi(GenericPCIEDev *s, int vector);
void generic_pcie_dev_send_msix(GenericPCIEDev *s, int vector);
int generic_pcie_dev_dma_read(GenericPCIEDev *s, dma_addr_t addr, void *buf, dma_addr_t len);
int generic_pcie_dev_dma_write(GenericPCIEDev *s, dma_addr_t addr, const void *buf, dma_addr_t len);

/* Exporter helpers for BAR MemoryRegion forwarding */
bool qemu_plugin_unimp_read(const char *name, hwaddr addr, uint64_t *val, unsigned size);
bool qemu_plugin_unimp_write(const char *name, hwaddr addr, uint64_t val, unsigned size);

#endif /* GENERIC_PCIE_DEV_H */
