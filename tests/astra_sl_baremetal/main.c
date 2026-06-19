#include <stdint.h>

#include "torq_reg_util.h"
#include "torq_regs_host_view.h"
#include "torq_nss_regs.h"
#include "torq_css_regs.h"

#define PL011_BASE 0x09000000UL
#define PL011_DR   (PL011_BASE + 0x00)
#define PL011_FR   (PL011_BASE + 0x18)
#define FR_TXFF    (1u << 5)

#define TORQ_BASE_ADDR 0x0a000000
#define TORQ_IRQ       33U

#define ASTRA_SL_GIC_DIST_BASE 0x08000000UL
#define ASTRA_SL_GIC_CPU_BASE  0x08010000UL

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
    while (mmio_read32(PL011_FR) & FR_TXFF) {
    }
    mmio_write32(PL011_DR, (uint32_t)c);
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

    uart_puts("astra_sl bare-metal hello done\n");

    for (;;) {
        __asm__ volatile ("wfe");
    }
}
