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
#include "system/blockdev.h"
#include "system/device_tree.h"
#include "hw/core/boards.h"
#include "system/system.h"
#include "hw/core/qdev-properties.h"
#include "hw/arm/boot.h"
#include "hw/arm/bsa.h"
#include "hw/arm/fdt.h"
#include "hw/arm/machines-qom.h"
#include "hw/char/serial-mm.h"
#include "hw/intc/arm_gic.h"
#include "hw/misc/torq.h"
#include "hw/sd/sd.h"
#include "hw/sd/sdhci.h"
#include "target/arm/cpu-qom.h"
#include "system/address-spaces.h"

/* Memory map */
#define ASTRA_SL_GIC_DIST_BASE   0xf7e59000
#define ASTRA_SL_GIC_CPU_BASE    0xf7e5a000
#define ASTRA_SL_GIC_HYP_BASE    0xf7e5c000
#define ASTRA_SL_GIC_VCPU_BASE   0xf7e5e000
#define ASTRA_SL_UART0_BASE      0xe5030000
#define ASTRA_SL_DEV_BASE        0xf7600000
#define ASTRA_SL_DEV_SIZE        TORQ_MMIO_SIZE
#define ASTRA_SL_EMMC_BASE       0xf7a00000
#define ASTRA_SL_EMMC_SIZE       0x00001000
#define ASTRA_SL_RAM_BASE        0x40000000
#define ASTRA_SL_RAM_SIZE        (2 * GiB)

/* SPI input line indices (matching sl2611.dtsi) */
#define ASTRA_SL_UART0_SPI       130  /* uart0 SPI ID 130 */
#define ASTRA_SL_DEV_JOB_SPI     52   /* TORQ job SPI ID 52 */
#define ASTRA_SL_DEV_FAULT_SPI   207  /* TORQ fault SPI ID 207 */
#define ASTRA_SL_EMMC_SPI        13   /* sdhci0 SPI ID 13 */

#define ASTRA_SL_NUM_IRQS        256
#define ASTRA_SL_NUM_CPUS        1

static struct arm_boot_info astra_sl_binfo = {
    .loader_start = ASTRA_SL_RAM_BASE,
    .board_id     = -1, /* no board ID; rely on device tree */
};

static void *astra_sl_fdt;
static int astra_sl_fdt_size;

static void *astra_sl_get_dtb(const struct arm_boot_info *binfo, int *fdt_size)
{
    *fdt_size = astra_sl_fdt_size;
    return astra_sl_fdt;
}

