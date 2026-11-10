// Copyright 2026 Synaptics Inc.
//
// Licensed under the Apache License v2.0
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0

/*
 * Runs the CSS (Compute Sub-System) firmware on the MPACT (CoralNPU)
 * RISC-V behavioral simulator.
 *
 * This is the function registered with the TORQ C model via
 * torq_cm__set_css_cpu_code(). When the model needs to execute CSS software
 * it calls torq_mpact_run_cpu(css_cpu_t *), which boots the CSS firmware on a
 * real MPACT core. The core's memory and register accesses are routed back
 * through the behavioral C model (TCM via css_sw__tcm_a2p, registers via
 * css_sw__reg_read/write, LRAM via the model's external-memory callbacks).
 * 
 */

#include "hw/misc/torq_mpact.h"

/* The CSS model tags memory accesses with multichar constants (e.g. 'CCPU');
 * mirror the pragma in cm/torq_cm.h to keep -Werror=multichar happy. */
#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wmultichar"
#endif

/* Build the CSS software model helpers as real (host) symbols. */
#define TORQ_BEH_CSS_SW

#include <coralnpu_simulator.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <string>

#include "css_sw/common/css_sw_reg_inc.h"
#include "css_sw_inc.h"

/*
 * Runtime switches, read from the environment (no IREE flag framework in
 * QEMU). The coral simulator is silent and untraced by default:
 *   TORQ_MPACT_VERBOSE=1        enable LOG() diagnostics
 *   TORQ_MPACT_TRACE_FILE=path  write the instruction trace to `path`
 */
static bool torq_mpact_verbose;
static std::string torq_mpact_trace_file;

static void mpact_load_env(void)
{
    static bool loaded = false;

    if (loaded) {
        return;
    }
    loaded = true;

    const char *v = getenv("TORQ_MPACT_VERBOSE");
    if (v && v[0]) {
        torq_mpact_verbose = (v[0] != '0');
    }
    const char *t = getenv("TORQ_MPACT_TRACE_FILE");
    if (t && t[0]) {
        torq_mpact_trace_file = t;
    }
}

#define LOG(...)                                                                                   \
    do {                                                                                           \
        if (torq_mpact_verbose) {                                                                   \
            printf(__VA_ARGS__);                                                                    \
        }                                                                                          \
        fflush(stdout);                                                                             \
    } while (0)

/* Opaque C model CSS CPU. Mirrors cm/css/src/css_cpu.c `struct css_cpu_t`
 * (layout must stay in sync). */
typedef int (*mpact_cb_read_mem_t) (void *ctx, uint32_t tag, size_t addr,
                                    size_t size, uint8_t *data);
typedef int (*mpact_cb_write_mem_t) (void *ctx, uint32_t tag, size_t addr,
                                     size_t size, uint8_t *data);

typedef struct mpact_fake_css_cpu {
    uint32_t state;
    char is_waiting;
    uint8_t *itcm;
    uint32_t itcm_size;
    uint8_t *dtcm;
    uint32_t dtcm_size;
    uint32_t cpu_regs[REG_SIZE__TORQ_CPU_REGS / 4];
    void *ext_mem_ctx;
    mpact_cb_read_mem_t cb_read_ext_mem;
    mpact_cb_write_mem_t cb_write_ext_mem;
    void *ext_irq_ctx;
    void *cb_get_ext_irq;
    void (*css_sw)(void *);
} mpact_fake_css_cpu;

