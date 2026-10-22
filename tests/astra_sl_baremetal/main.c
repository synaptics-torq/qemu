#include <stddef.h>
#include <stdint.h>

#include "torq_reg_util.h"
#include "torq_regs_host_view.h"
#include "torq_nss_regs.h"
#include "torq_css_regs.h"

#define PL011_BASE 0xe5030000UL
#define UART_THR   (PL011_BASE + 0x00)  /* Transmit Holding Register */
#define UART_LSR   (PL011_BASE + 0x14)  /* Line Status Register (regshift=2: offset 5*4) */
#define LSR_THRE   (1u << 5)            /* Transmit Holding Register Empty */

#define TORQ_BASE_ADDR 0xf7600000
#define TORQ_IRQ       84U

#define ASTRA_SL_GIC_DIST_BASE 0xf7e59000UL
#define ASTRA_SL_GIC_CPU_BASE  0xf7e5a000UL

/* NPU core reset line, per the DT "resets = <&chip 0x378 1 1 0>" property:
 * chip-ctrl register offset 0x378, bit 1, sticky, active-low (bit set =
 * deasserted). The berlin-chip-ctrl provider is emulated at CHIP_CTRL_BASE
 * (see astra_sl.c). */
#define CHIP_CTRL_BASE        0xf7e10000UL
#define CHIP_CTRL_NPU_RST_REG (CHIP_CTRL_BASE + 0x378UL)
#define CHIP_CTRL_NPU_RST_BIT (1u << 1)

#define GICD_CTLR              0x000U
#define GICD_ISENABLER         0x100U
#define GICD_ICENABLER         0x180U
#define GICD_IPRIORITYR        0x400U
#define GICD_ITARGETSR         0x800U

#define GICC_CTLR              0x000U
#define GICC_PMR               0x004U
#define GICC_IAR               0x00cU
#define GICC_EOIR              0x010U

static volatile uint32_t torq_irq_seen;

static inline void mmio_write32(uintptr_t addr, uint32_t value)
{
    *(volatile uint32_t *)addr = value;
}

static inline uint32_t mmio_read32(uintptr_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline void mmio_rmw32(uintptr_t addr, uint32_t mask, uint32_t value)
{
    uint32_t cur = mmio_read32(addr);
    cur = (cur & ~mask) | (value & mask);
    mmio_write32(addr, cur);
}

static void uart_putc(char c)
{
    while (!(mmio_read32(UART_LSR) & LSR_THRE)) {
    }
    mmio_write32(UART_THR, (uint32_t)c);
}

static void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n') {
            uart_putc('\r');
        }
        uart_putc(*s++);
    }
}


struct cfg_cmd {
    uint32_t a : 16;   // Start CFG register address        
    uint32_t n : 12;   // Number of consecutive CFG register values
    uint32_t id : 3;   // Command ID; 1=CFG command; Must be 1
    uint32_t lc : 1;   // Last command in descriptor
};

struct syn_cmd {
    uint32_t mask : 16;   // Start CFG register address
    uint32_t _reserved : 12;   // reserved bits must be 0
    uint32_t id : 3;   // Command ID; 1=SYN command; Must be 6
    uint32_t lc : 1;   // Last command in descriptor
};

struct next_cmd {
    uint32_t addr: 24;   // Next descriptor address
    uint32_t _reserved : 4;   // reserved bits must be 0
    uint32_t id : 3;   // Command ID; 1=NEXT command; Must be 7
    uint32_t lc : 1;   // Last command in descriptor
};

struct npu_instruction {

    union {
        struct cfg_cmd cfg;
        struct syn_cmd syn;
        struct next_cmd next;
        uint32_t raw;
    } hdr;
    
    uint32_t data[];
};

void torq_reg_write32(uint64_t addr, uint32_t data)
{
    mmio_write32((uintptr_t)(TORQ_BASE_ADDR + addr), data);
}

uint32_t torq_reg_read32(uint64_t addr)
{
    return mmio_read32((uintptr_t)(TORQ_BASE_ADDR + addr));
}

