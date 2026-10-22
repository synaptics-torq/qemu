# Host tool: regenerates DMA_NPU_PROGRAM / DMA_XRAM_SRC / DMA_LRAM_DST in
# ../main.c using the CT descriptor API (synpu-hw/ct/desc_gen).
SYNPU ?= ../../../../synpu-hw
REG   ?= $(SYNPU)/common/reg/250321

gen_dma_program: gen_dma_program.c stubs/direct.h stubs/io.h
	gcc -O2 -o $@ gen_dma_program.c $(SYNPU)/ct/desc_gen/src/torq_api.c \
		-I stubs -I $(SYNPU)/ct/desc_gen/inc -I $(SYNPU)/ct/desc_gen/src \
		-I $(REG) -fno-strict-aliasing -w

.PHONY: run
run: gen_dma_program
	./gen_dma_program > dma_program.h
