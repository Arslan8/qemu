/*
 * Generic x64 Machine Implementation
 *
 * Copyright (c) 2026
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "hw/i386/x86.h"
#include "hw/qdev-properties.h"
#include "hw/qdev-clock.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "hw/misc/unimp.h"
#include "system/address-spaces.h"
#include "system/reset.h"
#include "target/i386/cpu.h"
#include "hw/loader.h"
#include "elf.h"
#include "hw/isa/isa.h"
#include "hw/intc/i8259.h"
#include "hw/timer/i8254.h"
#include "hw/rtc/mc146818rtc.h"
#include "hw/i386/e820_memory_layout.h"
#include "hw/i386/fw_cfg.h"
#include "hw/nvram/fw_cfg.h"

typedef struct {
    uint64_t entry;
} GenericX64ResetState;

static GenericX64ResetState generic_reset_info;

static void generic_x64_cpu_reset(void *opaque)
{
    GenericX64ResetState *s = opaque;
    CPUState *cs;
    CPU_FOREACH(cs) {
        X86CPU *cpu = X86_CPU(cs);
        CPUX86State *env = &cpu->env;
        env->eip = s->entry;
        /* Real Mode CS selector 0x0000 -> base 0x00000000 */
        cpu_x86_load_seg_cache(env, R_CS, 0x0000, 0x00000000, 0xffff,
                               DESC_P_MASK | DESC_S_MASK | DESC_CS_MASK |
                               DESC_R_MASK | DESC_A_MASK);
    }
}

static void create_unimplemented_io_device(const char *name, hwaddr base, hwaddr size)
{
    DeviceState *dev = qdev_new(TYPE_UNIMPLEMENTED_DEVICE);
    qdev_prop_set_string(dev, "name", name);
    qdev_prop_set_uint64(dev, "size", size);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    MemoryRegion *mr = sysbus_mmio_get_region(SYS_BUS_DEVICE(dev), 0);
    memory_region_add_subregion_overlap(get_system_io(), base, mr, -1000);
}

static void generic_x64_set_cmos(ISABus *isa_bus, ram_addr_t ram_size)
{
    MC146818RtcState *s = mc146818_rtc_init(isa_bus, 2000, NULL);
    int val;

    /* Base memory size (up to 640KB) */
    val = MIN(ram_size / KiB, 640);
    mc146818rtc_set_cmos_data(s, 0x15, val);
    mc146818rtc_set_cmos_data(s, 0x16, val >> 8);

    /* Extended memory (1MB to 64MB) */
    if (ram_size > 1 * MiB) {
        val = (ram_size - 1 * MiB) / KiB;
    } else {
        val = 0;
    }
    if (val > 65535) {
        val = 65535;
    }
    mc146818rtc_set_cmos_data(s, 0x17, val);
    mc146818rtc_set_cmos_data(s, 0x18, val >> 8);
    mc146818rtc_set_cmos_data(s, 0x30, val);
    mc146818rtc_set_cmos_data(s, 0x31, val >> 8);

    /* Memory between 16MB and 4GB */
    if (ram_size > 16 * MiB) {
        val = (ram_size - 16 * MiB) / (64 * KiB);
    } else {
        val = 0;
    }
    if (val > 65535) {
        val = 65535;
    }
    mc146818rtc_set_cmos_data(s, 0x34, val);
    mc146818rtc_set_cmos_data(s, 0x35, val >> 8);
}

