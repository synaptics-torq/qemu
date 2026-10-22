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
#include "qemu/guest-random.h"
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
#include "hw/arm/smmuv3.h"
#include "hw/arm/smmu-common.h"
#include "hw/misc/torq.h"
#include "hw/misc/berlin_reset.h"
#include "hw/sd/sd.h"
#include "hw/sd/sdhci.h"
#include "target/arm/cpu.h"
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

/* SMMUv3 (ARM MMU-600) per sl2611.dtsi "smmu: iommu@f7800000". */
#define ASTRA_SL_SMMU_BASE         0xf7800000
#define ASTRA_SL_SMMU_SIZE         0x20000 /* SMMUv3 register window (like virt) */
/* QEMU IRQ order: evtq(0), priq(1), cmdq-sync(2), gerror(3). The DT only
 * declares eventq/priq/gerror (SPI 120/121/125); cmdq-sync is not in the DT,
 * so it is wired to a free SPI that the guest never requests. */
#define ASTRA_SL_SMMU_EVTQ_SPI     120  /* "eventq" */
#define ASTRA_SL_SMMU_PRIQ_SPI     121  /* "priq" */
#define ASTRA_SL_SMMU_SYNC_SPI     126  /* QEMU cmdq-sync (free SPI, not in DT) */
#define ASTRA_SL_SMMU_GERROR_SPI   125  /* "gerror" */
/* The single emulated TORQ master uses the first NPU stream id. */
#define ASTRA_SL_TORQ_STREAM_ID    0x1

/* Free MMIO window in the sl2611.dtsi address map (between 0xf7800000 and
 * 0xf7a00000) used for the emulated virtio-mmio transport.
 */
#define ASTRA_SL_VIRTIO_MMIO_BASE  0xf7880000
#define ASTRA_SL_VIRTIO_MMIO_SIZE  0x1000
#define ASTRA_SL_VIRTIO_MMIO_SPI   208  /* first of free SPIs 208.. per sl2611.dtsi */
#define ASTRA_SL_VIRTIO_MMIO_NUM   2    /* e.g. one for blk, one for net */

/* NPU reset provider ("marvell,berlin4ct-chip-ctrl") per sl2611.dtsi "chip". */
#define ASTRA_SL_CHIP_CTRL_BASE    0xf7e10000

/* SPI input line indices (matching sl2611.dtsi) */
#define ASTRA_SL_UART0_SPI       130  /* uart0 SPI ID 130 */
#define ASTRA_SL_DEV_JOB_SPI     52   /* TORQ job SPI ID 52 */
#define ASTRA_SL_DEV_FAULT_SPI   207  /* TORQ fault SPI ID 207 */
#define ASTRA_SL_EMMC_SPI        13   /* sdhci0 SPI ID 13 */

#define ASTRA_SL_NUM_IRQS        256
#define ASTRA_SL_NUM_CPUS        2

static struct arm_boot_info astra_sl_binfo = {
    .loader_start = ASTRA_SL_RAM_BASE,
    .board_id     = -1, /* no board ID; rely on device tree */
    /* Use QEMU's emulated PSCI (SMC conduit) so the guest can power on the
     * secondary Cortex-A55 core. Matches the real board's PSCI node
     * (arm,psci-1.0, method = "smc"). */
    .psci_conduit = QEMU_PSCI_CONDUIT_SMC,
};

static void *astra_sl_fdt;
static int astra_sl_fdt_size;

static void *astra_sl_get_dtb(const struct arm_boot_info *binfo, int *fdt_size)
{
    *fdt_size = astra_sl_fdt_size;
    return astra_sl_fdt;
}

