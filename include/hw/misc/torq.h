/*
 * TORQ placeholder device
 *
 * Copyright (c) 2026 QEMU Contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_TORQ_H
#define HW_MISC_TORQ_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_TORQ "torq"
OBJECT_DECLARE_SIMPLE_TYPE(TORQState, TORQ)

#define TORQ_MMIO_SIZE 0x00200000

struct TORQState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq irq;
    hwaddr mmio_base;

    void *cm;
};

DeviceState *torq_create(hwaddr addr, qemu_irq irq);

#endif