static void astra_sl_create_fdt(MachineState *machine)
{
    uint32_t clock_phandle;
    uint32_t gic_phandle;
    uint32_t timer_irqflags = deposit32(GIC_FDT_IRQ_FLAGS_LEVEL_HI,
                                        GIC_FDT_IRQ_PPI_CPU_START,
                                        GIC_FDT_IRQ_PPI_CPU_WIDTH, 1);
    const char compat[] = "snps,dw-apb-uart";
    const char timer_compat[] = "arm,armv8-timer\0arm,armv7-timer";
    const char *uart_node = "/uart@e5030000";
    const char *synpu_node = "/synpu@f7600000";
    const char *emmc_node = "/mmc@f7a00000";
    const char sdhci_compat[] = "snps,dwcmshc-sdhci";
    const char sdhci_clocknames[] = "core\0bus";
    const char synpu_interrupt_names[] = "job\0fault";

    astra_sl_fdt = create_device_tree(&astra_sl_fdt_size);
    if (!astra_sl_fdt) {
        error_report("astra_sl: create_device_tree() failed");
        exit(1);
    }

    qemu_fdt_setprop_string(astra_sl_fdt, "/", "compatible", "astra,astra-sl");
    qemu_fdt_setprop_string(astra_sl_fdt, "/", "model", "astra-sl");
    qemu_fdt_setprop_cell(astra_sl_fdt, "/", "#address-cells", 0x2);
    qemu_fdt_setprop_cell(astra_sl_fdt, "/", "#size-cells", 0x2);

    qemu_fdt_add_subnode(astra_sl_fdt, "/chosen");
    qemu_fdt_add_subnode(astra_sl_fdt, "/aliases");

    clock_phandle = qemu_fdt_alloc_phandle(astra_sl_fdt);
    qemu_fdt_add_subnode(astra_sl_fdt, "/apb-pclk");
    qemu_fdt_setprop_string(astra_sl_fdt, "/apb-pclk", "compatible",
                            "fixed-clock");
    qemu_fdt_setprop_cell(astra_sl_fdt, "/apb-pclk", "#clock-cells", 0x0);
    qemu_fdt_setprop_cell(astra_sl_fdt, "/apb-pclk", "clock-frequency",
                          24000000);
    qemu_fdt_setprop_string(astra_sl_fdt, "/apb-pclk", "clock-output-names",
                            "clk24mhz");
    qemu_fdt_setprop_cell(astra_sl_fdt, "/apb-pclk", "phandle",
                          clock_phandle);

    qemu_fdt_add_subnode(astra_sl_fdt, "/cpus");
    qemu_fdt_setprop_cell(astra_sl_fdt, "/cpus", "#address-cells", 0x2);
    qemu_fdt_setprop_cell(astra_sl_fdt, "/cpus", "#size-cells", 0x0);
    qemu_fdt_add_subnode(astra_sl_fdt, "/cpus/cpu@0");
    qemu_fdt_setprop_string(astra_sl_fdt, "/cpus/cpu@0", "device_type", "cpu");
    qemu_fdt_setprop_string(astra_sl_fdt, "/cpus/cpu@0", "compatible",
                            "arm,cortex-a55");
    qemu_fdt_setprop_u64(astra_sl_fdt, "/cpus/cpu@0", "reg", 0);

    gic_phandle = qemu_fdt_alloc_phandle(astra_sl_fdt);
    qemu_fdt_setprop_cell(astra_sl_fdt, "/", "interrupt-parent", gic_phandle);
    qemu_fdt_add_subnode(astra_sl_fdt, "/intc@f7e59000");
    qemu_fdt_setprop_string(astra_sl_fdt, "/intc@f7e59000", "compatible",
                            "arm,gic-400");
    qemu_fdt_setprop_cell(astra_sl_fdt, "/intc@f7e59000", "#interrupt-cells", 3);
    qemu_fdt_setprop(astra_sl_fdt, "/intc@f7e59000", "interrupt-controller",
                     NULL, 0);
    qemu_fdt_setprop_sized_cells(astra_sl_fdt, "/intc@f7e59000", "reg",
                                 2, ASTRA_SL_GIC_DIST_BASE, 2, 0x1000,
                                 2, ASTRA_SL_GIC_CPU_BASE, 2, 0x2000,
                                 2, ASTRA_SL_GIC_HYP_BASE, 2, 0x2000,
                                 2, ASTRA_SL_GIC_VCPU_BASE, 2, 0x2000);
    qemu_fdt_setprop_cell(astra_sl_fdt, "/intc@f7e59000", "phandle", gic_phandle);

    qemu_fdt_add_subnode(astra_sl_fdt, "/timer");
    qemu_fdt_setprop(astra_sl_fdt, "/timer", "compatible",
                     timer_compat, sizeof(timer_compat));
    qemu_fdt_setprop(astra_sl_fdt, "/timer", "always-on", NULL, 0);
    qemu_fdt_setprop_cells(astra_sl_fdt, "/timer", "interrupts",
                           GIC_FDT_IRQ_TYPE_PPI,
                           INTID_TO_PPI(ARCH_TIMER_S_EL1_IRQ), timer_irqflags,
                           GIC_FDT_IRQ_TYPE_PPI,
                           INTID_TO_PPI(ARCH_TIMER_NS_EL1_IRQ), timer_irqflags,
                           GIC_FDT_IRQ_TYPE_PPI,
                           INTID_TO_PPI(ARCH_TIMER_VIRT_IRQ), timer_irqflags,
                           GIC_FDT_IRQ_TYPE_PPI,
                           INTID_TO_PPI(ARCH_TIMER_NS_EL2_IRQ), timer_irqflags);

    qemu_fdt_add_subnode(astra_sl_fdt, uart_node);
    qemu_fdt_setprop_string(astra_sl_fdt, uart_node, "compatible", compat);
    qemu_fdt_setprop_sized_cells(astra_sl_fdt, uart_node, "reg",
                                 2, ASTRA_SL_UART0_BASE, 2, 0x100);
    qemu_fdt_setprop_cells(astra_sl_fdt, uart_node, "interrupts",
                           GIC_FDT_IRQ_TYPE_SPI,
                           ASTRA_SL_UART0_SPI,
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI);
    qemu_fdt_setprop_cell(astra_sl_fdt, uart_node, "clocks", clock_phandle);
    qemu_fdt_setprop_cell(astra_sl_fdt, uart_node, "reg-shift", 2);
    qemu_fdt_setprop_string(astra_sl_fdt, "/chosen", "stdout-path", uart_node);
    qemu_fdt_setprop_string(astra_sl_fdt, "/aliases", "serial0", uart_node);

    qemu_fdt_add_subnode(astra_sl_fdt, synpu_node);
    qemu_fdt_setprop_string(astra_sl_fdt, synpu_node, "compatible", "syna,npu");
    qemu_fdt_setprop_sized_cells(astra_sl_fdt, synpu_node, "reg",
                                 2, ASTRA_SL_DEV_BASE, 2, ASTRA_SL_DEV_SIZE);
    qemu_fdt_setprop_string(astra_sl_fdt, synpu_node, "reg-names", "npu_region");
    qemu_fdt_setprop_cells(astra_sl_fdt, synpu_node, "interrupts",
                           GIC_FDT_IRQ_TYPE_SPI,
                           ASTRA_SL_DEV_JOB_SPI,
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI,
                           GIC_FDT_IRQ_TYPE_SPI,
                           ASTRA_SL_DEV_FAULT_SPI,
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI);
    qemu_fdt_setprop(astra_sl_fdt, synpu_node, "interrupt-names",
                     synpu_interrupt_names, sizeof(synpu_interrupt_names));
    qemu_fdt_setprop_string(astra_sl_fdt, synpu_node, "status", "disabled");

    qemu_fdt_add_subnode(astra_sl_fdt, emmc_node);
    qemu_fdt_setprop(astra_sl_fdt, emmc_node, "compatible",
                     sdhci_compat, sizeof(sdhci_compat));
    qemu_fdt_setprop_sized_cells(astra_sl_fdt, emmc_node, "reg",
                                 2, ASTRA_SL_EMMC_BASE,
                                 2, ASTRA_SL_EMMC_SIZE);
    qemu_fdt_setprop_cells(astra_sl_fdt, emmc_node, "interrupts",
                           GIC_FDT_IRQ_TYPE_SPI,
                           ASTRA_SL_EMMC_SPI,
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI);
    qemu_fdt_setprop_cells(astra_sl_fdt, emmc_node, "clocks",
                           clock_phandle, clock_phandle);
    qemu_fdt_setprop(astra_sl_fdt, emmc_node, "clock-names",
                     sdhci_clocknames, sizeof(sdhci_clocknames));
    qemu_fdt_setprop_cell(astra_sl_fdt, emmc_node, "bus-width", 8);
    qemu_fdt_setprop(astra_sl_fdt, emmc_node, "non-removable", NULL, 0);
}