static void astra_sl_create_fdt(MachineState *machine, int smp_cpus)
{
    uint32_t clock_phandle;
    uint32_t gic_phandle;
    uint32_t smmu_phandle;
    uint32_t npu_core_clk_phandle;
    uint32_t syspll1_phandle;
    uint32_t chip_ctrl_phandle;
    /* GICv2 PPI per-CPU mask bits covering all CPUs (GIC_CPU_MASK_SIMPLE(n)). */
    uint32_t timer_irqflags = deposit32(GIC_FDT_IRQ_FLAGS_LEVEL_HI,
                                        GIC_FDT_IRQ_PPI_CPU_START,
                                        GIC_FDT_IRQ_PPI_CPU_WIDTH,
                                        (1 << smp_cpus) - 1);
    const char compat[] = "snps,dw-apb-uart";
    const char timer_compat[] = "arm,armv8-timer\0arm,armv7-timer";
    const char *uart_node = "/uart@e5030000";
    const char *synpu_node = "/synpu@f7600000";    
    const char *smmu_node = "/iommu@f7800000";
    const char synpu_interrupt_names[] = "job\0fault";
    const char synpu_clock_names[] = "core\0clk_high\0clk_low";
    const char synpu_reset_names[] = "core_rst";
    const char syspll1_clock_names[] = "syspll1_low\0syspll1_high";
    const char smmu_interrupt_names[] = "eventq\0priq\0gerror";

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

    /* This machine has no hardware RNG, so seed the kernel's CRNG directly
     * from the DT; otherwise boots can stall indefinitely on blocking
     * getrandom() calls (e.g. systemd machine-id generation). */
    {
        uint8_t rng_seed[32];

        qemu_guest_getrandom_nofail(rng_seed, sizeof(rng_seed));
        qemu_fdt_setprop(astra_sl_fdt, "/chosen", "rng-seed",
                         rng_seed, sizeof(rng_seed));
    }

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

    {
        int i;

        qemu_fdt_add_subnode(astra_sl_fdt, "/cpus");
        qemu_fdt_setprop_cell(astra_sl_fdt, "/cpus", "#address-cells", 0x2);
        qemu_fdt_setprop_cell(astra_sl_fdt, "/cpus", "#size-cells", 0x0);
        for (i = 0; i < smp_cpus; i++) {
            char *node = g_strdup_printf("/cpus/cpu@%d", i);

            qemu_fdt_add_subnode(astra_sl_fdt, node);
            qemu_fdt_setprop_string(astra_sl_fdt, node, "device_type", "cpu");
            qemu_fdt_setprop_string(astra_sl_fdt, node, "compatible",
                                    "arm,cortex-a55");
            /* enable-method "psci": secondary CPUs are powered on by the
             * QEMU-emulated PSCI CPU_ON call. */
            qemu_fdt_setprop_string(astra_sl_fdt, node, "enable-method",
                                    "psci");
            /* The MPIDR affinity must match arm_cpu_mp_affinity() for this
             * CPU (a single cluster of A55 cores), so that PSCI's
             * arm_set_cpu_on() can resolve the target. */
            qemu_fdt_setprop_u64(astra_sl_fdt, node, "reg", i);
            g_free(node);
        }
    }

    gic_phandle = qemu_fdt_alloc_phandle(astra_sl_fdt);
    smmu_phandle = qemu_fdt_alloc_phandle(astra_sl_fdt);
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

    /* NPU clock providers. On the real board these come from the
     * "syna,sl261x-clk" (&clk) and "syna,dolphin-pll" (&syspll1) overlay
     * drivers, which are not loaded in the emulated guest; fixed clocks
     * model them. Deliberately no OPP table: the driver's devfreq path
     * then stays disabled, probe still succeeds and jobs can run. */
    npu_core_clk_phandle = qemu_fdt_alloc_phandle(astra_sl_fdt);
    qemu_fdt_add_subnode(astra_sl_fdt, "/npu-core-clk");
    qemu_fdt_setprop_string(astra_sl_fdt, "/npu-core-clk", "compatible",
                            "fixed-clock");
    qemu_fdt_setprop_cell(astra_sl_fdt, "/npu-core-clk", "#clock-cells", 0x0);
    qemu_fdt_setprop_cell(astra_sl_fdt, "/npu-core-clk", "clock-frequency",
                          800000000);
    qemu_fdt_setprop_string(astra_sl_fdt, "/npu-core-clk", "clock-output-names",
                            "npucoreclk");
    qemu_fdt_setprop_cell(astra_sl_fdt, "/npu-core-clk", "phandle",
                          npu_core_clk_phandle);

    /* Two outputs selected by index (clk_high/clk_low), like the real
     * &syspll1; without an OPP table the driver never switches between
     * them, so equal rates are fine. */
    syspll1_phandle = qemu_fdt_alloc_phandle(astra_sl_fdt);
    qemu_fdt_add_subnode(astra_sl_fdt, "/syspll1");
    qemu_fdt_setprop_string(astra_sl_fdt, "/syspll1", "compatible",
                            "fixed-clock");
    qemu_fdt_setprop_cell(astra_sl_fdt, "/syspll1", "#clock-cells", 0x1);
    qemu_fdt_setprop_cell(astra_sl_fdt, "/syspll1", "clock-frequency",
                          400000000);
    qemu_fdt_setprop(astra_sl_fdt, "/syspll1", "clock-output-names",
                     syspll1_clock_names, sizeof(syspll1_clock_names));
    qemu_fdt_setprop_cell(astra_sl_fdt, "/syspll1", "phandle", syspll1_phandle);

    /* NPU reset provider, matched by the built-in
     * "marvell,berlin4ct-chip-ctrl" reset driver (CONFIG_RESET_BERLIN=y).
     * Backed by the berlin-chip-ctrl device instantiated above
     * (berlin_chip_ctrl_create); the NPU core reset (resets =
     * <&chip 0x378 1 1 0>) fires the C model reset on the assert edge. */
    chip_ctrl_phandle = qemu_fdt_alloc_phandle(astra_sl_fdt);
    qemu_fdt_add_subnode(astra_sl_fdt, "/chip-control@f7e10000");
    qemu_fdt_setprop_string(astra_sl_fdt, "/chip-control@f7e10000",
                            "compatible", "marvell,berlin4ct-chip-ctrl");
    qemu_fdt_setprop_sized_cells(astra_sl_fdt, "/chip-control@f7e10000",
                                 "reg", 2, 0xf7e10000, 2, 0x400);
    qemu_fdt_setprop_cell(astra_sl_fdt, "/chip-control@f7e10000",
                          "#reset-cells", 4);
    qemu_fdt_setprop_cell(astra_sl_fdt, "/chip-control@f7e10000", "phandle",
                          chip_ctrl_phandle);

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
    /* Mirrors the real DTS: clocks = <&clk CLK_NPU>, <&syspll1 0>,
     * <&syspll1 1>; clock-names = "core", "clk_high", "clk_low". */
    qemu_fdt_setprop_cells(astra_sl_fdt, synpu_node, "clocks",
                           npu_core_clk_phandle,
                           syspll1_phandle, 0x0,
                           syspll1_phandle, 0x1);
    qemu_fdt_setprop(astra_sl_fdt, synpu_node, "clock-names",
                     synpu_clock_names, sizeof(synpu_clock_names));
    /* Mirrors the real DTS: resets = <&chip 0x378 1 1 0>. */
    qemu_fdt_setprop_cells(astra_sl_fdt, synpu_node, "resets",
                           chip_ctrl_phandle, 0x378, 1, 1, 0);
    qemu_fdt_setprop(astra_sl_fdt, synpu_node, "reset-names",
                     synpu_reset_names, sizeof(synpu_reset_names));
    /* NPU stream ids (matches the real NPU's iommus property). */
    qemu_fdt_setprop_cells(astra_sl_fdt, synpu_node, "iommus",
                           smmu_phandle, 0x1,
                           smmu_phandle, 0x2,
                           smmu_phandle, 0x3,
                           smmu_phandle, 0x4);
    /* Enabled, as the coralboard overlay does (&synpu { status = "okay"; }).
     * Without this the kernel never instantiates the platform device. */
    qemu_fdt_setprop_string(astra_sl_fdt, synpu_node, "status", "okay");

    /* SMMUv3 (ARM MMU-600), matching sl2611.dtsi "smmu: iommu@f7800000". */
    qemu_fdt_add_subnode(astra_sl_fdt, smmu_node);
    qemu_fdt_setprop_string(astra_sl_fdt, smmu_node, "compatible", "arm,smmu-v3");
    qemu_fdt_setprop_sized_cells(astra_sl_fdt, smmu_node, "reg",
                                 2, ASTRA_SL_SMMU_BASE, 2, ASTRA_SL_SMMU_SIZE);
    qemu_fdt_setprop_cell(astra_sl_fdt, smmu_node, "#iommu-cells", 1);
    qemu_fdt_setprop_cell(astra_sl_fdt, smmu_node, "#address-cells", 2);
    qemu_fdt_setprop_cell(astra_sl_fdt, smmu_node, "#size-cells", 2);
    qemu_fdt_setprop_cells(astra_sl_fdt, smmu_node, "interrupts",
                           GIC_FDT_IRQ_TYPE_SPI, ASTRA_SL_SMMU_EVTQ_SPI,
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI,
                           GIC_FDT_IRQ_TYPE_SPI, ASTRA_SL_SMMU_PRIQ_SPI,
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI,
                           GIC_FDT_IRQ_TYPE_SPI, ASTRA_SL_SMMU_GERROR_SPI,
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI);
    qemu_fdt_setprop(astra_sl_fdt, smmu_node, "interrupt-names",
                     smmu_interrupt_names, sizeof(smmu_interrupt_names));
    qemu_fdt_setprop_cell(astra_sl_fdt, smmu_node, "phandle", smmu_phandle);
    qemu_fdt_setprop_string(astra_sl_fdt, smmu_node, "status", "okay");

    for (int i = 0; i < ASTRA_SL_VIRTIO_MMIO_NUM; i++) {
        hwaddr base = ASTRA_SL_VIRTIO_MMIO_BASE + i * ASTRA_SL_VIRTIO_MMIO_SIZE;
        char *virtio_node = g_strdup_printf("/virtio_mmio@%" HWADDR_PRIx,
                                            base);

        qemu_fdt_add_subnode(astra_sl_fdt, virtio_node);
        qemu_fdt_setprop_string(astra_sl_fdt, virtio_node, "compatible",
                                "virtio,mmio");
        qemu_fdt_setprop_sized_cells(astra_sl_fdt, virtio_node, "reg",
                                     2, base, 2, ASTRA_SL_VIRTIO_MMIO_SIZE);
        qemu_fdt_setprop_cells(astra_sl_fdt, virtio_node, "interrupts",
                               GIC_FDT_IRQ_TYPE_SPI,
                               ASTRA_SL_VIRTIO_MMIO_SPI + i,
                               GIC_FDT_IRQ_FLAGS_EDGE_LO_HI);
        qemu_fdt_setprop(astra_sl_fdt, virtio_node, "dma-coherent", NULL, 0);
        g_free(virtio_node);
    }
}