int torq_hw_start(uint32_t start_lram_addr)
{
    torq_reg_write32(RA_(NSS,CFG), RF_LSH(NSS,CFG_LINK_EN, 1) | RF_BMSK_LSH(NSS,CFG_DESC, start_lram_addr));  // set NSS CFG descriptor address
    torq_reg_write32(RA_(NSS,CTRL), RF_LSH(NSS,CTRL_IEN_NSS, 1));  // enable NSS interrupt (source)
    torq_reg_write32(RA_(CSS,IEN_HST), RF_LSH(CSS,IEN_HST_NSS, 1));  // enable NSS interrupt (for host)
    torq_reg_write32(RA_(NSS,START), RF_LSH(NSS,START_NSS, 1));  // kick off NSS CFG agent
    return 0;
}

void torq_wfi() {
    __asm__ volatile ("wfi");
}

static void gic_enable_spi_irq(uint32_t irq)
{
    uint32_t reg = irq / 32U;
    uint32_t bit = irq % 32U;
    mmio_write32(ASTRA_SL_GIC_DIST_BASE + GICD_ISENABLER + reg * 4U,
                 (1U << bit));
}

static void gic_disable_spi_irq(uint32_t irq)
{
    uint32_t reg = irq / 32U;
    uint32_t bit = irq % 32U;
    mmio_write32(ASTRA_SL_GIC_DIST_BASE + GICD_ICENABLER + reg * 4U,
                 (1U << bit));
}

static void gic_set_irq_target_cpu0(uint32_t irq)
{
    uintptr_t addr = ASTRA_SL_GIC_DIST_BASE + GICD_ITARGETSR + ((irq / 4U) * 4U);
    uint32_t shift = (irq % 4U) * 8U;
    mmio_rmw32(addr, 0xffU << shift, 0x01U << shift);
}

static void gic_set_irq_priority(uint32_t irq, uint8_t priority)
{
    uintptr_t addr = ASTRA_SL_GIC_DIST_BASE + GICD_IPRIORITYR + ((irq / 4U) * 4U);
    uint32_t shift = (irq % 4U) * 8U;
    mmio_rmw32(addr, 0xffU << shift, ((uint32_t)priority) << shift);
}

static void gic_init(void)
{
    torq_irq_seen = 0;

    mmio_write32(ASTRA_SL_GIC_CPU_BASE + GICC_CTLR, 0U);
    mmio_write32(ASTRA_SL_GIC_DIST_BASE + GICD_CTLR, 0U);

    gic_set_irq_priority(TORQ_IRQ, 0x80U);
    gic_set_irq_target_cpu0(TORQ_IRQ);
    gic_enable_spi_irq(TORQ_IRQ);

    mmio_write32(ASTRA_SL_GIC_CPU_BASE + GICC_PMR, 0xffU);
    mmio_write32(ASTRA_SL_GIC_CPU_BASE + GICC_CTLR, 0x1U);
    mmio_write32(ASTRA_SL_GIC_DIST_BASE + GICD_CTLR, 0x1U);

    __asm__ volatile ("dsb sy\n\tisb");
}

void irq_handler(void)
{
    uint32_t iar = mmio_read32(ASTRA_SL_GIC_CPU_BASE + GICC_IAR);
    uint32_t irq = iar & 0x3ffU;

    if (irq == TORQ_IRQ) {
        torq_irq_seen = 1;
        gic_disable_spi_irq(TORQ_IRQ);
    }

    mmio_write32(ASTRA_SL_GIC_CPU_BASE + GICC_EOIR, iar);
}

int torq_hw_wait()
{
    while (!torq_irq_seen) {
        torq_wfi();
    }

    while (1) {
        uint32_t status = torq_reg_read32(RA_(NSS,STATUS)); // poll NSS status
        if (RF_FMSK_RSH(NSS,STATUS_NSS, status)) break;
    }
    uart_puts("Wait done\n");
    return 0;
}