static void astra_sl_init(MachineState *machine)
{
    DriveInfo *di;
    BlockBackend *blk;
    MemoryRegion *sysmem = get_system_memory();
    DeviceState *carddev;
    DeviceState *gicdev;
    DeviceState *sdhci;
    SysBusDevice *gicbusdev;
    SysBusDevice *sdhcibusdev;
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

    /* GIC-400 (GICv2 with virtualization extensions) */
    gicdev = qdev_new(TYPE_ARM_GIC);
    qdev_prop_set_uint32(gicdev, "revision", 2);
    qdev_prop_set_uint32(gicdev, "num-cpu", ASTRA_SL_NUM_CPUS);
    qdev_prop_set_uint32(gicdev, "num-irq",
                         ASTRA_SL_NUM_IRQS + GIC_INTERNAL);
    qdev_prop_set_bit(gicdev, "has-virtualization-extensions", true);
    gicbusdev = SYS_BUS_DEVICE(gicdev);
    sysbus_realize_and_unref(gicbusdev, &error_fatal);
    sysbus_mmio_map(gicbusdev, 0, ASTRA_SL_GIC_DIST_BASE);
    sysbus_mmio_map(gicbusdev, 1, ASTRA_SL_GIC_CPU_BASE);
    sysbus_mmio_map(gicbusdev, 2, ASTRA_SL_GIC_HYP_BASE);
    sysbus_mmio_map(gicbusdev, 3, ASTRA_SL_GIC_VCPU_BASE);

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

    /* DesignWare APB UART (snps,dw-apb-uart) */
    serial_mm_init(get_system_memory(), ASTRA_SL_UART0_BASE, 2,
                   pic[ASTRA_SL_UART0_SPI], 115200, serial_hd(0),
                   DEVICE_LITTLE_ENDIAN);

    /* TORQ device: model exposes only one IRQ, wired to the "job" line. */
    torq_create(ASTRA_SL_DEV_BASE, pic[ASTRA_SL_DEV_JOB_SPI]);

    /* eMMC via a minimal SDHCI controller */
    sdhci = qdev_new(TYPE_SYSBUS_SDHCI);
    qdev_prop_set_uint8(sdhci, "sd-spec-version", 3);
    sdhcibusdev = SYS_BUS_DEVICE(sdhci);
    sysbus_realize_and_unref(sdhcibusdev, &error_fatal);
    sysbus_mmio_map(sdhcibusdev, 0, ASTRA_SL_EMMC_BASE);
    sysbus_connect_irq(sdhcibusdev, 0, pic[ASTRA_SL_EMMC_SPI]);

    di = drive_get(IF_SD, 0, 0);
    blk = di ? blk_by_legacy_dinfo(di) : NULL;
    carddev = qdev_new(TYPE_EMMC);
    qdev_prop_set_drive_err(carddev, "drive", blk, &error_fatal);
    qdev_realize_and_unref(carddev, qdev_get_child_bus(sdhci, "sd-bus"),
                           &error_fatal);

    /* Boot */
    astra_sl_create_fdt(machine);
    astra_sl_binfo.ram_size = machine->ram_size;
    astra_sl_binfo.get_dtb = astra_sl_get_dtb;
    arm_load_kernel(ARM_CPU(first_cpu), machine, &astra_sl_binfo);
}

static void astra_sl_machine_init(MachineClass *mc)
{
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-a55"),
        NULL
    };

    mc->desc              = "Astra SL board (Cortex-A55)";
    mc->init              = astra_sl_init;
    mc->default_cpu_type  = ARM_CPU_TYPE_NAME("cortex-a55");
    mc->valid_cpu_types   = valid_cpu_types;
    mc->block_default_type = IF_SD;
    mc->default_ram_size  = ASTRA_SL_RAM_SIZE;
    mc->default_ram_id    = "astra_sl.ram";
    mc->units_per_default_bus = 1;
    mc->max_cpus          = ASTRA_SL_NUM_CPUS;
    mc->ignore_memory_transaction_failures = true;
}

DEFINE_MACHINE_ARM("astra_sl", astra_sl_machine_init)
