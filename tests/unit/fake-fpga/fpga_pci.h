// Copyright 2026 Synaptics Inc.
//
// Licensed under the Apache License v2.0
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0

/*
 * Stand-in for the AWS FPGA SDK's fpga_pci.h, with only what hw/misc/torq-fpga.c uses, so
 * that tests can build the server's FPGA backend without the SDK (see fake_fpga.c).
 */

#ifndef TORQ_FAKE_FPGA_PCI_H
#define TORQ_FAKE_FPGA_PCI_H

#include <stdint.h>

typedef int pci_bar_handle_t;

int fpga_pci_attach(int slot_id, int pf_id, int bar_id, uint32_t flags,
                    pci_bar_handle_t *handle);
int fpga_pci_detach(pci_bar_handle_t handle);
int fpga_pci_peek(pci_bar_handle_t handle, uint64_t offset, uint32_t *value);
int fpga_pci_poke(pci_bar_handle_t handle, uint64_t offset, uint32_t value);
int fpga_pci_get_address(pci_bar_handle_t handle, uint64_t offset, uint64_t dword_len,
                         void **ptr);

#endif