/*
 * Single cluster of Cortex-A55 cores. arm_build_mp_affinity() returns the
 * core index as MPIDR for the default cluster size (8), so the FDT "reg"
 * value for cpu@<i> is simply <i> and PSCI's arm_set_cpu_on() (which matches
 * on arm_cpu_mp_affinity) resolves the target.
 */
static const CPUArchIdList *astra_sl_possible_cpu_arch_ids(MachineState *ms)
{
    unsigned int max_cpus = ms->smp.max_cpus;
    int n;

    if (ms->possible_cpus) {
        assert(ms->possible_cpus->len == max_cpus);
        return ms->possible_cpus;
    }

    ms->possible_cpus = g_malloc0(sizeof(CPUArchIdList) +
                                  sizeof(CPUArchId) * max_cpus);
    ms->possible_cpus->len = max_cpus;
    for (n = 0; n < ms->possible_cpus->len; n++) {
        ms->possible_cpus->cpus[n].type = ms->cpu_type;
        ms->possible_cpus->cpus[n].arch_id = arm_build_mp_affinity(n,
                                                                   ARM_DEFAULT_CPUS_PER_CLUSTER);
        ms->possible_cpus->cpus[n].props.has_core_id = true;
        ms->possible_cpus->cpus[n].props.core_id = n;
    }
    return ms->possible_cpus;
}

