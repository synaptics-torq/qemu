/*
 * Berlin "chip-control" reset controller
 * ("marvell,berlin4ct-chip-ctrl" and compatible variants)
 *
 * Copyright (c) 2026 QEMU Contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/berlin_reset.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"

/*
 * Emulates the "marvell,berlin4ct-chip-ctrl" reset controller. The kernel
 * driver (reset/reset-berlin.c) treats this block as a register file: each
 * reset line is one bit of one 32-bit register, selected by the device-tree
 * specifier <offset bit sticky inverted>. The driver asserts a line by
 * clearing the bit (setting it when inverted) and deasserts it the other
 * way around, so this model stores the raw register bits and notifies each
 * registered line on every assert/deassert transition. Lines that no
 * consumer has registered are plain register storage.
 */

typedef struct BerlinResetLine {
    BerlinResetSpec spec;
    void *opaque;
    BerlinResetLineCB cb;
} BerlinResetLine;

static bool berlin_line_asserted(uint32_t reg, const BerlinResetSpec *rst)
{
    bool set = !!(reg & (1u << rst->bit));

    return rst->inverted ? set : !set;
}

static uint64_t berlin_chip_ctrl_read(void *opaque, hwaddr addr, unsigned size)
{
    ChipCtrlState *cc = BERLIN_CHIP_CTRL(opaque);

    if ((addr & 3) || (addr + size > BERLIN_CHIP_CTRL_SIZE)) {
        return 0;
    }
    return cc->regs[addr / 4];
}

static void berlin_chip_ctrl_write(void *opaque, hwaddr addr, uint64_t value,
                                   unsigned size)
{
    ChipCtrlState *cc = BERLIN_CHIP_CTRL(opaque);
    uint32_t old, reg;
    GSList *l;

    if (size != 4 || (addr & 3) || (addr + size > BERLIN_CHIP_CTRL_SIZE)) {
        return;
    }
    old = cc->regs[addr / 4];
    reg = value;
    cc->regs[addr / 4] = reg;

    for (l = cc->lines; l; l = l->next) {
        BerlinResetLine *line = l->data;
        bool was_asserted, asserted;

        if (line->spec.offset != addr) {
            continue;
        }
        was_asserted = berlin_line_asserted(old, &line->spec);
        asserted = berlin_line_asserted(reg, &line->spec);
        if (was_asserted != asserted) {
            line->cb(line->opaque, asserted);
        }
    }
}

static const MemoryRegionOps berlin_chip_ctrl_ops = {
    .read  = berlin_chip_ctrl_read,
    .write = berlin_chip_ctrl_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void berlin_chip_ctrl_init(Object *obj)
{
    ChipCtrlState *cc = BERLIN_CHIP_CTRL(obj);

    memory_region_init_io(&cc->mmio, obj, &berlin_chip_ctrl_ops, cc,
                          TYPE_BERLIN_CHIP_CTRL, BERLIN_CHIP_CTRL_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &cc->mmio);
}

static void berlin_chip_ctrl_finalize(Object *obj)
{
    ChipCtrlState *cc = BERLIN_CHIP_CTRL(obj);
    GSList *l;

    for (l = cc->lines; l; l = l->next) {
        g_free(l->data);
    }
    g_slist_free(cc->lines);
    cc->lines = NULL;
}

static const TypeInfo berlin_chip_ctrl_info = {
    .name = TYPE_BERLIN_CHIP_CTRL,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ChipCtrlState),
    .instance_init = berlin_chip_ctrl_init,
    .instance_finalize = berlin_chip_ctrl_finalize,
};

static void berlin_chip_ctrl_register_types(void)
{
    type_register_static(&berlin_chip_ctrl_info);
}

type_init(berlin_chip_ctrl_register_types)

DeviceState *berlin_chip_ctrl_create(hwaddr addr)
{
    SysBusDevice *sbd;
    DeviceState *dev;

    dev = qdev_new(TYPE_BERLIN_CHIP_CTRL);
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, addr);

    return dev;
}

void berlin_chip_ctrl_add_reset_line(DeviceState *cc, const BerlinResetSpec *rst,
                                     void *opaque, BerlinResetLineCB cb)
{
    ChipCtrlState *s = BERLIN_CHIP_CTRL(cc);
    BerlinResetLine *line;

    assert(rst->offset % 4 == 0 && rst->offset < BERLIN_CHIP_CTRL_SIZE);
    assert(rst->bit < 32);
    line = g_new0(BerlinResetLine, 1);
    line->spec = *rst;
    line->opaque = opaque;
    line->cb = cb;
    s->lines = g_slist_prepend(s->lines, line);

    /* The line starts deasserted (the consumer is out of reset). */
    if (rst->inverted) {
        s->regs[rst->offset / 4] &= ~(1u << rst->bit);
    } else {
        s->regs[rst->offset / 4] |= 1u << rst->bit;
    }
}

bool berlin_chip_ctrl_reset_status(DeviceState *cc, const BerlinResetSpec *rst)
{
    ChipCtrlState *s = BERLIN_CHIP_CTRL(cc);

    assert(rst->offset % 4 == 0 && rst->offset < BERLIN_CHIP_CTRL_SIZE);
    return berlin_line_asserted(s->regs[rst->offset / 4], rst);
}
