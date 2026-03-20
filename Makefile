obj-m += gvsoc_vfio_kernel_module.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

MODULE := gvsoc_vfio_kernel_module
DMA_TEST := dma_test
LOADER := elf_loader

ccflags-y += -Wall -O2

all: module dma_test elf_loader

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

dma_test: dma_test.c
	$(CC) -O2 -Wall -Wextra dma_test.c -o $(DMA_TEST)

elf_loader: elf_loader.c
	$(CC) -O2 -Wall -Wextra elf_loader.c -o $(LOADER)

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f $(DMA_TEST) $(LOADER)

load: module
	sudo insmod $(MODULE).ko

unload:
	-sudo rmmod $(MODULE)

reload: unload load
