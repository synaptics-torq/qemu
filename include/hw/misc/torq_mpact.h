/*
 * TORQ MPACT CSS simulation
 *
 * Copyright (c) 2026 QEMU Contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_TORQ_MPACT_H
#define HW_MISC_TORQ_MPACT_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * CSS entry point executed by the MPACT (CoralNPU) RISC-V simulator.
 *
 * Registered with the C model via torq_cm__set_css_cpu_code(). When the model
 * wants to run the CSS software it invokes this function, which boots the CSS
 * firmware on a real MPACT core and routes the core's memory / register
 * accesses through the behavioral C model.
 *
 * @cpu is the C model's CSS CPU instance (opaque; the css_cpu_t struct).
 *
 * Only available when QEMU is built with MPACT CSS simulation support
 * (--with-torq-mpact-sim-lib / --with-torq-mpact-sim-incdir).
 */
void torq_mpact_run_cpu(void *cpu);

#ifdef __cplusplus
}
#endif

#endif /* HW_MISC_TORQ_MPACT_H */