/*
 * Provide a virtio-mmio transport slot at ASTRA_SL_VIRTIO_MMIO_BASE. The user
 * attaches a backend to it from the command line, e.g.:
 *   -device virtio-blk-device,drive=rootfs
 * QEMU auto-plugs the device onto the free virtio-mmio bus. An empty slot
 * (no -device attached) is harmless: the guest sees the transport but no
 * device, exactly like the "virt" machine does.
 */
static void astra_sl_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    const CPUArchIdList *possible_cpus;
    DeviceState *gicdev;
    SysBusDevice *gicbusdev;
    qemu_irq pic[ASTRA_SL_NUM_IRQS];
    int smp_cpus;
    int i;
    DeviceState *smmu;
    DeviceState *torq;
    DeviceState *chip_ctrl;

    /* Number of CPUs to actually create (capped by the hardware maximum). */
    smp_cpus = MIN(machine->smp.cpus, ASTRA_SL_NUM_CPUS);
    possible_cpus = astra_sl_possible_cpu_arch_ids(machine);

    /* Validate RAM size */
    if (machine->ram_size > ASTRA_SL_RAM_SIZE) {
        error_report("astra_sl: RAM size %" PRId64 " MiB exceeds maximum %d MiB",
                     machine->ram_size / MiB, (int)(ASTRA_SL_RAM_SIZE / MiB));
        exit(1);
    }

    /* Create and realize CPUs, each with an MPIDR affinity matching the FDT
     * cpu@<i> "reg" (see astra_sl_possible_cpu_arch_ids). */
    for (i = 0; i < smp_cpus; i++) {
        Object *cpuobj = object_new(possible_cpus->cpus[i].type);

        object_property_add_child(OBJECT(machine), "cpu[*]", cpuobj);
        object_property_set_uint(cpuobj, "mp-affinity",
                                 possible_cpus->cpus[i].arch_id, &error_fatal);
        qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);
        object_unref(cpuobj);
    }

    /* Map RAM */
    memory_region_add_subregion(sysmem, ASTRA_SL_RAM_BASE, machine->ram);

    /* GIC-400 (GICv2 with virtualization extensions) */
    gicdev = qdev_new(TYPE_ARM_GIC);
    qdev_prop_set_uint32(gicdev, "revision", 2);
    qdev_prop_set_uint32(gicdev, "num-cpu", smp_cpus);
    qdev_prop_set_uint32(gicdev, "num-irq",
                         ASTRA_SL_NUM_IRQS + GIC_INTERNAL);
    qdev_prop_set_bit(gicdev, "has-virtualization-extensions", true);
    gicbusdev = SYS_BUS_DEVICE(gicdev);
    sysbus_realize_and_unref(gicbusdev, &error_fatal);
    sysbus_mmio_map(gicbusdev, 0, ASTRA_SL_GIC_DIST_BASE);
    sysbus_mmio_map(gicbusdev, 1, ASTRA_SL_GIC_CPU_BASE);
    sysbus_mmio_map(gicbusdev, 2, ASTRA_SL_GIC_HYP_BASE);
    sysbus_mmio_map(gicbusdev, 3, ASTRA_SL_GIC_VCPU_BASE);

    /* Wire CPU timers to GIC PPIs and GIC outputs to CPU IRQ/FIQ inputs. */
    for (i = 0; i < smp_cpus; i++) {
        DeviceState *cpudev = DEVICE(qemu_get_cpu(i));
        int irq_base = ASTRA_SL_NUM_IRQS + i * GIC_INTERNAL;
        const int timer_irq[] = {
            [GTIMER_PHYS] = ARCH_TIMER_NS_EL1_IRQ,
            [GTIMER_VIRT] = ARCH_TIMER_VIRT_IRQ,
            [GTIMER_HYP] = ARCH_TIMER_NS_EL2_IRQ,
            [GTIMER_SEC] = ARCH_TIMER_S_EL1_IRQ,
        };

        for (unsigned timer = 0; timer < ARRAY_SIZE(timer_irq); timer++) {
            qdev_connect_gpio_out(cpudev, timer,
                                  qdev_get_gpio_in(gicdev,
                                                   irq_base + timer_irq[timer]));
        }

        sysbus_connect_irq(gicbusdev, i,
                           qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(gicbusdev, i + smp_cpus,
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

    /* SMMUv3 (ARM MMU-600): a standalone device with no PCI. Created after
     * the GIC so its SPIs are available; it services the TORQ master. */
    smmu = qdev_new(TYPE_ARM_SMMUV3);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(smmu), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(smmu), 0, ASTRA_SL_SMMU_BASE);
    /* QEMU SMMU IRQ order: evtq(0), priq(1), cmdq-sync(2), gerror(3). */
    sysbus_connect_irq(SYS_BUS_DEVICE(smmu), 0, pic[ASTRA_SL_SMMU_EVTQ_SPI]);
    sysbus_connect_irq(SYS_BUS_DEVICE(smmu), 1, pic[ASTRA_SL_SMMU_PRIQ_SPI]);
    sysbus_connect_irq(SYS_BUS_DEVICE(smmu), 2, pic[ASTRA_SL_SMMU_SYNC_SPI]);
    sysbus_connect_irq(SYS_BUS_DEVICE(smmu), 3, pic[ASTRA_SL_SMMU_GERROR_SPI]);

    /* "marvell,berlin4ct-chip-ctrl" NPU reset provider at
     * ASTRA_SL_CHIP_CTRL_BASE; the kernel reset driver asserts/deasserts
     * device resets through it. */
    chip_ctrl = berlin_chip_ctrl_create(ASTRA_SL_CHIP_CTRL_BASE);

    /* TORQ device: model exposes only one IRQ, wired to the "job" line. Its
     * DMA is routed through the SMMU for stream id ASTRA_SL_TORQ_STREAM_ID. */
    torq = torq_create(ASTRA_SL_DEV_BASE, pic[ASTRA_SL_DEV_JOB_SPI]);
    torq_set_dma_as(torq,
                    smmu_get_platform_master_as(ARM_SMMU(smmu),
                                                ASTRA_SL_TORQ_STREAM_ID));

    /* Register the NPU core reset line (resets = <&chip 0x378 1 1 0>) on the
     * chip-ctrl provider; the assert edge fires the TORQ C model reset. */
    berlin_chip_ctrl_add_reset_line(chip_ctrl, &torq_core_rst, TORQ(torq),
                                    torq_reset_line_cb);

    /* Virtio-mmio transport slot; backends are attached via -device
     * (e.g. -device virtio-blk-device,drive=...). */
    for (i = 0; i < ASTRA_SL_VIRTIO_MMIO_NUM; i++) {
        sysbus_create_simple("virtio-mmio",
                             ASTRA_SL_VIRTIO_MMIO_BASE +
                             i * ASTRA_SL_VIRTIO_MMIO_SIZE,
                             pic[ASTRA_SL_VIRTIO_MMIO_SPI + i]);
    }

    /* Boot */
    astra_sl_create_fdt(machine, smp_cpus);
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

    mc->desc              = "Astra SL board (dual Cortex-A55)";
    mc->init              = astra_sl_init;
    mc->default_cpu_type  = ARM_CPU_TYPE_NAME("cortex-a55");
    mc->valid_cpu_types   = valid_cpu_types;
    mc->block_default_type = IF_SD;
    mc->default_ram_size  = ASTRA_SL_RAM_SIZE;
    mc->default_ram_id    = "astra_sl.ram";
    mc->units_per_default_bus = 1;
    mc->max_cpus          = ASTRA_SL_NUM_CPUS;
    /* Two cores by default so a plain `-machine astra_sl` brings up the
     * dual-core A55. default_cpus also drives ms->smp.max_cpus, which TCG
     * uses to size its execution thread pool. */
    mc->default_cpus      = ASTRA_SL_NUM_CPUS;
    mc->possible_cpu_arch_ids = astra_sl_possible_cpu_arch_ids;
    mc->ignore_memory_transaction_failures = true;
}

DEFINE_MACHINE_ARM("astra_sl", astra_sl_machine_init)
