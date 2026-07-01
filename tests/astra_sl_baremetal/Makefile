CROSS ?= aarch64-linux-gnu-
CC := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy

# check CDESC_DIR is set
ifndef CDESC_DIR
$(error CDESC_DIR is not set. Please set it to the directory containing the Torq headers)
endif

CFLAGS := -O2 -g -ffreestanding -fno-builtin -fno-stack-protector -Wall -Wextra -I$(CDESC_DIR) -I$(CDESC_DIR)/reg -I$(CDESC_DIR)/ct
LDFLAGS := -nostdlib -Wl,--build-id=none -T linker.ld

all: hello.elf hello.bin

hello.elf: startup.S main.c linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) startup.S main.c -o $@

hello.bin: hello.elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -f hello.elf hello.bin

run: hello.elf
	cd ../../build && ./qemu-system-aarch64 \
		-machine astra_sl \
		-accel tcg \
		-display none \
		-monitor none \
		-serial stdio \
		-kernel ../tests/astra_sl_baremetal/hello.elf
