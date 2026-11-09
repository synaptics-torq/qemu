CROSS ?= aarch64-linux-gnu-
CC := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
QEMU_BUILD_DIR ?= $(abspath ../../build)

# check CDESC_DIR is set
ifndef CDESC_DIR
$(error CDESC_DIR is not set. Please set it to the directory containing the Torq headers)
endif

CFLAGS := -O2 -g -ffreestanding -fno-builtin -fno-stack-protector -mgeneral-regs-only -Wall -Wextra -I$(CDESC_DIR) -I$(CDESC_DIR)/reg -I$(CDESC_DIR)/ct
LDFLAGS := -nostdlib -Wl,--build-id=none -T linker.ld

all: hello.elf hello.bin

hello.elf: gen startup.S main.c linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) startup.S main.c -o $@

hello.bin: hello.elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -f hello.elf hello.bin

run:
	QEMU_BUILD_DIR="$(QEMU_BUILD_DIR)" ./run.sh "$(CDESC_DIR)"

.PHONY: gen clean run

# Regenerate DMA_NPU_PROGRAM in main.c via the CT descriptor API (host tool).
gen:
	$(MAKE) -C gen run
