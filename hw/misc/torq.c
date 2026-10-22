/*
 * TORQ placeholder device
 *
 * Copyright (c) 2026 QEMU Contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/torq.h"
#include "hw/misc/berlin_reset.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "system/address-spaces.h"
#include "cm/torq_cm.h"

static void torq_update_irq(TORQState *s)
{
    uint32_t irq = 0;

    torq_cm__get_irq(s->cm, 0, 1, &irq);
    
    qemu_set_irq(s->irq, irq != 0);
}

static uint64_t torq_read(void *opaque, hwaddr addr, unsigned int size)
{
    TORQState *s = TORQ(opaque);
    hwaddr cm_addr = addr >= s->mmio_base ? addr - s->mmio_base : addr;
    uint64_t data = 0;

    if (size == 8) {
        uint32_t lo = 0, hi = 0;

        /* The CM only speaks 32-bit accesses; split a 64-bit one. */
        torq_cm_read32(s->cm, cm_addr, &lo);
        torq_cm_read32(s->cm, cm_addr + 4, &hi);
        data = lo | ((uint64_t)hi << 32);
    } else {
        uint32_t d32 = 0;

        torq_cm_read32(s->cm, cm_addr, &d32);
        data = d32;
    }
    
    torq_update_irq(s);

    return data;
}

static void torq_write(void *opaque, hwaddr addr,
                       uint64_t value, unsigned int size)
{
    TORQState *s = TORQ(opaque);
    hwaddr cm_addr = addr >= s->mmio_base ? addr - s->mmio_base : addr;
    uint32_t lo = value & 0xffffffff;

    if (size == 8) {
        uint32_t hi = value >> 32;

        /* The CM only speaks 32-bit accesses; split a 64-bit one. */
        torq_cm_write32(s->cm, cm_addr, &lo);
        torq_cm_write32(s->cm, cm_addr + 4, &hi);
    } else {
        torq_cm_write32(s->cm, cm_addr, &lo);
    }
    torq_update_irq(s);
}

/* ctx is the TORQState (see torq_cm__set_ext_mem_if). */
static int torq_cm_read_mem(void *ctx, uint32_t tag, size_t addr,
                            size_t size, uint8_t *data)
{
    TORQState *s = ctx;
    AddressSpace *as = s->dma_as;

    return address_space_read(as, addr, MEMTXATTRS_UNSPECIFIED, data, size);
}

static int torq_cm_write_mem(void *ctx, uint32_t tag, size_t addr,
                             size_t size, uint8_t *data)
{
    TORQState *s = ctx;
    AddressSpace *as = s->dma_as;

    return address_space_write(as, addr, MEMTXATTRS_UNSPECIFIED, data, size);
}

static const MemoryRegionOps torq_ops = {
    .read = torq_read,
    .write = torq_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

static void torq_init(Object *obj)
{
    TORQState *s = TORQ(obj);

    memory_region_init_io(&s->mmio, obj, &torq_ops, s, TYPE_TORQ, TORQ_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->mmio_base = 0;
    s->dma_as = &address_space_memory;

    s->cm = torq_cm_open(0, 0, 0);

    if (!s->cm) {
        error_report("Failed to open TORQ CM");
        exit(1);
    }

    torq_cm__set_ext_mem_if(s->cm, s, torq_cm_read_mem, torq_cm_write_mem);
    torq_update_irq(s);
}

void torq_set_dma_as(DeviceState *dev, AddressSpace *as)
{
    TORQState *s = TORQ(dev);

    if (as) {
        s->dma_as = as;
    }
}

static void torq_finalize(Object *obj)
{
    TORQState *s = TORQ(obj);

    if (s->cm) {
        torq_cm_close(s->cm);
        s->cm = NULL;
    }
}

static const TypeInfo torq_info = {
    .name = TYPE_TORQ,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TORQState),
    .instance_init = torq_init,
    .instance_finalize = torq_finalize,
};

static void torq_register_types(void)
{
    type_register_static(&torq_info);
}

type_init(torq_register_types)

/* Reset-line registration is done by the board (see torq_core_rst and
 * torq_reset_line_cb in hw/misc/torq.h). */
const BerlinResetSpec torq_core_rst = {
    .offset = 0x378,
    .bit = 1,
    .inverted = false,
};

/*
 * The guest asserts the NPU core reset by clearing the (active-low) bit,
 * after which the kernel deasserts it and reprograms the NPU from scratch,
 * so resetting the C model on the assert edge matches the hardware.
 * @opaque must be a TORQState.
 */
void torq_reset_line_cb(void *opaque, bool asserted)
{
    TORQState *s = TORQ(opaque);

    printf("TORQ reset line callback: asserted=%d\n", asserted);

    if (asserted) {
        torq_cm__reset(s->cm);
    }
}

DeviceState *torq_create(hwaddr addr, qemu_irq irq)
{
    SysBusDevice *sbd;
    DeviceState *dev;
    TORQState *s;

    dev = qdev_new(TYPE_TORQ);
    s = TORQ(dev);
    s->mmio_base = addr;
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, addr);
    sysbus_connect_irq(sbd, 0, irq);

    return dev;
}
