// Host-side generator for the bare-metal DMA test.
//
// Uses the CT descriptor-generation API (synpu-hw/ct/desc_gen) — the same
// torq_api.c the compiler uses — to build the NPU program that performs a
// single DMA.XR transfer: 64 bytes from XRAM (guest RAM at XRAM_SRC) into
// LRAM at LRAM_DST. Mirrors the flow of
// synpu-hw/ct/app/example/torq_api_example_dma.c (dma_xram2lram).
//
// It prints C snippets (DMA_NPU_PROGRAM / DMA_EXPECTED_LRAM) to paste into
// tests/astra_sl_baremetal/main.c.

#include <stdio.h>
#include <stdint.h>
#include <string.h>

// NOTE: do NOT define TORQ_CT_DEBUG (keeps torq_api.c's xdbg/assert as no-op
// stdlib assert, no printf/exit symbols referenced from debug macros).

#include "torq_api.h"

// Stubbed out (no dump files needed for the test).
void *torq_desc_dump__open(const char *path) { (void)path; return (void *)1; }
int torq_desc_dump__close(void *self) { (void)self; return 0; }
int torq_desc_dump__ndl_info(void *self, int job_id, int tsk_id, int slc_id,
                             uint32_t tag, int set_id, torq_ndl_cmd_t *cmd)
{
    (void)self; (void)job_id; (void)tsk_id; (void)slc_id;
    (void)tag; (void)set_id; (void)cmd; return 0;
}
int torq_desc_dump__data(void *self, int job_id, int stg_id, int tsk_id,
                         int slc_id, uint32_t mem_tag, uint32_t op_tag,
                         uint32_t data_tag, uint32_t addr_tag, uint32_t addr,
                         uint32_t size, void *data)
{
    (void)self; (void)job_id; (void)stg_id; (void)tsk_id; (void)slc_id;
    (void)mem_tag; (void)op_tag; (void)data_tag; (void)addr_tag;
    (void)addr; (void)size; (void)data; return 0;
}

#define XRAM_SRC  0x40010000u // guest RAM (astra_sl: RAM at 0x40000000, kernel+stack live just below)
#define LRAM_DST  0x40u      // LRAM words 16..31; program occupies words 0..17
#define DMA_SIZE  64u

int main(void)
{
    int r;
    void *npu = torq_open(NULL);

    r = torq_cfg_begin(npu, -1, 0, TORQ_XADDR_NONE);
    if (r < 0) { fprintf(stderr, "torq_cfg_begin failed\n"); return 1; }

    torq_cfg_t cfg;
    torq_ndl_cmd_t cmd;
    memset(&cfg, 0, sizeof(cfg));
    cfg.dma_xr_start = 1;
    cfg.dma_xr_wait = 1;
    cfg.dma_xr_attr = 0x0011; // arqos=0, arcache=0, arprot=1, aruser=1
                              // aruser=1 -> SMMU stream 0x1 (see NPU DT node)
    r = torq_task_cfg_begin(npu, &cfg, ~0, TORQ_XADDR_NONE);
    if (r < 0) { fprintf(stderr, "torq_task_cfg_begin failed\n"); return 1; }

    // XRAM side (source): 64 contiguous bytes
    memset(&cmd, 0, sizeof(cmd));
    cmd.n[0] = DMA_SIZE; cmd.s[0] = 1; cmd.t[0] = 'A'; cmd.nhd = 1;
    cmd.base_addr = XRAM_SRC;
    r = torq_ndl_desc_write(npu, 'DIXR', 0, &cmd, 0, TORQ_XADDR_NONE);
    if (r < 0) { fprintf(stderr, "DIXR failed\n"); return 1; }

    // LRAM side (destination): 64 contiguous bytes at LRAM_DST
    memset(&cmd, 0, sizeof(cmd));
    cmd.n[0] = DMA_SIZE; cmd.s[0] = 1; cmd.t[0] = 'A'; cmd.nhd = 1;
    cmd.base_addr = LRAM_DST;
    r = torq_ndl_desc_write(npu, 'DILW', 0, &cmd, 0, TORQ_XADDR_NONE);
    if (r < 0) { fprintf(stderr, "DILW failed\n"); return 1; }

    r = torq_task_cfg_end(npu);
    if (r < 0) { fprintf(stderr, "torq_task_cfg_end failed\n"); return 1; }
    r = torq_cfg_end(npu);
    if (r < 0) { fprintf(stderr, "torq_cfg_end failed\n"); return 1; }

    // Collect the generated segments.
    torq_bitstream_segment_t *seg, *first = NULL, *last = NULL;
    r = torq_get_bitstream(npu, &seg);
    if (r < 0) { fprintf(stderr, "torq_get_bitstream failed\n"); return 1; }
    while (seg) {
        if (!first) first = seg;
        last = seg;
        seg = seg->next;
    }
    // Print all segments; find the program segment at lram_addr 0.
    const uint32_t *prog = NULL;
    size_t prog_words = 0;
    for (seg = first; seg; seg = seg->next) {
        printf("// seg: lram=0x%08x xram=0x%08x size=%u words\n",
               seg->lram_addr, seg->xram_addr, seg->size, seg->size / 4);
        if (seg->lram_addr == 0) {
            prog = (const uint32_t *)seg->data;
            prog_words = seg->size / 4;
        }
    }
    if (!prog) { fprintf(stderr, "no program segment at lram 0\n"); return 1; }

    printf("\nconst uint32_t DMA_NPU_PROGRAM[%zu] = {\n", prog_words);
    for (size_t i = 0; i < prog_words; i++) {
        printf("    0x%08xu%s\n", prog[i], i + 1 < prog_words ? "," : "");
    }
    printf("};\n");

    // Reference data for the DMA source: i*8+1
    printf("\nconst uint8_t DMA_EXPECTED_LRAM[%u] = {\n", DMA_SIZE);
    for (size_t i = 0; i < DMA_SIZE; i++) {
        printf("    0x%02xu%s\n", (uint8_t)(i * 8 + 1),
               i + 1 < DMA_SIZE ? "," : "");
    }
    printf("};\n");

    return 0;
}
