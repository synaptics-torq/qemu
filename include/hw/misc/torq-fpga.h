#ifndef HW_MISC_TORQ_FPGA_H
#define HW_MISC_TORQ_FPGA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct TorqFpga TorqFpga;

TorqFpga *torq_fpga_open(void);
void torq_fpga_close(TorqFpga *fpga);
int torq_fpga_read32(TorqFpga *fpga, uint32_t address, uint32_t *value);
int torq_fpga_write32(TorqFpga *fpga, uint32_t address, uint32_t value);
int torq_fpga_memory(TorqFpga *fpga, uint32_t address, size_t size,
                      uint8_t *data, bool writing);
int torq_fpga_get_irq(TorqFpga *fpga, uint32_t *irq);
bool torq_fpga_busy(TorqFpga *fpga);
int torq_fpga_reset(TorqFpga *fpga);

#endif
