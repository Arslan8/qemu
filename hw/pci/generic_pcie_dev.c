/*
 * Generic PCIe Device Implementation
 *
 * Copyright (c) 2026
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/pci/pci.h"
#include "hw/pci/pcie.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
#include "qemu/error-report.h"
#include "hw/pci/generic_pcie_dev.h"
#include "system/memory.h"
#include "system/dma.h"

/* List of pre-boot registered PCIe device descriptions */
#define MAX_PREBOOT_PCIE_DEVS 32
static QEMUPCIeDeviceDesc registered_descs[MAX_PREBOOT_PCIE_DEVS];
static int num_registered_descs = 0;

void qemu_register_generic_pcie_device_desc(const QEMUPCIeDeviceDesc *desc)
{
    if (num_registered_descs >= MAX_PREBOOT_PCIE_DEVS) {
        error_report("Exceeded maximum allowed pre-boot generic PCIe devices (%d)", MAX_PREBOOT_PCIE_DEVS);
        return;
    }
    registered_descs[num_registered_descs++] = *desc;
    printf("Registered Generic PCIe Device Description: '%s' (Vendor: 0x%04X, Device: 0x%04X, Class: 0x%06X)\n",
           desc->name, desc->vendor_id, desc->device_id, desc->class_code);
}

/* BAR MemoryRegion Accessors */
static uint64_t generic_pcie_bar_read(void *opaque, hwaddr addr, unsigned size)
{
    GenericPCIEBarState *bar_state = opaque;
    GenericPCIEDev *s = bar_state->dev;
    MemoryRegion *mr = &s->bar_mr[bar_state->bar_idx];

    /* Forward to plugin using system/memory.c exporter mechanism */
    uint64_t val = 0;
    if (!qemu_plugin_unimp_read(mr->name, addr, &val, size)) {
        val = 0;
    }
    return val;
}

static void generic_pcie_bar_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    GenericPCIEBarState *bar_state = opaque;
    GenericPCIEDev *s = bar_state->dev;
    MemoryRegion *mr = &s->bar_mr[bar_state->bar_idx];

    /* Forward to plugin using system/memory.c exporter mechanism */
    qemu_plugin_unimp_write(mr->name, addr, val, size);
}