void torq_enable_interrupts() {
    __asm__ volatile ("msr daifclr, #2");  // enable IRQ
}

int torq_hw_end()
{
    torq_reg_write32(RA_(NSS,STATUS), 1);  // clear NSS status
    return 0;
}

/*
 * DMA test.
 *
 * DMA_NPU_PROGRAM is generated with the CT descriptor API (the same
 * torq_api.c the compiler uses to emit NPU programs) -- see gen/gen_dma_program.c
 * for the generator and synpu-hw/ct/app/example/torq_api_example_dma.c for the
 * flow it follows. It configures a single DMA.XR transfer: 64 bytes from XRAM
 * (guest RAM at DMA_XRAM_SRC, ARUSER=1 -> SMMU stream 0x1) into LRAM at
 * DMA_LRAM_DST, then SYN-waits for the DMA.XR completion status.
 * The program occupies LRAM words 0..17.
 */
#define DMA_XRAM_SRC   0x40010000u  // guest RAM (astra_sl: RAM at 0x40000000)
#define DMA_LRAM_DST   0x40u        // LRAM destination (past the program)
#define DMA_XFER_SIZE  64u

#include "gen/dma_program.h"

static void run_dma_test(void)
{
    volatile uint32_t *lram = (volatile uint32_t *)(TORQ_BASE_ADDR + REG_ADDR__TORQ_HV_LRAM);
    volatile uint8_t *src = (volatile uint8_t *)(uintptr_t)DMA_XRAM_SRC;
    size_t i;
    int ok = 1;

    uart_puts("DMA test: priming source buffer in RAM\n");
    for (i = 0; i < DMA_XFER_SIZE; i++) {
        src[i] = (uint8_t)(i * 8 + 1);
    }

    uart_puts("DMA test: loading NPU program into LRAM\n");

    // load the generated NPU program into LRAM at 0
    for (i = 0; i < sizeof(DMA_NPU_PROGRAM) / sizeof(DMA_NPU_PROGRAM[0]); i++) {
        lram[i] = DMA_NPU_PROGRAM[i];
    }

    // the previous test's IRQ handler disabled the TORQ SPI
    torq_irq_seen = 0;
    gic_enable_spi_irq(TORQ_IRQ);

    uart_puts("DMA test: starting NPU program (DMA.XR: RAM -> LRAM)\n");
    torq_hw_start(0);
    torq_hw_wait();

    uint32_t status = torq_reg_read32(RA_(NSS, STATUS));
    if (!RF_FMSK_RSH(NSS, STATUS_XR, status)) {
        uart_puts("DMA test: FAIL (NSS.STATUS.xr not set)\n");
        torq_hw_end();
        return;
    }

    // verify LRAM contents match the source pattern
    for (i = 0; i < DMA_XFER_SIZE / 4; i++) {
        uint32_t want = 0;
        for (int b = 0; b < 4; b++) {
            want |= (uint32_t)((i * 4 + b) * 8 + 1) << (8 * b);
        }
        uint32_t got = lram[DMA_LRAM_DST / 4 + i];
        if (got != want) {
            uart_puts("DMA test: FAIL (LRAM data mismatch)\n");
            ok = 0;
            break;
        }
    }
    if (ok) {
        uart_puts("DMA test: PASS (64 bytes RAM -> LRAM)\n");
    }

    torq_hw_end();
}

/*
 * NPU core reset test.
 *
 * Asserting the reset line (clearing the active-low bit) must reset the NPU
 * C model: previously written NSS/CSS registers read back as 0. Deasserting
 * (setting the bit) leaves the registers usable again.
 */
