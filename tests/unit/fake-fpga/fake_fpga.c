// Copyright 2026 Synaptics Inc.
//
// Licensed under the Apache License v2.0
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0

/*
 * Fake FPGA for tests/unit/test-torq-server-fpga.py: a register file in which an NSS start
 * completes after TORQ_FAKE_FPGA_JOB_US microseconds (default 500; negative: never), by
 * setting the started bits in the NSS status register. TORQ_FAKE_FPGA_READ_US makes each
 * register read take that long, like a PCIe read. XRAM is anonymous memory.
 */

#include "fpga_pci.h"
#include "hw/misc/torq-protocol.h"

#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>

static uint32_t registers[0x200000 / 4];
static uint32_t started;
static int64_t started_us;

static int64_t setting_us(const char *name, int64_t fallback)
{
    const char *value = getenv(name);

    return value ? strtoll(value, NULL, 10) : fallback;
}

static int64_t now_us(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

int fpga_pci_attach(int slot_id, int pf_id, int bar_id, uint32_t flags,
                    pci_bar_handle_t *handle)
{
    *handle = bar_id;
    return 0;
}

int fpga_pci_detach(pci_bar_handle_t handle)
{
    return 0;
}

int fpga_pci_peek(pci_bar_handle_t handle, uint64_t offset, uint32_t *value)
{
    int64_t read_us = setting_us("TORQ_FAKE_FPGA_READ_US", 0);
    int64_t job_us = setting_us("TORQ_FAKE_FPGA_JOB_US", 500);

    for (int64_t until = now_us() + read_us; now_us() < until;) {
    }
    if (offset == TORQ_NSS_STATUS && started && job_us >= 0 &&
        now_us() - started_us >= job_us) {
        registers[TORQ_NSS_STATUS / 4] |= started;
        started = 0;
    }
    *value = registers[(offset % sizeof(registers)) / 4];
    return 0;
}

int fpga_pci_poke(pci_bar_handle_t handle, uint64_t offset, uint32_t value)
{
    if (offset == TORQ_NSS_START) {
        started = value;
        started_us = now_us();
    } else if (offset == TORQ_NSS_STATUS) {
        /* write 1 to clear */
        registers[TORQ_NSS_STATUS / 4] &= ~value;
    } else {
        registers[(offset % sizeof(registers)) / 4] = value;
    }
    return 0;
}

int fpga_pci_get_address(pci_bar_handle_t handle, uint64_t offset, uint64_t dword_len,
                         void **ptr)
{
    *ptr = mmap(NULL, dword_len, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return *ptr == MAP_FAILED ? -1 : 0;
}