/* Decode a RISC-V exception code (see RISC-V Privileged Spec Table 3.6). */
static void dump_riscv_fault(uint32_t mcode, uint32_t mtval)
{
    fprintf(stderr, "RISC-V Exception: mcode=0x%08x, mtval=0x%08x\n", mcode, mtval);

    switch (mcode) {
    case 0x0:
        fprintf(stderr, "Instruction address misaligned. Faulting address: 0x%08x\n", mtval);
        break;
    case 0x1:
        fprintf(stderr, "Instruction access fault. Faulting address: 0x%08x\n", mtval);
        break;
    case 0x2:
        fprintf(stderr, "Illegal instruction. Faulting instruction: 0x%08x\n", mtval);
        break;
    case 0x3:
        fprintf(stderr, "Breakpoint at address: 0x%08x\n", mtval);
        break;
    case 0x4:
        fprintf(stderr, "Load address misaligned. Faulting address: 0x%08x\n", mtval);
        break;
    case 0x5:
        fprintf(stderr, "Load access fault. Faulting address: 0x%08x\n", mtval);
        break;
    case 0x6:
        fprintf(stderr, "Store/AMO address misaligned. Faulting address: 0x%08x\n", mtval);
        break;
    case 0x7:
        fprintf(stderr, "Store/AMO access fault. Faulting address: 0x%08x\n", mtval);
        break;
    case 0x8:
        fprintf(stderr, "Environment call from U-mode.\n");
        break;
    case 0x9:
        fprintf(stderr, "Environment call from S-mode.\n");
        break;
    case 0xb:
        fprintf(stderr, "Environment call from M-mode.\n");
        break;
    default:
        fprintf(stderr, "Unknown or unhandled RISC-V exception code: 0x%08x\n", mcode);
        break;
    }
}

/*
 * After the core halts, the CSS firmware signals success/failure through the
 * mailbox. MBX_DAT_0 holds the exception code (0xFFFFFFFF == clean); on fault
 * MBX_DAT_1 holds mtval.
 */
static void process_status_code(void *cpu)
{
    uint32_t mcode = css_sw__reg_read(cpu, RA_(CSS, MBX_DAT_0));

    if (mcode != 0xFFFFFFFF) {
        uint32_t mtval = css_sw__reg_read(cpu, RA_(CSS, MBX_DAT_1));

        dump_riscv_fault(mcode, mtval);
        fflush(stderr);

        fprintf(stderr, "Aborting execution");
        fflush(stderr);
        abort();
    }
}

static void execute_program(CoralNPUSimulator *coral_sim, uint32_t start_pc)
{
    LOG("------ coral output start ------\n");

    coral_sim->Run(start_pc);
    if (coral_sim->WaitForTermination(500000)) {
        LOG("Halted \n");
    } else {
        LOG("Didn't halt \n");
    }

    LOG("------ coral output end------\n");
}

namespace {

/* ITCM/DTCM: backed by the model's TCM buffers. */
class TcmMemoryTarget : public CoralMemoryTarget {
public:
    TcmMemoryTarget(void *cpu) : cpu_(cpu) {}

    void Load(uint64_t address, uint8_t *data, size_t size) override
    {
        void *host = css_sw__tcm_a2p(cpu_, (uint32_t)address);
        memcpy(data, host, size);
    }

    void Store(uint64_t address, const uint8_t *data, size_t size) override
    {
        void *host = css_sw__tcm_a2p(cpu_, (uint32_t)address);
        memcpy(host, data, size);
    }

private:
    void *cpu_;
};

/* CSS/CDMA register space: routed to the model register file. */
class CssRegsTarget : public CoralMemoryTarget {
public:
    CssRegsTarget(void *cpu) : cpu_(cpu) {}

    void Load(uint64_t address, uint8_t *data, size_t size) override
    {
        assert(size == 4);
        assert((address & 0x3) == 0);
        *(uint32_t *)data = css_sw__reg_read(cpu_, (uint32_t)address);
    }

    void Store(uint64_t address, const uint8_t *data, size_t size) override
    {
        assert(size == 4);
        assert((address & 0x3) == 0);
        css_sw__reg_write(cpu_, (uint32_t)address, *(uint32_t *)data);
    }

private:
    void *cpu_;
};

/* UART scratch: treat any store as a null-terminated message to the host. */
class UartTarget : public CoralMemoryTarget {
public:
    void Load(uint64_t address, uint8_t *data, size_t size) override
    {
        /* No-op for reads */
    }

    void Store(uint64_t address, const uint8_t *data, size_t size) override
    {
        std::string output(reinterpret_cast<const char *>(data), size);
        printf("%s", output.c_str());
        fflush(stdout);
    }
};

/* LRAM: forwarded to the model's external memory (guest address space via
 * the xbar -> nss mem path). */
class LramTarget : public CoralMemoryTarget {
public:
    LramTarget(void *cpu) : cpu_((mpact_fake_css_cpu *)cpu) {}