static const MemoryRegionOps generic_pcie_bar_ops = {
    .read = generic_pcie_bar_read,
    .write = generic_pcie_bar_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static void generic_pcie_dev_realize(PCIDevice *pci_dev, Error **errp)
{
    GenericPCIEDev *s = GENERIC_PCIE_DEV(pci_dev);

    /* 1. Set PCI IDs and Class Code */
    pci_config_set_vendor_id(pci_dev->config, s->vendor_id);
    pci_config_set_device_id(pci_dev->config, s->device_id);
    pci_config_set_revision(pci_dev->config, s->revision_id);
    pci_config_set_class(pci_dev->config, s->class_code >> 8);
    pci_config_set_prog_interface(pci_dev->config, s->class_code & 0xFF);

    pci_set_word(pci_dev->config + PCI_SUBSYSTEM_VENDOR_ID,
                 s->subsys_vendor_id ? s->subsys_vendor_id : s->vendor_id);
    pci_set_word(pci_dev->config + PCI_SUBSYSTEM_ID,
                 s->subsys_id ? s->subsys_id : s->device_id);

    /* 2. Legacy Interrupt Pin */
    if (s->int_pin > 0 && s->int_pin <= 4) {
        pci_dev->config[PCI_INTERRUPT_PIN] = s->int_pin;
    }

    /* 3. PCIe Endpoint Capability */
    pcie_endpoint_cap_init(pci_dev, 0);

    /* 4. MSI / MSI-X Capabilities */
    if (s->msi_vectors > 0) {
        msi_init(pci_dev, 0, s->msi_vectors, true, false, errp);
    }
    if (s->msix_entries > 0) {
        msix_init_exclusive_bar(pci_dev, s->msix_entries, s->msix_bar, errp);
    }

    /* 5. Register BARs */
    for (int i = 0; i < GENERIC_PCIE_MAX_BARS; i++) {
        if (s->bar_size[i] == 0) {
            continue;
        }

        uint8_t attr = 0;
        switch (s->bar_type[i]) {
        case 0: /* MMIO 32-bit non-prefetchable */
            attr = PCI_BASE_ADDRESS_SPACE_MEMORY;
            break;
        case 1: /* MMIO 64-bit non-prefetchable */
            attr = PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64;
            break;
        case 2: /* MMIO 32-bit prefetchable */
            attr = PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_PREFETCH;
            break;
        case 3: /* MMIO 64-bit prefetchable */
            attr = PCI_BASE_ADDRESS_SPACE_MEMORY | PCI_BASE_ADDRESS_MEM_TYPE_64 | PCI_BASE_ADDRESS_MEM_PREFETCH;
            break;
        case 4: /* Port I/O */
            attr = PCI_BASE_ADDRESS_SPACE_IO;
            break;
        default:
            attr = PCI_BASE_ADDRESS_SPACE_MEMORY;
            break;
        }

        snprintf(s->bar_name[i], sizeof(s->bar_name[i]), "generic_pcie_%s_bar%d",
                 s->name ? s->name : "dev", i);

        s->bar_state[i].dev = s;
        s->bar_state[i].bar_idx = i;

        memory_region_init_io(&s->bar_mr[i], OBJECT(s), &generic_pcie_bar_ops,
                              &s->bar_state[i], s->bar_name[i], s->bar_size[i]);

        pci_register_bar(pci_dev, i, attr, &s->bar_mr[i]);
    }
}

static void generic_pcie_dev_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = generic_pcie_dev_realize;
    k->vendor_id = 0x1B36;
    k->device_id = 0x0001;
    k->revision = 0x01;
    k->class_id = PCI_CLASS_OTHERS;

    dc->desc = "Generic PCIe Device for Plugin Instrumentation";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo generic_pcie_dev_info = {
    .name          = TYPE_GENERIC_PCIE_DEV,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(GenericPCIEDev),
    .class_init    = generic_pcie_dev_class_init,
    .interfaces    = (InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { }
    },
};

static void generic_pcie_dev_register_types(void)
{
    type_register_static(&generic_pcie_dev_info);
}

type_init(generic_pcie_dev_register_types)

void qemu_instantiate_generic_pcie_devices(PCIBus *bus)
{
    for (int i = 0; i < num_registered_descs; i++) {
        QEMUPCIeDeviceDesc *d = &registered_descs[i];
        PCIDevice *pci_dev = pci_new(-1, TYPE_GENERIC_PCIE_DEV);
        GenericPCIEDev *s = GENERIC_PCIE_DEV(pci_dev);

        s->name = g_strdup(d->name);
        s->vendor_id = d->vendor_id;
        s->device_id = d->device_id;
        s->revision_id = d->revision_id;
        s->class_code = d->class_code;
        s->subsys_vendor_id = d->subsys_vendor_id;
        s->subsys_id = d->subsys_id;
        s->int_pin = d->int_pin;
        s->msi_vectors = d->msi_vectors;
        s->msix_entries = d->msix_entries;
        s->msix_bar = d->msix_bar;

        for (int b = 0; b < GENERIC_PCIE_MAX_BARS; b++) {
            s->bar_size[b] = d->bars[b].size;
            s->bar_type[b] = d->bars[b].type;
        }

        pci_realize_and_unref(pci_dev, bus, &error_fatal);
        printf("Generic PCIe Device '%s' instantiated on PCIe root bus (Vendor: 0x%04X, Device: 0x%04X)\n",
               d->name, d->vendor_id, d->device_id);
    }
}

void generic_pcie_dev_raise_irq(GenericPCIEDev *s, int level)
{
    pci_set_irq(&s->pci_dev, level);
}

void generic_pcie_dev_send_msi(GenericPCIEDev *s, int vector)
{
    msi_notify(&s->pci_dev, vector);
}

void generic_pcie_dev_send_msix(GenericPCIEDev *s, int vector)
{
    msix_notify(&s->pci_dev, vector);
}

int generic_pcie_dev_dma_read(GenericPCIEDev *s, dma_addr_t addr, void *buf, dma_addr_t len)
{
    return pci_dma_read(&s->pci_dev, addr, buf, len);
}

int generic_pcie_dev_dma_write(GenericPCIEDev *s, dma_addr_t addr, const void *buf, dma_addr_t len)
{
    return pci_dma_write(&s->pci_dev, addr, buf, len);
}
