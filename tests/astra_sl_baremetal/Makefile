CROSS ?= aarch64-linux-gnu-
CC := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy

CFLAGS := -O2 -g -ffreestanding -fno-builtin -fno-stack-protector -Wall -Wextra
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
