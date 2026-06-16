/*
 * Astra SL board emulation
 *
 * Copyright (c) 2024 QEMU Contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "hw/core/boards.h"
#include "system/system.h"
#include "hw/core/qdev-properties.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "hw/char/pl011.h"
#include "hw/intc/arm_gic.h"
#include "target/arm/cpu-qom.h"
#include "system/address-spaces.h"

/* Memory map */
#define ASTRA_SL_GIC_DIST_BASE   0x08000000
#define ASTRA_SL_GIC_CPU_BASE    0x08010000
#define ASTRA_SL_UART0_BASE      0x09000000
#define ASTRA_SL_RAM_BASE        0x40000000
#define ASTRA_SL_RAM_SIZE        (2 * GiB)

/* Interrupts: SPIs start at GIC offset 32 */
#define ASTRA_SL_UART0_IRQ       32   /* SPI 0 */

#define ASTRA_SL_NUM_IRQS        64
#define ASTRA_SL_NUM_CPUS        1

static struct arm_boot_info astra_sl_binfo = {
    .loader_start = ASTRA_SL_RAM_BASE,
    .board_id     = -1, /* no board ID; rely on device tree */
};

static void astra_sl_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    DeviceState *gicdev;
    SysBusDevice *gicbusdev;
    qemu_irq pic[ASTRA_SL_NUM_IRQS];
    int i;

    /* Validate RAM size */
    if (machine->ram_size > ASTRA_SL_RAM_SIZE) {
        error_report("astra_sl: RAM size %" PRId64 " MiB exceeds maximum %d MiB",
                     machine->ram_size / MiB, (int)(ASTRA_SL_RAM_SIZE / MiB));
        exit(1);
    }

    /* Create and realize CPUs */
    for (i = 0; i < ASTRA_SL_NUM_CPUS; i++) {
        Object *cpuobj = object_new(machine->cpu_type);
        object_property_add_child(OBJECT(machine), "cpu[*]", cpuobj);
        qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);
        object_unref(cpuobj);
    }

    /* Map RAM */
    memory_region_add_subregion(sysmem, ASTRA_SL_RAM_BASE, machine->ram);

    /* GICv2 */
    gicdev = qdev_new(TYPE_ARM_GIC);
    qdev_prop_set_uint32(gicdev, "num-cpu", ASTRA_SL_NUM_CPUS);
    qdev_prop_set_uint32(gicdev, "num-irq",
                         ASTRA_SL_NUM_IRQS + GIC_INTERNAL);
    gicbusdev = SYS_BUS_DEVICE(gicdev);
    sysbus_realize_and_unref(gicbusdev, &error_fatal);
    sysbus_mmio_map(gicbusdev, 0, ASTRA_SL_GIC_DIST_BASE);
    sysbus_mmio_map(gicbusdev, 1, ASTRA_SL_GIC_CPU_BASE);

    /* Wire GIC outputs to each CPU's IRQ/FIQ inputs */
    for (i = 0; i < ASTRA_SL_NUM_CPUS; i++) {
        DeviceState *cpudev = DEVICE(qemu_get_cpu(i));
        sysbus_connect_irq(gicbusdev, i,
                           qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(gicbusdev, i + ASTRA_SL_NUM_CPUS,
                           qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
    }

    /* Collect SPI input lines from GIC */
    for (i = 0; i < ASTRA_SL_NUM_IRQS; i++) {
        pic[i] = qdev_get_gpio_in(gicdev, i);
    }

    /* PL011 UART */
    pl011_create(ASTRA_SL_UART0_BASE, pic[ASTRA_SL_UART0_IRQ],
                 serial_hd(0));

    /* Boot */
    astra_sl_binfo.ram_size = machine->ram_size;
    arm_load_kernel(ARM_CPU(first_cpu), machine, &astra_sl_binfo);
}

static void astra_sl_machine_init(MachineClass *mc)
{
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-a53"),
        NULL
    };

    mc->desc              = "Astra SL board (Cortex-A53)";
    mc->init              = astra_sl_init;
    mc->default_cpu_type  = ARM_CPU_TYPE_NAME("cortex-a53");
    mc->valid_cpu_types   = valid_cpu_types;
    mc->default_ram_size  = ASTRA_SL_RAM_SIZE;
    mc->default_ram_id    = "astra_sl.ram";
    mc->max_cpus          = ASTRA_SL_NUM_CPUS;
    mc->ignore_memory_transaction_failures = true;
}

DEFINE_MACHINE_ARM("astra_sl", astra_sl_machine_init)
