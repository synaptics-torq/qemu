/*
 * Berlin "chip-control" reset controller
 * ("marvell,berlin4ct-chip-ctrl" and compatible variants)
 *
 * Copyright (c) 2026 QEMU Contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_BERLIN_RESET_H
#define HW_MISC_BERLIN_RESET_H

#include "hw/core/sysbus.h"
#include "hw/core/qdev.h"

#define TYPE_BERLIN_CHIP_CTRL "berlin-chip-ctrl"
OBJECT_DECLARE_SIMPLE_TYPE(ChipCtrlState, BERLIN_CHIP_CTRL)

#define BERLIN_CHIP_CTRL_SIZE  0x400

/* One reset line: a bit of the 32-bit register at byte @offset. Mirrors the
 * last three cells of the kernel DT specifier <offset bit sticky inverted>;
 * only sticky (assert/deassert) lines are modeled. */
typedef struct BerlinResetSpec {
    uint32_t offset;   /* byte offset within the controller */
    uint32_t bit;      /* bit index (0..31) */
    bool inverted;     /* true = active-high (asserting sets the bit) */
} BerlinResetSpec;

/* Fired when a registered reset line changes state. @asserted is true if the
 * line was just asserted (reset active). */
typedef void (*BerlinResetLineCB)(void *opaque, bool asserted);

struct ChipCtrlState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    uint32_t regs[BERLIN_CHIP_CTRL_SIZE / 4];
    GSList *lines;     /* registered reset lines */
};

/* Create a "marvell,berlin4ct-chip-ctrl" reset controller at @addr. */
DeviceState *berlin_chip_ctrl_create(hwaddr addr);

/*
 * Register a reset line: the line starts deasserted (the consumer is out of
 * reset) and @cb is fired on every assert/deassert transition caused by
 * guest MMIO writes. Multiple consumers (different IPs) may register
 * different offset/bit/inverted combinations.
 */
void berlin_chip_ctrl_add_reset_line(DeviceState *cc, const BerlinResetSpec *rst,
                                     void *opaque, BerlinResetLineCB cb);

/* Return true if the given reset line is currently asserted. */
bool berlin_chip_ctrl_reset_status(DeviceState *cc, const BerlinResetSpec *rst);

#endif