    void Load(uint64_t address, uint8_t *data, size_t size) override
    {
        int r = (cpu_->cb_read_ext_mem)(cpu_->ext_mem_ctx, 'CCPU',
                                        (size_t)address, size, data);
        if (r < 0) {
            fprintf(stderr, "Invalid CSS read at address 0x%08x, size %zu\n",
                    (uint32_t)address, size);
            abort();
        }
    }

    void Store(uint64_t address, const uint8_t *data, size_t size) override
    {
        int r = (cpu_->cb_write_ext_mem)(cpu_->ext_mem_ctx, 'CCPU',
                                         (size_t)address, size,
                                         (uint8_t *)data);
        if (r < 0) {
            fprintf(stderr, "Invalid CSS write at address 0x%08x, size %zu\n",
                    (uint32_t)address, size);
            abort();
        }
    }

private:
    mpact_fake_css_cpu *cpu_;
};

/* Unmapped address: fail loudly. */
class OobTarget : public CoralMemoryTarget {
public:
    void Load(uint64_t address, uint8_t *data, size_t size) override
    {
        fprintf(stderr, "Out-of-bounds read at address 0x%08x, size %zu\n",
                (uint32_t)address, size);
        abort();
    }

    void Store(uint64_t address, const uint8_t *data, size_t size) override
    {
        fprintf(stderr, "Out-of-bounds write at address 0x%08x, size %zu\n",
                (uint32_t)address, size);
        abort();
    }
};

} /* anonymous namespace */

/* Instruction trace sink (only used when a trace file is configured). */
static void trace_callback(uint32_t pc, uint32_t instruction,
                           std::string &disassembly)
{
    static FILE *trace_file = nullptr;

    if (!trace_file) {
        trace_file = fopen(torq_mpact_trace_file.c_str(), "w");
        if (!trace_file) {
            fprintf(stderr, "Failed to open trace file: %s\n",
                    torq_mpact_trace_file.c_str());
            return;
        }
    }

    fprintf(trace_file, "0x%08x: 0x%08x %s\n", pc, instruction,
            disassembly.c_str());

    /* Flush always: the simulation may crash. */
    fflush(trace_file);
}

/*
 * Entry point invoked by the TORQ C model to run CSS software.
 * @cpu is the model's CSS CPU instance (mpact_fake_css_cpu).
 */
extern "C" void torq_mpact_run_cpu(void *cpu)
{
    mpact_load_env();

    LOG("Creating CPU\n");

    CoralNPUSimulator *coral_sim = CoralNPUSimulator::Create();

    TcmMemoryTarget tcm_memory_target(cpu);
    UartTarget uart_target;
    CssRegsTarget css_regs_target(cpu);
    LramTarget lram_target(cpu);
    OobTarget oob_target;

    coral_sim->RegisterMemoryTarget(REG_ADDR__TORQ_CV_ITCM,
                                    REG_SIZE__TORQ_CV_ITCM, &tcm_memory_target);
    coral_sim->RegisterMemoryTarget(REG_ADDR__TORQ_CV_DTCM,
                                    REG_SIZE__TORQ_CV_DTCM, &tcm_memory_target);
    coral_sim->RegisterMemoryTarget(REG_ADDR__TORQ_CV_LRAM,
                                    REG_SIZE__TORQ_CV_LRAM, &lram_target);
    coral_sim->RegisterMemoryTarget(0x10000000, 4, &uart_target);
    coral_sim->RegisterMemoryTarget(REG_ADDR__TORQ_CV_CSS_REGS,
                                    REG_SIZE__TORQ_CV_CSS_REGS, &css_regs_target);
    coral_sim->RegisterMemoryTarget(REG_ADDR__TORQ_CV_CDMA_REGS,
                                    REG_SIZE__TORQ_CV_CDMA_REGS, &css_regs_target);
    coral_sim->RegisterOOBMemoryTarget(&oob_target);

    if (!torq_mpact_trace_file.empty()) {
        coral_sim->SetTraceCallback(trace_callback, true);
    }

    LOG("Starting CPU\n");

    uint32_t start_pc = css_sw__reg_read(cpu, RA_(CPU, PCSTART));

    LOG("Starting CPU with PC = 0x%08x\n", start_pc);

    execute_program(coral_sim, start_pc);

    /* For debug purposes: check the mailbox status word. */
    process_status_code(cpu);

    css_sw__end_sleep(cpu);
}
