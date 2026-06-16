#include <stdint.h>

#define PL011_BASE 0x09000000UL
#define PL011_DR   (PL011_BASE + 0x00)
#define PL011_FR   (PL011_BASE + 0x18)
#define FR_TXFF    (1u << 5)

static inline void mmio_write32(uintptr_t addr, uint32_t value)
{
    *(volatile uint32_t *)addr = value;
}

static inline uint32_t mmio_read32(uintptr_t addr)
{
    return *(volatile uint32_t *)addr;
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

void main(void)
{
    uart_puts("astra_sl bare-metal hello from EL1\n");

    for (;;) {
        __asm__ volatile ("wfe");
    }
}