static void run_reset_test(void)
{
    uint32_t cfg = RF_LSH(NSS, CFG_LINK_EN, 1) | RF_BMSK_LSH(NSS, CFG_DESC, 0x123);
    int ok = 1;

    uart_puts("Reset test: writing NSS.CFG / CSS.IEN_HST markers\n");
    torq_reg_write32(RA_(NSS, CFG), cfg);
    if (torq_reg_read32(RA_(NSS, CFG)) != cfg) {
        uart_puts("Reset test: FAIL (NSS.CFG marker readback)\n");
        return;
    }
    torq_reg_write32(RA_(CSS, IEN_HST), RF_LSH(CSS, IEN_HST_NSS, 1));

    if (!(mmio_read32(CHIP_CTRL_NPU_RST_REG) & CHIP_CTRL_NPU_RST_BIT)) {
        uart_puts("Reset test: FAIL (reset line not deasserted initially)\n");
        return;
    }

    uart_puts("Reset test: asserting NPU core reset (clearing bit 1)\n");
    mmio_rmw32(CHIP_CTRL_NPU_RST_REG, CHIP_CTRL_NPU_RST_BIT, 0);
    if (mmio_read32(CHIP_CTRL_NPU_RST_REG) & CHIP_CTRL_NPU_RST_BIT) {
        uart_puts("Reset test: FAIL (bit still set after assert)\n");
        ok = 0;
    }

    /* The assert edge must have reset the C model. */
    if (torq_reg_read32(RA_(NSS, CFG)) != 0) {
        uart_puts("Reset test: FAIL (NSS.CFG not cleared by reset)\n");
        ok = 0;
    }
    if (torq_reg_read32(RA_(CSS, IEN_HST)) != 0) {
        uart_puts("Reset test: FAIL (CSS.IEN_HST not cleared by reset)\n");
        ok = 0;
    }

    uart_puts("Reset test: deasserting NPU core reset (setting bit 1)\n");
    mmio_rmw32(CHIP_CTRL_NPU_RST_REG, CHIP_CTRL_NPU_RST_BIT,
               CHIP_CTRL_NPU_RST_BIT);
    if (!(mmio_read32(CHIP_CTRL_NPU_RST_REG) & CHIP_CTRL_NPU_RST_BIT)) {
        uart_puts("Reset test: FAIL (bit not set after deassert)\n");
        ok = 0;
    }

    /* The NPU registers must be usable again after the reset pulse. */
    torq_reg_write32(RA_(NSS, CFG), cfg);
    if (torq_reg_read32(RA_(NSS, CFG)) != cfg) {
        uart_puts("Reset test: FAIL (NSS.CFG unusable after reset)\n");
        ok = 0;
    }

    if (ok) {
        uart_puts("Reset test: PASS (NPU core reset pulse works)\n");
    }
}

void main(void)
{
    uart_puts("astra_sl bare-metal hello\n");
    
    // write a minimal NPU program in RAM
    int program_size_words = 2;
    
    volatile uint32_t *lram = (volatile uint32_t *)(TORQ_BASE_ADDR + REG_ADDR__TORQ_HV_LRAM);
    
    // build a minimal program
    uint32_t npu_program[program_size_words];

    struct npu_instruction *instr = (struct npu_instruction *)npu_program;

    // CFG last = 1 | n = 1 | a = 0x100 | data = 0 (dummy value, not used for this test) 
    instr[0].hdr.cfg.lc = 1; // last command
    instr[0].hdr.cfg.id = 1; // CFG command
    instr[0].hdr.cfg.n = 1; // 1 consecutive CFG register value
    instr[0].hdr.cfg.a = 0x100; // start CFG register address -- point to DMA.
    instr[0].data[0] = 0; // bogus value

    // copy the program to LRAM at address 0
    for (int i = 0; i < program_size_words; i++) {
        lram[i] = npu_program[i];
    }

    uart_puts("Setup interrupts...\n");
    gic_init();
    torq_enable_interrupts();

    uart_puts("Starting NPU program...\n");

    // start the NPU program (which we wrote in LRAM at address 0)
    torq_hw_start(0);

    uart_puts("Waiting for NPU program to finish...\n");
    torq_hw_wait();

    uart_puts("NPU program finished.\n");
    torq_hw_end();

    run_dma_test();

    run_reset_test();

    uart_puts("astra_sl bare-metal hello done\n");

    for (;;) {
        __asm__ volatile ("wfe");
    }
}
