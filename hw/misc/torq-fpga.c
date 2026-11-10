// Copyright 2026 Synaptics Inc.
//
// Licensed under the Apache License v2.0
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0

#include "hw/misc/torq-fpga.h"
#include "hw/misc/torq-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fpga_pci.h>

struct TorqFpga {
    pci_bar_handle_t registers;
    pci_bar_handle_t memory;
    uint8_t *xram;
    int lock_fd;
    uint32_t launch_address;
    uint32_t launch_value;
    bool busy;
};

TorqFpga *torq_fpga_open(void)
{
    TorqFpga *fpga = calloc(1, sizeof(*fpga));
    struct stat status;

    if (!fpga) {
        return NULL;
    }
    fpga->registers = fpga->memory = fpga->lock_fd = -1;
    fpga->lock_fd = open("/tmp/torq-aws-fpga-0.lock",
                          O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fpga->lock_fd < 0 || fstat(fpga->lock_fd, &status) ||
        status.st_uid != getuid() || flock(fpga->lock_fd, LOCK_EX | LOCK_NB) ||
        fpga_pci_attach(0, 0, 0, 0, &fpga->registers) ||
        fpga_pci_attach(0, 0, 4, 0, &fpga->memory) ||
        fpga_pci_get_address(fpga->memory, UINT64_C(0x800000000),
                             UINT64_C(0x400000000), (void **)&fpga->xram)) {
        torq_fpga_close(fpga);
        return NULL;
    }
    return fpga;
}

void torq_fpga_close(TorqFpga *fpga)
{
    if (fpga->registers >= 0) {
        fpga_pci_detach(fpga->registers);
    }
    if (fpga->memory >= 0) {
        fpga_pci_detach(fpga->memory);
    }
    if (fpga->lock_fd >= 0) {
        close(fpga->lock_fd);
    }
    free(fpga);
}

int torq_fpga_read32(TorqFpga *fpga, uint32_t address, uint32_t *value)
{
    return fpga_pci_peek(fpga->registers, address, value) ? -EIO : 0;
}

int torq_fpga_write32(TorqFpga *fpga, uint32_t address, uint32_t value)
{
    atomic_thread_fence(memory_order_seq_cst);
    if (fpga_pci_poke(fpga->registers, address, value)) {
        return -EIO;
    }
    if (torq_execution_write(address, value)) {
        fpga->launch_address = address;
        fpga->launch_value = value;
        fpga->busy = true;
    }
    return 0;
}

int torq_fpga_memory(TorqFpga *fpga, uint32_t address, size_t size,
                      uint8_t *data, bool writing)
{
    if (fpga->busy || size > TORQ_IOVA_LIMIT - address) {
        return -EBUSY;
    }
    while (size) {
        uint32_t aligned = address & ~15U;
        size_t offset = address & 15;
        size_t count = size < 16 - offset ? size : 16 - offset;
        uint8_t block[16];
        uint8_t *device = fpga->xram + ((uint64_t)aligned << 2);

        if (!writing || offset || count != sizeof(block)) {
            memcpy(block, device, sizeof(block));
        }
        if (writing) {
            memcpy(block + offset, data, count);
            memcpy(device, block, sizeof(block));
        } else {
            memcpy(data, block + offset, count);
        }
        data += count;
        address += count;
        size -= count;
    }
    atomic_thread_fence(memory_order_seq_cst);
    return 0;
}

int torq_fpga_get_irq(TorqFpga *fpga, uint32_t *irq)
{
    uint32_t enabled, status, control, mailbox;
    uint32_t dma_status, dma_enable, dma_control;
    uint32_t pending = 0;

    if (torq_fpga_read32(fpga, 0x1fc004, &enabled) ||
        torq_fpga_read32(fpga, TORQ_NSS_STATUS, &status) ||
        torq_fpga_read32(fpga, 0x1f0008, &control) ||
        torq_fpga_read32(fpga, TORQ_CDMA_STATUS, &dma_status) ||
        torq_fpga_read32(fpga, 0x1ff008, &dma_enable) ||
        torq_fpga_read32(fpga, 0x1fe20c, &dma_control)) {
        return -EIO;
    }
    pending = status & control & 0xff;
    for (unsigned int index = 0; index < 4; index++) {
        if (torq_fpga_read32(fpga, 0x1fc010 + index * 4, &mailbox)) {
            return -EIO;
        }
        pending |= (mailbox & 1) << (8 + index);
    }
    if ((dma_status & (1U << 16)) && (dma_enable & dma_control & 1)) {
        pending |= 1U << 12;
    }
    *irq = !!(pending & enabled);
    if (fpga->busy) {
        if (fpga->launch_address == TORQ_NSS_START) {
            fpga->busy = (status & fpga->launch_value) != fpga->launch_value;
        } else if (fpga->launch_address == TORQ_CDMA_COMMAND) {
            fpga->busy = !(dma_status & (1U << 16));
        } else {
            fpga->busy = !(pending & (1U << 9));
        }
    }
    return 0;
}

bool torq_fpga_busy(TorqFpga *fpga)
{
    return fpga->busy;
}

int torq_fpga_reset(TorqFpga *fpga)
{
    if (fpga->busy) {
        return -EBUSY;
    }
    if (torq_fpga_write32(fpga, TORQ_CPU_CONTROL, 3) ||
        torq_fpga_write32(fpga, 0x1fc004, 0) ||
        torq_fpga_write32(fpga, TORQ_NSS_STATUS, UINT32_MAX) ||
        torq_fpga_write32(fpga, TORQ_NSS_START, 0) ||
        torq_fpga_write32(fpga, TORQ_CDMA_COMMAND, 4)) {
        return -EIO;
    }
    fpga->busy = false;
    return 0;
}