static void generic_x64_init(MachineState *machine)
{
    X86MachineState *x86ms = X86_MACHINE(machine);

    if (!machine->cpu_type) {
        machine->cpu_type = TARGET_DEFAULT_CPU_TYPE;
    }

    printf("Generic x64 Machine: CPU type: %s\n", machine->cpu_type);

    x86_cpus_init(x86ms, CPU_VERSION_LATEST);

    /* 1. Map system RAM at priority 1 to cleanly override generic_io_64 */
    if (machine->ram) {
        memory_region_add_subregion_overlap(get_system_memory(), 0x0, machine->ram, 1);
        e820_add_entry(0, machine->ram_size, E820_RAM);
    }

    /* 2. Unimplemented MMIO fallback region at background priority -1000 across full address space */
    create_unimplemented_device("generic_io_64", 0x0, 0xFFFFFFFFFFFFFFFFULL);

    /* Initialize ISA Bus for Port I/O space (0x0 to 0xFFFF) */
    ISABus *isa_bus = isa_bus_new(NULL, get_system_memory(), get_system_io(), &error_abort);

    /* Initialize i8259 PIC and register ISA input IRQs */
    qemu_irq *i8259 = i8259_init(isa_bus, x86_allocate_cpu_irq());
    isa_bus_register_input_irqs(isa_bus, i8259);

    /* Initialize i8254 PIT timer */
    i8254_pit_init(isa_bus, 0x40, 0, NULL);

    /* Initialize CMOS RTC so SeaBIOS reads valid memory limits */
    generic_x64_set_cmos(isa_bus, machine->ram_size);

    /* Initialize FW_CFG for SeaBIOS RAM map and CPU topology */
    FWCfgState *fw_cfg = fw_cfg_init_io_dma(FW_CFG_IO_BASE, FW_CFG_IO_BASE + 4, &address_space_memory);
    fw_cfg_add_i16(fw_cfg, FW_CFG_NB_CPUS, machine->smp.cpus);
    fw_cfg_add_i16(fw_cfg, FW_CFG_MAX_CPUS, machine->smp.max_cpus);
    fw_cfg_add_i64(fw_cfg, FW_CFG_RAM_SIZE, (uint64_t)machine->ram_size);
    rom_set_fw(fw_cfg);
    x86ms->fw_cfg = fw_cfg;

    /* Unimplemented Port I/O fallback region so unhandled in/out export to plugin (dev.c) */
    create_unimplemented_io_device("generic_io_port_64", 0x0, 0x10000);

    /* Initialize BIOS ROM with F-segment ISA alias (isapc_ram_fw = true) */
    x86_bios_rom_init(x86ms, "bios.bin", get_system_memory(), true);

    /* Generic image loader (ELF or raw binary) */
    if (machine->kernel_filename) {
        uint64_t entry = 0x7c00;
        /* A. Try loading as an ELF file (header specifies LMA section addresses & entry point) */
        ssize_t image_size = load_elf(machine->kernel_filename, NULL, NULL, NULL,
                                      &entry, NULL, NULL, NULL,
                                      0, EM_NONE, 1, 0);
        if (image_size < 0) {
            /* B. Fallback: Raw binary image loaded into RAM at physical address 0x7c00 */
            image_size = load_image_targphys(machine->kernel_filename, 0x7c00, 0x1000000);
            entry = 0x7c00;
        }

        if (image_size < 0) {
            error_report("Could not load image '%s'", machine->kernel_filename);
            exit(1);
        }

        printf("Generic x64 Machine: Loaded '%s' (%zd bytes) at entry 0x%" PRIx64 "\n",
               machine->kernel_filename, image_size, entry);

        generic_reset_info.entry = entry;
        qemu_register_reset(generic_x64_cpu_reset, &generic_reset_info);
    }
}

static void generic_x64_machine_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->desc = "Generic x64 Machine";
    mc->init = generic_x64_init;
    mc->max_cpus = 255;
    mc->default_cpu_type = TARGET_DEFAULT_CPU_TYPE;
    mc->default_ram_id = "pc.ram";

    /* Allow dynamic attachment of peripherals and storage controllers via -device */
    machine_class_allow_dynamic_sysbus_dev(mc, "serial");
    machine_class_allow_dynamic_sysbus_dev(mc, "hpet");
    machine_class_allow_dynamic_sysbus_dev(mc, "ahci");
    machine_class_allow_dynamic_sysbus_dev(mc, "sysbus-ahci");
}

static const TypeInfo generic_x64_machine_info = {
    .name          = MACHINE_TYPE_NAME("x64-generic"),
    .parent        = TYPE_X86_MACHINE,
    .class_init    = generic_x64_machine_init,
};

static void generic_x64_machine_register_types(void)
{
    type_register_static(&generic_x64_machine_info);
}

type_init(generic_x64_machine_register_types)
